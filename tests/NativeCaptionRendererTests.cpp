#include "../TVTDataBroadcastingWV2/pch.h"
#include <map>
#include <array>
#include <deque>
#include <optional>
#include <functional>
#include <iostream>
#include <set>
#include <string_view>

// Inspect retained state, but exercise the product's decoder, timeline and
// DirectWrite renderer rather than a mock caption backend.
#define private public
#include <aribcaption/aribcaption.hpp>
#include "renderer/renderer_impl.hpp"
#include "../TVTDataBroadcastingWV2/NativeCaptionRenderer.h"
#undef private
#include "../TVTDataBroadcastingWV2/NativeCaptionRenderer.cpp"

std::string wstrToUTF8String(const wchar_t* text)
{
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string value(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, value.data(), count, nullptr, nullptr);
    value.resize(count - 1);
    return value;
}
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// A complete caption statement containing CS, MSZ, LS1, and an ASCII glyph.
// It requires no optional language font packs on the Windows CI host.
std::vector<uint8_t> Statement(bool superimpose = false, char glyph = 'A')
{
    return {static_cast<uint8_t>(superimpose ? 0x81 : 0x80), 0xff, 0xf0,
        0x04, 0, 0, 0, 13, 0x3f, 0, 0, 9, 0x1f, 0x20, 0, 0, 4,
        0x0c, 0x89, 0x0e, static_cast<uint8_t>(glyph), 0, 0};
}
auto& Captions(NativeCaptionRenderer& renderer, int track = 0)
{
    return renderer.impl_->tracks[track].renderer->pimpl_->captions_;
}
void CheckHiddenStorage(NativeCaptionRenderer& renderer)
{
    renderer.Reset(); renderer.SetEnabled(false);
    for (int i = 1; i <= 1000; ++i) {
        renderer.Push(0xbd, Statement(), static_cast<int64_t>(i) * 90000);
        renderer.Update(i * 1000, 960, 540);
    }
    Check(Captions(renderer).size() <= MAX_STORED_CAPTIONS, "hidden caption history must stay bounded");
    Check(Captions(renderer).rbegin()->first == 1000000, "hidden track must keep recent captions");
    renderer.SetEnabled(true); renderer.Update(1000000, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "re-enabling captions must show the current decoded caption");
    renderer.Reset();
    for (int i = 0; i < 1000; ++i) renderer.Push(0xbf, Statement(true), std::nullopt);
    Check(renderer.impl_->tracks[1].pending.size() <= MAX_STORED_CAPTIONS, "PES received before PCR must also have a bound");
    NativeCaptionSettings settings; settings.delayTime = 0; settings.showSuperimpose = false;
    renderer.SetSettings(settings); renderer.Update(0, 960, 540);
    for (int i = 1; i <= 1000; ++i) {
        renderer.Update(i * 1000, 960, 540);
        renderer.Push(0xbf, Statement(true), std::nullopt);
    }
    Check(Captions(renderer, 1).size() <= MAX_STORED_CAPTIONS, "disabled superimposition history must stay bounded");
}
void CheckResetAndArrivalOrder(NativeCaptionRenderer& renderer)
{
    renderer.Update(100000, 960, 540); renderer.Reset();
    renderer.Push(0xbf, Statement(true), std::nullopt);
    Check(!renderer.impl_->lastPCR90kHz && Captions(renderer, 1).empty(), "reset must not reuse the previous service clock");
    renderer.Update(1000, 960, 540);
    Check(Captions(renderer, 1).begin()->first == 1000, "PTS-less superimposition waits for the new PCR");
    Check(renderer.impl_->tracks[1].visible, "new-service superimposition must be visible on its own clock");
    renderer.Reset();
    renderer.Push(0xbd, Statement(), 1100 * 90);
    renderer.Update(1000, 960, 540);
    Check(!renderer.impl_->tracks[0].visible, "future caption received before PCR must wait for its PTS");
    renderer.Update(1100, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "buffered caption must become visible at its PTS");
    renderer.Update(50000, 960, 540);
    Check(Captions(renderer).empty() && !renderer.impl_->tracks[0].visible, "a large forward seek must discard the old timeline");
    renderer.Push(0xbd, Statement(), 50000 * 90); renderer.Update(50000, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "captions resume after a seek");
    renderer.Update(1000, 960, 540);
    Check(Captions(renderer).empty(), "a backward seek must discard future captions");
}
void CheckWrap(NativeCaptionRenderer& renderer)
{
    renderer.Reset();
    const auto period = (INT64_C(1) << 33);
    renderer.Update(period / 90 - 200, 960, 540);
    renderer.Push(0xbd, Statement(), 50 * 90);
    renderer.Update(period / 90 - 100, 960, 540);
    Check(!renderer.impl_->tracks[0].visible, "post-wrap caption must not appear before wrap");
    renderer.Update(0, 960, 540);
    Check(!renderer.impl_->tracks[0].visible, "post-wrap caption must still wait at raw PCR zero");
    renderer.Update(50, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "post-wrap caption must appear at its PTS");
    Check(renderer.impl_->currentTime90kHz >= period, "PCR wrap must extend the continuous clock");
    renderer.Reset();
    renderer.Update(period / 90 - 100, 960, 540);
    renderer.Push(0xbd, Statement(), period - 18000);
    renderer.Update(period / 90 - 50, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "pre-wrap caption must be visible before wrap");
    renderer.Update(100, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "an indefinite caption must remain visible through PCR wrap");
    NativeCaptionSettings delayed; delayed.delayTime = 450; renderer.SetSettings(delayed);
    renderer.Reset(); renderer.Update(period / 90 - 200, 960, 540);
    renderer.Push(0xbd, Statement(), 50 * 90);
    renderer.Update(499, 960, 540);
    Check(!renderer.impl_->tracks[0].visible, "caption delay must cross wrap without showing early");
    renderer.Update(500, 960, 540);
    Check(renderer.impl_->tracks[0].visible, "caption delay must expire on the continuous clock");
}
void CheckFonts(NativeCaptionRenderer& renderer)
{
    NativeCaptionSettings settings; settings.faceName = L"Arial"; renderer.SetSettings(settings);
    auto& backend = *renderer.impl_->tracks[0].renderer->pimpl_;
    const auto japanese = backend.language_font_family_[aribcaption::ThreeCC("jpn")];
    Check(backend.force_default_font_family_ && backend.language_font_family_[0][0] == "Arial", "explicit font must be forced");
    settings.faceName.clear(); renderer.SetSettings(settings);
    Check(!backend.force_default_font_family_, "clearing font fields must restore automatic selection");
    Check(backend.language_font_family_[aribcaption::ThreeCC("jpn")] == japanese, "automatic language font families must be preserved");
    Check(backend.language_font_family_[0][0] != "Arial", "previous forced fallback must not survive clearing");
}
int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        NativeCaptionRenderer renderer([](const nlohmann::json&) {});
        Check(renderer.Initialize(), "DirectWrite caption renderer must initialize");
        NativeCaptionSettings settings; settings.delayTime = 0; settings.faceName = L"Arial";
        renderer.SetSettings(settings);
        CheckHiddenStorage(renderer);
        settings.showSuperimpose = true; renderer.SetSettings(settings); renderer.SetEnabled(true);
        CheckResetAndArrivalOrder(renderer); CheckWrap(renderer); CheckFonts(renderer);
        std::cout << "Native B24 storage, reset, PCR/PTS wrap, seek and font tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; if (SUCCEEDED(com)) CoUninitialize(); return 1;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
