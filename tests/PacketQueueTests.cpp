#include "../TVTDataBroadcastingWV2/PacketQueue.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>

using Packet = std::array<BYTE, PacketQueue::packetSize>;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Packet Payload(WORD pid = 0x30, BYTE marker = 0xa5)
{
    Packet p; p.fill(marker);
    p[0] = 0x47; p[1] = static_cast<BYTE>(pid >> 8); p[2] = static_cast<BYTE>(pid); p[3] = 0x10;
    return p;
}
Packet PCR(DWORD value, WORD pid = 0x100)
{
    auto p = Payload(pid, 0xff);
    p[3] = 0x20; p[4] = 183; p[5] = 0x10;
    p[6] = static_cast<BYTE>(value >> 24); p[7] = static_cast<BYTE>(value >> 16);
    p[8] = static_cast<BYTE>(value >> 8); p[9] = static_cast<BYTE>(value); p[10] = 0x7e; p[11] = 0;
    return p;
}
Packet TDT(BYTE second = 0)
{
    auto p = Payload(0x14, 0xff);
    p[1] |= 0x40; p[4] = 0; p[5] = 0x70; p[6] = 0x70; p[7] = 5;
    p[8] = 60000 >> 8; p[9] = 60000 & 255; p[10] = 0x12; p[11] = 0; p[12] = second;
    return p;
}
void CheckClockReset()
{
    PacketQueue queue;
    auto p = PCR(4500000), t = TDT();
    for (int i = 0; i < 3; ++i) queue.enqueuePacket(p.data());
    queue.enqueuePacket(t.data());
    const auto base = queue.getBroadcastTime();
    Check(base == 1677294000, "TDT must be converted from JST to UTC");
    p = PCR(4545000); queue.enqueuePacket(p.data());
    Check(queue.getBroadcastTime() == base + 1, "PCR must interpolate the broadcast clock");
    queue.clear();
    Check(queue.getBroadcastTime() == 0 && !queue.pop(), "clear must invalidate queued data and broadcast time");
    // Different PCR PID, without a TOT: no old timestamp may be extrapolated.
    p = PCR(9000000, 0x200);
    for (int i = 0; i < 3; ++i) queue.enqueuePacket(p.data());
    Check(queue.getBroadcastTime() == 0, "new PCR alone must not revive an old TOT");
    t = TDT(0x10); queue.enqueuePacket(t.data());
    p = PCR(9045000, 0x200); queue.enqueuePacket(p.data());
    Check(queue.getBroadcastTime() == base + 11, "new PCR PID must be selected without old candidates");
    queue.clear();
    queue.enqueuePacket(t.data());
    p = PCR(123450000, 0x300);
    for (int i = 0; i < 3; ++i) queue.enqueuePacket(p.data());
    Check(queue.getBroadcastTime() == base + 10, "TOT before PCR must anchor at the first usable PCR");
    p = PCR(123495000, 0x300); queue.enqueuePacket(p.data());
    Check(queue.getBroadcastTime() == base + 11, "interpolation must resume after anchoring");
    queue.clear();
    p = PCR(0xffff8000); for (int i = 0; i < 3; ++i) queue.enqueuePacket(p.data());
    t = TDT(); queue.enqueuePacket(t.data());
    p = PCR(static_cast<DWORD>(0xffff8000ULL + 45000ULL)); queue.enqueuePacket(p.data());
    Check(queue.getBroadcastTime() == base + 1, "PCR wrap must advance the clock by one second");
}
void CheckQueueBound()
{
    PacketQueue queue;
    for (size_t block = 0; block <= PacketQueue::maxQueueLength; ++block)
    {
        auto p = Payload(0x30, static_cast<BYTE>(block));
        for (int i = 0; i < 500; ++i) queue.enqueuePacket(p.data());
    }
    for (size_t block = 1; block <= PacketQueue::maxQueueLength; ++block)
    {
        const auto p = queue.pop();
        Check(p && p->size() == PacketQueue::packetBlockSize && (*p)[4] == block,
            "queue overflow must discard the oldest complete block and preserve FIFO order");
    }
    Check(!queue.pop(), "queue must stay within its maximum block count");
}
void CheckPCRFiltering()
{
    for (BYTE length : {0, 1, 6, 184, 255})
    {
        PacketQueue queue; queue.setPIDsToExclude({0x100});
        auto p = PCR(4500000); p[3] = 0x30; p[4] = length;
        for (int i = 0; i < 3; ++i) queue.enqueuePacket(p.data());
        p = Payload();
        for (int i = 0; i < 500; ++i) queue.enqueuePacket(p.data());
        auto block = queue.pop();
        Check(block && block->size() == PacketQueue::packetBlockSize, "ordinary packets must still be delivered");
        for (size_t i = 0; i < block->size(); i += 188)
            Check((*block)[i + 2] == 0x30, "missing or truncated adaptation field must not synthesize a PCR");
    }
    PacketQueue queue; queue.setPIDsToExclude({0x100});
    auto p = PCR(4500000); p[4] = 7;
    for (int i = 0; i < 3; ++i) queue.enqueuePacket(p.data());
    auto block = queue.pop();
    Check(block && block->size() == 3 * 188, "a minimum-length valid PCR must survive PID filtering");
    Check((*block)[3] == 0x20 && (*block)[4] == 183 && (*block)[5] == 0x10 &&
        std::memcmp(block->data() + 6, p.data() + 6, 6) == 0, "PCR-only packet must retain the PCR bytes");
    queue.clear();
    p = Payload(0x100);
    for (int i = 0; i < 500; ++i) queue.enqueuePacket(p.data());
    Check(!queue.pop(), "reset must retain the configured PID exclusions");
}

// Pause the TS producer while it owns its first queue lock. On release, make
// clear() complete before the producer can proceed beyond that unlock. A split
// critical section would let the old 499-packet partial block be published next.
struct ScheduledMutex
{
    std::mutex inner;
    static inline std::mutex gate;
    static inline std::condition_variable changed;
    static inline bool entered = false, release = false, cleared = false;
    static inline thread_local bool producer = false, pauseUnlock = false;
    void lock()
    {
        inner.lock();
        if (producer)
        {
            producer = false; pauseUnlock = true;
            std::unique_lock<std::mutex> hold(gate);
            entered = true; changed.notify_all();
            Check(changed.wait_for(hold, std::chrono::seconds(5), [] { return release; }), "producer release timed out");
        }
    }
    void unlock()
    {
        inner.unlock();
        if (pauseUnlock)
        {
            pauseUnlock = false;
            std::unique_lock<std::mutex> hold(gate);
            Check(changed.wait_for(hold, std::chrono::seconds(5), [] { return cleared; }), "clear timed out");
        }
    }
};
void CheckConcurrentClear()
{
    BasicPacketQueue<ScheduledMutex> queue;
    auto old = Payload();
    for (int i = 0; i < 499; ++i) queue.enqueuePacket(old.data());
    std::thread producer([&] { ScheduledMutex::producer = true; queue.enqueuePacket(old.data()); });
    {
        std::unique_lock<std::mutex> hold(ScheduledMutex::gate);
        Check(ScheduledMutex::changed.wait_for(hold, std::chrono::seconds(5), [] { return ScheduledMutex::entered; }), "producer did not enter");
    }
    std::thread clearer([&] {
        queue.clear();
        std::lock_guard<std::mutex> hold(ScheduledMutex::gate);
        ScheduledMutex::cleared = true; ScheduledMutex::changed.notify_all();
    });
    {
        std::lock_guard<std::mutex> hold(ScheduledMutex::gate);
        ScheduledMutex::release = true; ScheduledMutex::changed.notify_all();
    }
    producer.join(); clearer.join();
    Check(!queue.pop(), "old-service block must not be published after clear");
    auto fresh = Payload(0x31);
    for (int i = 0; i < 500; ++i) queue.enqueuePacket(fresh.data());
    auto block = queue.pop();
    Check(block && block->size() == PacketQueue::packetBlockSize, "fresh block must contain 500 new packets");
    for (size_t i = 0; i < block->size(); i += 188)
        Check((*block)[i + 2] == 0x31, "fresh block must not contain an old partial block");
}
int main()
{
    try {
        CheckClockReset(); CheckPCRFiltering(); CheckConcurrentClear(); CheckQueueBound();
        std::cout << "Packet queue clock reset, PCR bounds and concurrent clear tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
