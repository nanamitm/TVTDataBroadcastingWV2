#include "pch.h"
#include "NativeCaptionRenderer.h"

#include <algorithm>
#include <array>
#include <deque>
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
constexpr std::int64_t CLOCK_PERIOD_90KHZ = INT64_C(1) << 33;
constexpr std::int64_t MAX_CLOCK_JUMP_90KHZ = 10000 * 90;
constexpr std::size_t MAX_STORED_CAPTIONS = 128;

std::int64_t WrapClockDelta(std::int64_t delta)
{
    delta %= CLOCK_PERIOD_90KHZ;
    if (delta > CLOCK_PERIOD_90KHZ / 2) delta -= CLOCK_PERIOD_90KHZ;
    else if (delta <= -CLOCK_PERIOD_90KHZ / 2) delta += CLOCK_PERIOD_90KHZ;
    return delta;
}

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
        struct PendingCaption {
            aribcaption::Caption caption;
            std::optional<std::int64_t> pts90kHz;
        };
        std::unique_ptr<aribcaption::Decoder> decoder;
        std::unique_ptr<aribcaption::Renderer> renderer;
        std::deque<PendingCaption> pending;
        bool visible = false;
    };

    explicit Impl(MessageCallback messageCallback) : callback(std::move(messageCallback)) {}

    bool InitializeTrack(Track& track, aribcaption::CaptionType type)
    {
        track.pending.clear();
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
        // The default minimum policy only evicts relative to a rendered caption.
        // Hidden tracks must have a bound even when Render() is never called.
        track.renderer->SetStoragePolicy(aribcaption::CaptionStoragePolicy::kUpperLimitCount, MAX_STORED_CAPTIONS);
        track.decoder->SetProfile(profileC ? aribcaption::Profile::kProfileC : aribcaption::Profile::kProfileA);
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
            else
            {
                // Keep the library's language-specific families and stop forcing
                // a previously selected font. Restore libaribcaption's Windows
                // default for language 0 (RendererImpl::LoadDefaultFontFamilies),
                // including the Japanese fonts used as a fallback for symbols.
                track.renderer->SetDefaultFontFamily(
                    {"sans-serif", "Windows TV MaruGothic", "Noto Sans JP", "MS Gothic"}, false);
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
        return index == 0 ? enabled && settings.showCaption : settings.showSuperimpose;
    }

    std::int64_t TrackDelayMs(int index) const
    {
        return index == 0 ? settings.delayTime : settings.delayTimeSuper;
    }

    void SendClear(int index)
    {
        if (frameCallback) frameCallback(index, frameWidth, frameHeight, {});
        callback({{"type", "captionImage"}, {"track", index}, {"images", nlohmann::json::array()}});
        tracks[index].visible = false;
    }

    void AppendCaption(int index, aribcaption::Caption caption, std::optional<std::int64_t> pts90kHz)
    {
        caption.pts = pts90kHz
            ? (currentTime90kHz + WrapClockDelta(*pts90kHz - *lastPCR90kHz)) / 90
            : currentTimeMs;
        if (caption.has_builtin_sound && IsTrackVisible(index))
            callback({{"type", "captionSound"}, {"sound", caption.builtin_sound_id}});
        tracks[index].renderer->AppendCaption(std::move(caption));
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

        if (frameCallback && frameCallback(index, frameWidth, frameHeight, result))
        {
            // 表示先を切り替えた直後の専用ウインドウの字幕も消す。
            callback({{"type", "captionImage"}, {"track", index}, {"images", nlohmann::json::array()}});
            track.visible = !result.images.empty();
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
    FrameCallback frameCallback;
    bool profileC = false;
    NativeCaptionSettings settings;
    bool enabled = false;
    bool forceRender = false;
    aribcaption::Context context;
    std::array<Track, 2> tracks;
    int frameWidth = 0;
    int frameHeight = 0;
    std::int64_t currentTimeMs = 0;
    std::int64_t currentTime90kHz = 0;
    std::optional<std::int64_t> lastPCR90kHz;
};

NativeCaptionRenderer::NativeCaptionRenderer(MessageCallback callback) :
    impl_(std::make_unique<Impl>(std::move(callback)))
{
}

NativeCaptionRenderer::~NativeCaptionRenderer() = default;

void NativeCaptionRenderer::RequestRedraw()
{
    impl_->forceRender = true;
}

void NativeCaptionRenderer::SetFrameCallback(FrameCallback callback)
{
    impl_->frameCallback = std::move(callback);
    impl_->forceRender = true;
}

void NativeCaptionRenderer::SetProfileC(bool profileC)
{
    if (impl_->profileC != profileC)
    {
        impl_->profileC = profileC;
        if (impl_->tracks[0].decoder) Reset();
    }
}

bool NativeCaptionRenderer::Initialize()
{
    return impl_->InitializeTrack(impl_->tracks[0], aribcaption::CaptionType::kCaption) &&
           impl_->InitializeTrack(impl_->tracks[1], aribcaption::CaptionType::kSuperimpose);
}

void NativeCaptionRenderer::Reset()
{
    impl_->currentTimeMs = 0;
    impl_->currentTime90kHz = 0;
    impl_->lastPCR90kHz.reset();
    impl_->forceRender = true;
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
    if (index != 0) pts90kHz.reset();
    if (pts90kHz && (*pts90kHz < 0 || *pts90kHz >= CLOCK_PERIOD_90KHZ)) return;
    // Decode management/DRCS data immediately, but do not schedule a caption
    // against zero or the previous service's clock before the first PCR.
    const std::int64_t ptsMs = pts90kHz ? *pts90kHz / 90 : impl_->currentTimeMs;
    aribcaption::DecodeResult result;
    if (track.decoder->Decode(data.data(), data.size(), ptsMs, result) != aribcaption::DecodeStatus::kGotCaption)
    {
        return;
    }
    if (!impl_->lastPCR90kHz)
    {
        track.pending.push_back({std::move(*result.caption), pts90kHz});
        if (track.pending.size() > MAX_STORED_CAPTIONS) track.pending.pop_front();
        return;
    }
    impl_->AppendCaption(index, std::move(*result.caption), pts90kHz);
}

void NativeCaptionRenderer::Update(std::int64_t currentTimeMs, int frameWidth, int frameHeight)
{
    if (frameWidth <= 0 || frameHeight <= 0)
    {
        return;
    }
    if (currentTimeMs < 0 || currentTimeMs > (CLOCK_PERIOD_90KHZ - 1) / 90) return;
    const auto rawPCR90kHz = currentTimeMs * 90;
    if (impl_->lastPCR90kHz)
    {
        const auto delta = WrapClockDelta(rawPCR90kHz - *impl_->lastPCR90kHz);
        // Match the TTML path: a large jump is a seek/discontinuity, not a wrap.
        if (delta > MAX_CLOCK_JUMP_90KHZ || delta < -MAX_CLOCK_JUMP_90KHZ) Reset();
        else impl_->currentTime90kHz += delta;
    }
    if (!impl_->lastPCR90kHz) impl_->currentTime90kHz = rawPCR90kHz;
    impl_->lastPCR90kHz = rawPCR90kHz;
    impl_->currentTimeMs = impl_->currentTime90kHz / 90;
    for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    {
        auto& pending = impl_->tracks[i].pending;
        while (!pending.empty())
        {
            auto caption = std::move(pending.front());
            pending.pop_front();
            if (impl_->tracks[i].renderer) impl_->AppendCaption(i, std::move(caption.caption), caption.pts90kHz);
        }
    }
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
    // 表示再開直後はlibaribcaptionが差分なしと判断しても描き直す必要がある
    const bool forceRedraw = frameSizeChanged || impl_->forceRender;
    impl_->forceRender = false;
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
        impl_->RenderTrack(i, impl_->currentTimeMs - impl_->TrackDelayMs(i), forceRedraw);
    }
}

void NativeCaptionRenderer::SetEnabled(bool enabled)
{
    if (impl_->enabled == enabled)
    {
        return;
    }
    impl_->enabled = enabled;
    if (enabled)
    {
        impl_->forceRender = true;
        return;
    }
    // 表示再開時に期限切れの字幕が一瞬見えないよう消しておく
    for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    {
        if (!impl_->IsTrackVisible(i) && impl_->tracks[i].visible)
        {
            impl_->SendClear(i);
        }
    }
}

void NativeCaptionRenderer::SetSettings(const NativeCaptionSettings& settings)
{
    impl_->settings = settings;
    for (auto& track : impl_->tracks)
    {
        impl_->ApplySettings(track);
    }
    // 非表示の間に描画済みと判定された字幕も、設定変更後は描き直す。
    impl_->forceRender = true;
    for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    {
        if (!impl_->IsTrackVisible(i) && impl_->tracks[i].visible)
        {
            impl_->SendClear(i);
        }
    }
}
