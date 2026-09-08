#include "pch.h"
#include "NativeCaptionRenderer.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <aribcaption/aribcaption.hpp>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

std::string wstrToUTF8String(const wchar_t* ws);

namespace
{
constexpr int STREAM_CAPTION = 0xbd;
constexpr int STREAM_SUPERIMPOSE = 0xbf;

std::string EncodeBase64(const std::uint8_t* data, std::size_t size)
{
    static constexpr char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
            (i + 2 < size ? static_cast<std::uint32_t>(data[i + 2]) : 0);
        result.push_back(TABLE[(value >> 18) & 0x3f]);
        result.push_back(TABLE[(value >> 12) & 0x3f]);
        result.push_back(i + 1 < size ? TABLE[(value >> 6) & 0x3f] : '=');
        result.push_back(i + 2 < size ? TABLE[value & 0x3f] : '=');
    }
    return result;
}
}

struct NativeCaptionRenderer::Impl
{
    struct Track
    {
        std::unique_ptr<aribcaption::Decoder> decoder;
        std::unique_ptr<aribcaption::Renderer> renderer;
        bool visible = false;
    };

    explicit Impl(MessageCallback messageCallback) : callback(std::move(messageCallback)) {}

    bool InitializeTrack(Track& track, aribcaption::CaptionType type)
    {
        track.decoder = std::make_unique<aribcaption::Decoder>(context);
        if (!track.decoder->Initialize(aribcaption::EncodingScheme::kAuto, type))
        {
            return false;
        }

        track.renderer = std::make_unique<aribcaption::Renderer>(context);
        if (!track.renderer->Initialize(type,
                                        aribcaption::FontProviderType::kDirectWrite,
                                        aribcaption::TextRendererType::kDirectWrite))
        {
            return false;
        }
        ApplySettings(track);
        return true;
    }

    void ApplySettings(Track& track)
    {
        if (track.decoder)
        {
            track.decoder->SetReplaceMSZFullWidthAlphanumeric(settings.replaceFullAlnum);
            track.decoder->SetReplaceMSZFullWidthJapanese(settings.replaceFullJapanese);
        }
        if (track.renderer)
        {
            std::vector<std::string> fontFamily;
            for (const auto* name : {&settings.faceName, &settings.faceName1, &settings.faceName2})
            {
                if (!name->empty())
                {
                    fontFamily.push_back(wstrToUTF8String(name->c_str()));
                }
            }
            if (!fontFamily.empty())
            {
                track.renderer->SetDefaultFontFamily(fontFamily, true);
            }
            track.renderer->SetForceNoBackground(settings.noBackground);
            // 縁取り幅は10倍で保持している
            track.renderer->SetForceStrokeText(settings.strokeWidth > 0);
            track.renderer->SetStrokeWidth(
                (settings.strokeWidth > 0 ? settings.strokeWidth : settings.ornStrokeWidth) / 10.0f);
            track.renderer->SetReplaceDRCS(settings.replaceDrcs);
            track.renderer->SetForceNoRuby(settings.ignoreSmall);
        }
    }

    bool IsTrackVisible(int index) const
    {
        return index == 0 ? settings.showCaption : settings.showSuperimpose;
    }

    std::int64_t TrackDelayMs(int index) const
    {
        return index == 0 ? settings.delayTime : settings.delayTimeSuper;
    }

    void SendClear(int index)
    {
        callback({{"type", "captionImage"}, {"track", index}, {"images", nlohmann::json::array()}});
        tracks[index].visible = false;
    }

    void RenderTrack(int index, std::int64_t currentTimeMs, bool frameSizeChanged)
    {
        auto& track = tracks[index];
        aribcaption::RenderResult result;
        const auto status = track.renderer->Render(currentTimeMs, result);
        if (status == aribcaption::RenderStatus::kError || status == aribcaption::RenderStatus::kNoImage)
        {
            // kErrorで何もしないと、表示中の字幕が消えずに残り続けてしまう。
            if (track.visible)
            {
                SendClear(index);
            }
            return;
        }
        if (status == aribcaption::RenderStatus::kGotImageUnchanged && !frameSizeChanged)
        {
            return;
        }

        nlohmann::json images = nlohmann::json::array();
        for (const auto& image : result.images)
        {
            if (image.width <= 0 || image.height <= 0 || image.stride <= 0 || image.bitmap.empty())
            {
                continue;
            }
            images.push_back({
                {"x", image.dst_x},
                {"y", image.dst_y},
                {"width", image.width},
                {"height", image.height},
                {"stride", image.stride},
                {"data", EncodeBase64(image.bitmap.data(), image.bitmap.size())},
            });
        }
        callback({
            {"type", "captionImage"},
            {"track", index},
            {"frameWidth", frameWidth},
            {"frameHeight", frameHeight},
            {"images", std::move(images)},
        });
        track.visible = !result.images.empty();
    }

    MessageCallback callback;
    NativeCaptionSettings settings;
    aribcaption::Context context;
    std::array<Track, 2> tracks;
    int frameWidth = 0;
    int frameHeight = 0;
    std::int64_t currentTimeMs = 0;
};

NativeCaptionRenderer::NativeCaptionRenderer(MessageCallback callback) :
    impl_(std::make_unique<Impl>(std::move(callback)))
{
}

NativeCaptionRenderer::~NativeCaptionRenderer() = default;

bool NativeCaptionRenderer::Initialize()
{
    return impl_->InitializeTrack(impl_->tracks[0], aribcaption::CaptionType::kCaption) &&
           impl_->InitializeTrack(impl_->tracks[1], aribcaption::CaptionType::kSuperimpose);
}

void NativeCaptionRenderer::Reset()
{
    for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    {
        auto& track = impl_->tracks[i];
        impl_->SendClear(i);
        const auto type = i == 0 ? aribcaption::CaptionType::kCaption : aribcaption::CaptionType::kSuperimpose;
        if (!impl_->InitializeTrack(track, type))
        {
            track.decoder.reset();
            track.renderer.reset();
            continue;
        }
        if (impl_->frameWidth > 0 && impl_->frameHeight > 0)
        {
            track.renderer->SetFrameSize(impl_->frameWidth, impl_->frameHeight);
        }
    }
}

void NativeCaptionRenderer::Push(int streamId, const std::vector<std::uint8_t>& data,
                                 std::optional<std::int64_t> pts90kHz)
{
    const int index = streamId == STREAM_CAPTION ? 0 : streamId == STREAM_SUPERIMPOSE ? 1 : -1;
    if (index < 0 || data.empty())
    {
        return;
    }

    auto& track = impl_->tracks[index];
    if (!track.decoder || !track.renderer)
    {
        return;
    }
    const std::int64_t ptsMs = index == 0 && pts90kHz ? *pts90kHz / 90 : impl_->currentTimeMs;
    aribcaption::DecodeResult result;
    if (track.decoder->Decode(data.data(), data.size(), ptsMs, result) != aribcaption::DecodeStatus::kGotCaption)
    {
        return;
    }
    if (result.caption->has_builtin_sound)
    {
        impl_->callback({{"type", "captionSound"}, {"sound", result.caption->builtin_sound_id}});
    }
    track.renderer->AppendCaption(std::move(*result.caption));
}

void NativeCaptionRenderer::Update(std::int64_t currentTimeMs, int frameWidth, int frameHeight)
{
    if (frameWidth <= 0 || frameHeight <= 0)
    {
        return;
    }
    impl_->currentTimeMs = currentTimeMs;
    const bool frameSizeChanged = impl_->frameWidth != frameWidth || impl_->frameHeight != frameHeight;
    if (frameSizeChanged)
    {
        impl_->frameWidth = frameWidth;
        impl_->frameHeight = frameHeight;
        for (auto& track : impl_->tracks)
        {
            if (track.renderer)
            {
                track.renderer->SetFrameSize(frameWidth, frameHeight);
            }
        }
    }
    for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    {
        if (!impl_->tracks[i].renderer)
        {
            continue;
        }
        if (!impl_->IsTrackVisible(i))
        {
            if (impl_->tracks[i].visible)
            {
                impl_->SendClear(i);
            }
            continue;
        }
        impl_->RenderTrack(i, currentTimeMs - impl_->TrackDelayMs(i), frameSizeChanged);
    }
}

void NativeCaptionRenderer::SetSettings(const NativeCaptionSettings& settings)
{
    impl_->settings = settings;
    for (auto& track : impl_->tracks)
    {
        impl_->ApplySettings(track);
    }
}
