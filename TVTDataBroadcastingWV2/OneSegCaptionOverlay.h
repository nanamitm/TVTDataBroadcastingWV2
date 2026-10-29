#pragma once

#include <array>
#include <aribcaption/aribcaption.hpp>

// 下端に寄せた字幕/文字スーパーを、乗算済みBGRAの透過フレームへ合成する。
void ComposeOneSegCaptionFrame(unsigned char* pixels, int width, int height,
    const std::array<aribcaption::RenderResult, 2>& frames);

// TVCaption3と同じ、映像側のトップレベルウインドウをオーナーにした透過ウインドウ。
// 入力は透過し、字幕のためにフォーカスを奪わない。
class OneSegCaptionOverlay
{
public:
    ~OneSegCaptionOverlay() { Clear(); }
    void Clear();
    // ウインドウの新規作成・再表示・サイズ変更で再描画が必要ならtrue。
    bool SetBounds(HWND parent, const RECT& bounds);
    bool Draw(int track, int width, int height, const aribcaption::RenderResult& result);

private:
    HWND window_ = nullptr;
    HWND parent_ = nullptr;
    RECT bounds_ = {};
    bool needsRedraw_ = true;
    std::array<aribcaption::RenderResult, 2> frames_;
};
