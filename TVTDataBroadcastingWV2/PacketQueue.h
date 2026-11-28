#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The default mutex is used by the plugin. Tests can supply a mutex that
// controls scheduling to exercise clear() racing a TS packet deterministically.
template<typename Mutex = std::mutex>
class BasicPacketQueue
{
private:
    mutable Mutex queueMutex;
    std::vector<BYTE> currentBlock;
    std::queue<std::vector<BYTE>> queue;
    std::unordered_set<WORD> pidsToExclude;
    std::unordered_map<WORD, int> pcrPIDCandidates;
    int pcrPID = -1;
    DWORD pcr = 0;
    bool pcrKnown = false;
    DWORD lastBlockPCR = 0;
    // Broadcast wall-clock from TDT/TOT (PID 0x0014), with the PCR at that moment
    // for interpolation. totUnix==0 means unknown.
    long long totUnix = 0;
    DWORD pcrAtTot = 0;
public:
    static constexpr size_t packetSize = 188;
    static constexpr size_t packetBlockSize = packetSize * 500;
    static constexpr size_t maxQueueLength = 100;
    // PCR units (45 kHz, LSB dropped). A larger step between consecutive PCRs is
    // a seek or discontinuity, not elapsed time (PCRs repeat at least every 100 ms).
    static constexpr int32_t maxPCRStep = 45000 * 10;

    BasicPacketQueue()
    {
        currentBlock.reserve(packetBlockSize);
    }

    // TSスレッドでのみ呼び出せる
    bool enqueuePacket(BYTE* packet)
    {
        // Serialize the whole packet with clear(), including its clock update
        // and any block publication. No old partial block can survive a reset.
        std::lock_guard<Mutex> lock(this->queueMutex);
        // 8-bit sync byte
        // 1-bit TEI
        // 1-bit PUSI
        // 1-bit priority
        // 13-bit PID
        bool transportErrorIndicator = !!(packet[1] & 0x80);
        WORD pid = ((packet[1] << 8) | packet[2]) & 0x1fff;
        int adaptationFieldControl = (packet[3] >> 4) & 0x03;
        bool pcrFlag = false;
        if (!transportErrorIndicator && !!(adaptationFieldControl & 2))
        {
            int adaptationLength = packet[4];
            // The flags byte plus all six PCR bytes must belong to the field.
            pcrFlag = adaptationLength >= 7 && adaptationLength <= 183 && !!(packet[5] & 0x10);
            if (pcrFlag)
            {
                // 参照するPCRのPIDを適当に選ぶ
                if (pid != this->pcrPID)
                {
                    auto it = this->pcrPIDCandidates.find(pid);
                    if (it == this->pcrPIDCandidates.end())
                    {
                        it = this->pcrPIDCandidates.emplace(pid, 0).first;
                    }
                    // 最初に3回出現するか、参照済みPCRが現れずに5回出現したPCRを使う
                    it->second++;
                    if ((this->pcrPID < 0 && it->second >= 3) || it->second >= 5)
                    {
                        this->pcrPID = pid;
                    }
                }
                if (pid == this->pcrPID)
                {
                    // PCRを取得する。時計演算の便利のため下位1bitは捨てる
                    this->pcrPIDCandidates.clear();
                    const DWORD previous = this->pcr;
                    this->pcr = (static_cast<DWORD>(packet[6]) << 24) | (packet[7] << 16) | (packet[8] << 8) | packet[9];
                    // A TDT/TOT may arrive before the first usable PCR.
                    if (!this->pcrKnown) this->pcrAtTot = this->pcr;
                    else if (const auto step = static_cast<int32_t>(this->pcr - previous); step > maxPCRStep || step < -maxPCRStep)
                    {
                        // Do not extrapolate the old TOT across a seek (up to ±13 hours
                        // wrong until the next TOT); report the time as unknown instead.
                        this->totUnix = 0;
                        this->pcrAtTot = this->pcr;
                    }
                    this->pcrKnown = true;
                }
            }
        }

        // TDT/TOT (PID 0x0014): broadcast wall-clock time.
        if (pid == 0x0014 && !transportErrorIndicator && !!(adaptationFieldControl & 1))
        {
            bool pusi = !!(packet[1] & 0x40);
            if (pusi)
            {
                int off = 4;
                if (adaptationFieldControl & 2) off += 1 + packet[4]; // skip adaptation field
                if (off < static_cast<int>(packetSize))
                {
                    off += 1 + packet[off]; // pointer_field
                    if (off + 8 <= static_cast<int>(packetSize))
                    {
                        BYTE tableId = packet[off];
                        if (tableId == 0x70 || tableId == 0x73) // TDT / TOT
                        {
                            const BYTE* t = packet + off + 3; // skip table_id + section_length(2)
                            int mjd = (t[0] << 8) | t[1];
                            int hh = (t[2] >> 4) * 10 + (t[2] & 0xf);
                            int mm = (t[3] >> 4) * 10 + (t[3] & 0xf);
                            int ss = (t[4] >> 4) * 10 + (t[4] & 0xf);
                            if (mjd > 40587 && hh < 24 && mm < 60 && ss < 60)
                            {
                                // MJD 40587 = 1970-01-01. ARIB time is JST(UTC+9).
                                long long unixT = static_cast<long long>(mjd - 40587) * 86400
                                                + hh * 3600 + mm * 60 + ss - 32400;
                                this->totUnix = unixT;
                                this->pcrAtTot = this->pcr;
                            }
                        }
                    }
                }
            }
        }

        const bool acceptPacket = this->pidsToExclude.count(pid) == 0;
        if (acceptPacket)
        {
            this->currentBlock.insert(this->currentBlock.end(), packet, packet + this->packetSize);
        }
        else if (pcrFlag)
        {
            // 除外されているPIDにPCRが含まれていればPCRのみをキューに加える
            BYTE pcr_packet[packetSize] = {};
            pcr_packet[0] = 0x47;
            // adaptation_field_control=0b10なのでCIは加算されない、0固定で間に合わせる
            pcr_packet[1] = pid >> 8;
            pcr_packet[2] = pid & 0xff;
            pcr_packet[3] = 2 << 4;
            pcr_packet[4] = packetSize - 5; // adaptation_field_length
            pcr_packet[5] = 0x10; // PCR
            memcpy(pcr_packet + 6, packet + 6, 6);
            memset(pcr_packet + 12, 0xff, sizeof(pcr_packet) - 12);
            this->currentBlock.insert(this->currentBlock.end(), pcr_packet, pcr_packet + sizeof(pcr_packet));
        }

        // PCRが100ミリ秒以上進めばキューに加える
        // キューには100*maxQueueLengthミリ秒分ほど貯められる
        if (this->currentBlock.size() >= this->packetBlockSize ||
            (!this->currentBlock.empty() && (this->pcr - this->lastBlockPCR) >= 45 * 100))
        {
            this->queue.push(std::move(this->currentBlock));
            if (this->queue.size() > this->maxQueueLength)
            {
                // 古いものを中身再利用して捨てる
                this->currentBlock.swap(this->queue.front());
                this->queue.pop();
            }
            this->currentBlock.clear();
            this->currentBlock.reserve(this->packetBlockSize);
            this->lastBlockPCR = this->pcr;
            return true;
        }
        return false;
    }

    // どのスレッドからも呼び出せる
    void clear()
    {
        std::lock_guard<Mutex> lock(this->queueMutex);
        std::queue<std::vector<BYTE>>().swap(this->queue);
        this->currentBlock.clear();
        this->pcrPIDCandidates.clear();
        this->pcrPID = -1;
        this->pcr = 0;
        this->pcrKnown = false;
        this->lastBlockPCR = 0;
        this->totUnix = 0;
        this->pcrAtTot = 0;
    }

    // どのスレッドからも呼び出せる
    std::optional<std::vector<BYTE>> pop()
    {
        std::lock_guard<Mutex> lock(this->queueMutex);
        if (this->queue.empty())
        {
            return std::nullopt;
        }
        auto r = std::move(this->queue.front());
        this->queue.pop();
        return r;
    }

    // どのスレッドからも呼び出せる
    void setPIDsToExclude(std::unordered_set<WORD> pids)
    {
        std::lock_guard<Mutex> lock(this->queueMutex);
        this->pidsToExclude.swap(pids);
    }

    // Current broadcast Unix time (JST converted to UTC seconds), interpolated
    // from the last TOT with the PCR delta. Returns 0 if unknown.
    // Callable from any thread.
    long long getBroadcastTime() const
    {
        std::lock_guard<Mutex> lock(this->queueMutex);
        long long base = this->totUnix;
        if (base == 0) return 0;
        // pcr is the upper 32 bits of the 33-bit PCR base (LSB dropped) => 45kHz.
        DWORD d = this->pcr - this->pcrAtTot;
        return base + static_cast<long long>(static_cast<int32_t>(d)) / 45000;
    }
};

using PacketQueue = BasicPacketQueue<>;
