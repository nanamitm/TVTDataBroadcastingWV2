#include "pch.h"
#include "OneSegCaptionOverlay.h"

#include <algorithm>
#include <cstring>

void OneSegCaptionOverlay::Clear()
{
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
    parent_ = nullptr;
    frames_ = {};
    bounds_ = {};
    needsRedraw_ = true;
}

bool OneSegCaptionOverlay::SetBounds(HWND parent, const RECT& bounds)
{
    if (!parent || !IsWindowVisible(parent) || IsIconic(GetAncestor(parent, GA_ROOT)) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    {
        if (window_) ShowWindow(window_, SW_HIDE);
        needsRedraw_ = true;
        return false;
    }
    const HWND owner = GetAncestor(parent, GA_ROOT);
    if (window_ && (!IsWindow(window_) || parent_ != parent || GetWindow(window_, GW_OWNER) != owner)) Clear();
    if (!window_)
    {
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return false;
        parent_ = parent;
    }
    if (bounds.right - bounds.left != bounds_.right - bounds_.left ||
        bounds.bottom - bounds.top != bounds_.bottom - bounds_.top)
    {
        // 次のRenderで新しい大きさの字幕が届くまでは古い画像を拡大しない。
        frames_ = {};
        needsRedraw_ = true;
        ShowWindow(window_, SW_HIDE);
    }
    bounds_ = bounds;
    POINT position = {bounds.left, bounds.top};
    ClientToScreen(parent, &position);
    SetWindowPos(window_, nullptr, position.x, position.y, bounds.right - bounds.left,
        bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
    const bool redraw = needsRedraw_;
    needsRedraw_ = false;
    return redraw;
}

bool OneSegCaptionOverlay::Draw(int track, int width, int height, const aribcaption::RenderResult& result)
{
    if (!window_ || !IsWindow(window_) || track < 0 || track >= 2) return false;
    const int frameWidth = bounds_.right - bounds_.left;
    const int frameHeight = bounds_.bottom - bounds_.top;
    if (!result.images.empty() && (width != frameWidth || height != frameHeight)) return false;
    frames_[track] = result;
    if (frames_[0].images.empty() && frames_[1].images.empty())
    {
        ShowWindow(window_, SW_HIDE);
        return true;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frameWidth;
    info.bmiHeader.biHeight = -frameHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return false;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DeleteObject(bitmap); return false; }
    const auto old = SelectObject(dc, bitmap);
    auto* pixels = static_cast<unsigned char*>(bits);
    ComposeOneSegCaptionFrame(pixels, frameWidth, frameHeight, frames_);
    POINT origin = {};
    SIZE size = { frameWidth, frameHeight };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const bool success = UpdateLayeredWindow(window_, nullptr, nullptr, &size, dc, &origin, 0, &blend, ULW_ALPHA) != FALSE;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bitmap);
    ShowWindow(window_, success && IsWindowVisible(parent_) && !IsIconic(GetWindow(window_, GW_OWNER))
        ? SW_SHOWNOACTIVATE : SW_HIDE);
    return success;
}

void ComposeOneSegCaptionFrame(unsigned char* pixels, int frameWidth, int frameHeight,
    const std::array<aribcaption::RenderResult, 2>& frames)
{
    std::memset(pixels, 0, static_cast<size_t>(frameWidth) * frameHeight * 4);
    for (const auto& frame : frames)
    {
        // TVCaption3と同様、Cプロファイルの字幕を映像の下端に詰める。
        int shiftY = frameHeight;
        for (const auto& image : frame.images)
            shiftY = std::min(shiftY, frameHeight - image.dst_y - image.height);
        for (const auto& image : frame.images)
        {
            if (image.pixel_format != aribcaption::PixelFormat::kRGBA8888 || image.width <= 0 ||
                image.height <= 0 || image.stride < image.width * 4 ||
                image.bitmap.size() < static_cast<size_t>(image.stride) * image.height) continue;
            for (int y = std::max(0, -image.dst_y - shiftY);
                 y < image.height && image.dst_y + shiftY + y < frameHeight; ++y)
            {
                for (int x = std::max(0, -image.dst_x); x < image.width && image.dst_x + x < frameWidth; ++x)
                {
                    const auto* src = image.bitmap.data() + static_cast<size_t>(y) * image.stride + x * 4;
                    auto* dst = pixels + (static_cast<size_t>(image.dst_y + shiftY + y) * frameWidth + image.dst_x + x) * 4;
                    const unsigned alpha = src[3];
                    // UpdateLayeredWindowには乗算済みのBGRAを渡す。
                    for (int c = 0; c < 3; ++c)
                        dst[c] = static_cast<unsigned char>((src[2 - c] * alpha + dst[c] * (255 - alpha) + 127) / 255);
                    dst[3] = static_cast<unsigned char>(alpha + (dst[3] * (255 - alpha) + 127) / 255);
                }
            }
        }
    }
}
