#pragma once
#include <cstdint>
#include <vector>

// 4Kデータ放送のアプリケーションの受信機内蔵音 (<audio src="romsound://7"> など)。
// 音はB62Shim.hがplay()を横取りしてプラグインのページで鳴らす。要素自体の読み込みには
// この短い無音を返し、読み込みの失敗 (errorイベント、readyStateが0のまま) で
// アプリケーションが音を鳴らすのをやめないようにする。
namespace B62RomSound {

// 8kHz・モノラル・16ビットで10ミリ秒の無音のWAV
inline std::vector<uint8_t> SilentWav()
{
    constexpr uint32_t sampleRate = 8000;
    constexpr uint16_t channels = 1;
    constexpr uint16_t bitsPerSample = 16;
    constexpr uint32_t dataSize = sampleRate / 100 * channels * (bitsPerSample / 8);
    std::vector<uint8_t> wav;
    auto text = [&](const char* s) { wav.insert(wav.end(), s, s + 4); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) wav.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { wav.push_back(static_cast<uint8_t>(v)); wav.push_back(static_cast<uint8_t>(v >> 8)); };
    text("RIFF");
    u32(36 + dataSize);
    text("WAVE");
    text("fmt ");
    u32(16);
    u16(1); // PCM
    u16(channels);
    u32(sampleRate);
    u32(sampleRate * channels * (bitsPerSample / 8));
    u16(channels * (bitsPerSample / 8));
    u16(bitsPerSample);
    text("data");
    u32(dataSize);
    wav.resize(wav.size() + dataSize, 0);
    return wav;
}

} // namespace B62RomSound
