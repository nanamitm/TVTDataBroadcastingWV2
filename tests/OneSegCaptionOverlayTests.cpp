#define NOMINMAX
#include <windows.h>
#include "OneSegCaptionOverlay.h"
#include <cassert>
#include <cstdio>
#include <vector>

static aribcaption::Image Image(int x, int y, int w, int h, unsigned char r, unsigned char g,
    unsigned char b, unsigned char a)
{
    aribcaption::Image image;
    image.dst_x = x; image.dst_y = y; image.width = w; image.height = h;
    image.stride = w * 4 + 8; // 行末のパディングも扱う。
    image.bitmap.resize(image.stride * h, 0xcc);
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
        {
            auto* p = image.bitmap.data() + row * image.stride + col * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = a;
        }
    return image;
}

int main()
{
    std::array<aribcaption::RenderResult, 2> frames;
    std::vector<unsigned char> pixels(8 * 8 * 4);
    auto pixel = [&](int x, int y, int c) { return pixels[(y * 8 + x) * 4 + c]; };
    frames[0].images.push_back(Image(1, -4, 2, 2, 255, 255, 0, 255));
    frames[0].images.push_back(Image(1, 0, 2, 2, 255, 255, 0, 255));
    ComposeOneSegCaptionFrame(pixels.data(), 8, 8, frames);
    assert(pixel(1, 2, 3) == 255 && pixel(1, 6, 3) == 255); // 2行をまとめて下端へ。
    assert(pixel(1, 0, 3) == 0 && pixel(4, 7, 3) == 0);
    assert(pixel(1, 7, 0) == 0 && pixel(1, 7, 1) == 255 && pixel(1, 7, 2) == 255);
    frames[1].images.push_back(Image(1, 0, 2, 2, 0, 0, 255, 128));
    ComposeOneSegCaptionFrame(pixels.data(), 8, 8, frames);
    assert(pixel(1, 7, 0) == 128 && pixel(1, 7, 1) == 127 && pixel(1, 7, 3) == 255);
    frames[0].images.clear();
    ComposeOneSegCaptionFrame(pixels.data(), 8, 8, frames);
    assert(pixel(1, 7, 0) == 128 && pixel(1, 7, 2) == 0 && pixel(1, 7, 3) == 128);
    frames[1].images = {Image(-1, -4, 12, 12, 255, 0, 0, 255)};
    ComposeOneSegCaptionFrame(pixels.data(), 8, 8, frames);
    assert(pixel(0, 0, 2) == 255 && pixel(7, 7, 2) == 255); // 範囲外は切り取る。
    frames = {};
    ComposeOneSegCaptionFrame(pixels.data(), 8, 8, frames);
    for (auto value : pixels) assert(value == 0);

    // 画面外のテスト用ウインドウ。実際のWin32 layered windowの生成・更新も検証する。
    HWND parent = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
        WS_POPUP | WS_VISIBLE, -30000, -30000, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(parent);
    OneSegCaptionOverlay overlay;
    assert(overlay.SetBounds(parent, {0, 0, 8, 8}));
    HWND child = GetWindow(parent, GW_HWNDPREV);
    assert(GetWindow(child, GW_OWNER) == parent);
    assert(child && !IsWindowVisible(child));
    frames[0].images = {Image(0, 0, 2, 2, 255, 255, 0, 255)};
    assert(overlay.Draw(0, 8, 8, frames[0]));
    assert(IsWindowVisible(child));
    assert(overlay.Draw(0, 0, 0, {}));
    assert(!IsWindowVisible(child));
    assert(!overlay.SetBounds(parent, {0, 0, 8, 8}));
    assert(overlay.SetBounds(parent, {2, 2, 18, 18}));
    assert(!overlay.Draw(0, 8, 8, frames[0])); // リサイズ前のフレームは使わない。
    assert(overlay.Draw(0, 16, 16, frames[0]));
    SetWindowPos(parent, nullptr, -29000, -29000, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    assert(!overlay.SetBounds(parent, {2, 2, 18, 18}));
    RECT position = {};
    GetWindowRect(child, &position);
    assert(position.left == -28998 && position.top == -28998);
    ShowWindow(parent, SW_HIDE);
    assert(!overlay.SetBounds(parent, {2, 2, 18, 18}));
    assert(!IsWindowVisible(child));
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    assert(overlay.SetBounds(parent, {2, 2, 18, 18}));
    assert(overlay.Draw(0, 16, 16, frames[0]));
    assert(IsWindowVisible(child));
    overlay.Clear();
    assert(!IsWindow(child));
    DestroyWindow(parent);
    std::puts("OneSeg caption composition and layered window tests passed");
}
