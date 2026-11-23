#include "../TVTDataBroadcastingWV2/pch.h"
#include "../TVTDataBroadcastingWV2/B62AppReceiver.h"
#include <array>
#include <iostream>
#include <stdexcept>
using Bytes = std::vector<uint8_t>;
uint32_t Crc(const Bytes& bytes) {
    uint32_t crc = 0xffffffff;
    for (auto b : bytes) {
        crc ^= uint32_t(b) << 24;
        for (int i = 0; i < 8; ++i) crc = crc & 0x80000000 ? (crc << 1) ^ 0x04c11db7 : crc << 1;
    }
    return crc;
}
Bytes Section(uint8_t table, uint16_t ext, uint8_t num, uint8_t last, Bytes body, uint8_t version = 0, bool current = true) {
    Bytes result{table, 0xb0, 0, uint8_t(ext >> 8), uint8_t(ext), uint8_t(0xc0 | (version << 1) | (current ? 1 : 0)), num, last};
    result.insert(result.end(),body.begin(),body.end());
    const auto length = result.size() + 4 - 3;
    result[1] |= uint8_t(length >> 8); result[2] = uint8_t(length);
    const auto crc = Crc(result);
    for (int shift = 24; shift >= 0; shift -= 8) result.push_back(uint8_t(crc >> shift));
    if (Crc(result)) throw std::runtime_error("Invalid fixture CRC");
    return result;
}
void Feed(B62::AppReceiver& receiver, uint16_t pid, uint8_t cc, const Bytes& section) {
    if (section.size() > 183) throw std::runtime_error("Fixture too long");
    std::array<uint8_t,188> packet; packet.fill(0xff);
    packet[0] = 0x47; packet[1] = 0x40 | uint8_t(pid >> 8); packet[2] = uint8_t(pid);
    packet[3] = 0x10 | cc; packet[4] = 0;
    std::copy(section.begin(), section.end(), packet.begin() + 5);
    receiver.FeedPacket(packet.data());
}
Bytes Ait(uint8_t number, uint16_t id, uint8_t control, uint8_t version = 0) {
    std::string base="http://example.test/", path="index.html";
    Bytes descriptors{0x80,0x2a,uint8_t(4+base.size()),0,3,0,uint8_t(base.size())};
    descriptors.insert(descriptors.end(),base.begin(),base.end());
    descriptors.insert(descriptors.end(),{0x80,0x2b,uint8_t(path.size())});
    descriptors.insert(descriptors.end(),path.begin(),path.end());
    Bytes app{0,0,0,1,uint8_t(id>>8),uint8_t(id),control,0xf0,uint8_t(descriptors.size())};
    app.insert(app.end(),descriptors.begin(),descriptors.end());
    Bytes body{0xf0,0,0xf0,uint8_t(app.size())};body.insert(body.end(),app.begin(),app.end());
    return Section(0x9c,0x11,number,1,body,version);
}

static void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void Initialize(B62::AppReceiver& receiver) {
    receiver.Reset(1);
    Feed(receiver,0,0,Section(0,1,0,0,{0,1,0xe1,0}));
    Feed(receiver,0x100,0,Section(2,1,0,0,{0xe1,1,0xf0,0,5,0xe1,1,0xf0,6,5,4,'D','4','K','A'}));
}
int main() {
    try {
        B62::AppReceiver receiver; Initialize(receiver);
        uint8_t cc=0;
        auto feed=[&](const Bytes& bytes) { Feed(receiver,0x101,cc++ & 15,bytes); };
        feed(Ait(0,1,1));
        Check(!receiver.GetApplication(), "An incomplete MH-AIT was published");
        feed(Ait(1,2,2));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 1, "A PRESENT section removed the AUTOSTART app");
        feed(Ait(0,1,1)); feed(Ait(1,2,2));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 1, "Repeats removed the application");
        // Receive a replacement out of order, interleaved with the old table.
        feed(Ait(1,2,2,1)); feed(Ait(0,1,1));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 1, "A partial replacement removed the old app");
        feed(Ait(0,3,1,1));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 3, "Out-of-order replacement failed");
        feed(Ait(0,1,1)); feed(Ait(1,2,2));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 3, "Old repeats reverted the app");
        feed(Section(0x9c,0x11,0,0,{0xf0,0,0xf0,0},2,false));
        Check(receiver.GetApplication().has_value(), "Future signalling was applied");
        feed(Section(0x9c,0x11,0,0,{0xf0,0,0xf0,9},2));
        Check(receiver.GetApplication().has_value(), "Malformed signalling removed the app");
        feed(Section(0x9c,0x11,0,0,{0xf0,0,0xf0,0},2));
        Check(!receiver.GetApplication(), "A complete empty table did not stop the app");
        // Missing many updates must not freeze signalling for half a version cycle.
        feed(Ait(0,6,1,20)); feed(Ait(1,2,2,20));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 6, "A version jump after lost reception was ignored");
        Initialize(receiver); cc=0;
        feed(Ait(0,4,1,31)); feed(Ait(1,2,2,31));
        feed(Ait(1,2,2,0)); feed(Ait(0,5,1,0));
        Check(receiver.GetApplication() && receiver.GetApplication()->applicationId == 5, "Version wrap failed");
        Initialize(receiver); cc=0; feed(Ait(1,2,2,0));
        Check(!receiver.GetApplication(), "Reset retained incomplete table state");
        std::cout << "B62 multi-section MH-AIT lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
