#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "thirdparty/json.hpp"

namespace aribcaption { struct RenderResult; }

// 字幕の表示設定。キー名と既定値はTVCaption3に合わせている。
struct NativeCaptionSettings
{
    // 使用するフォントおよびフォールバックフォント名(空なら指定しない)
    std::wstring faceName = L"MS Gothic";
    std::wstring faceName1;
    std::wstring faceName2;
    // 字幕文の縁取りの幅の10倍。0より大きいとき常に縁取る
    int strokeWidth = 30;
    // ORN縁取り指定された字幕文の縁取りの幅の10倍
    int ornStrokeWidth = 50;
    // 字幕/文字スーパーを表示するかどうか
    bool showCaption = true;
    bool showSuperimpose = true;
    // 字幕/文字スーパーを受け取ってから表示するまでの遅延時間(ミリ秒)
    int delayTime = 450;
    int delayTimeSuper = 0;
    // 背景を常に透明にする
    bool noBackground = false;
    // 英数字を半角置換する
    bool replaceFullAlnum = true;
    // 日本語の約物などを半角置換する
    bool replaceFullJapanese = true;
    // DRCS図形を文字に置換する
    bool replaceDrcs = false;
    // 振り仮名らしきものを除外する
    bool ignoreSmall = false;
    // 4K/8K放送の字幕 (ARIB-TTML) をB24に変換したものではなく、TTMLのままページで描く
    // (dantto4kがTSに載せた字幕を使う。届いている間はB24の字幕を表示しない)
    bool ttmlCaption = true;
    // ワンセグ字幕を専用ウインドウではなく映像の上に表示する
    bool oneSegCaptionOverlay = false;
};

class NativeCaptionRenderer
{
public:
    using MessageCallback = std::function<void(const nlohmann::json&)>;
    // trueを返したフレームはWebViewへ送らず、独立した映像上の表示に使う。
    using FrameCallback = std::function<bool(int, int, int, const aribcaption::RenderResult&)>;

    explicit NativeCaptionRenderer(MessageCallback callback);
    ~NativeCaptionRenderer();

    NativeCaptionRenderer(const NativeCaptionRenderer&) = delete;
    NativeCaptionRenderer& operator=(const NativeCaptionRenderer&) = delete;

    bool Initialize();
    // Initialize()の前後どちらでも呼べる
    void SetSettings(const NativeCaptionSettings& settings);
    // 通常字幕の表示ボタン。文字スーパーはshowSuperimposeの設定だけに従う
    void SetEnabled(bool enabled);
    void SetProfileC(bool profileC);
    void SetFrameCallback(FrameCallback callback);
    void RequestRedraw();
    void Reset();
    void Push(int streamId, const std::vector<std::uint8_t>& data, std::optional<std::int64_t> pts90kHz);
    void Update(std::int64_t currentTimeMs, int frameWidth, int frameHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
