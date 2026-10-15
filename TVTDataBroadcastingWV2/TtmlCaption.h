#pragma once

#include <string_view>

#include "thirdparty/json.hpp"

// 4K/8K放送の字幕 (ARIB-TTML) をdantto4kのparser/resolverで解析し、スタイルの継承と
// タイミングの合成を済ませた文書をページ (ttml_caption.ts) に渡す形にする。
// superimposeは文字スーパー (非同期型、時刻を持たない)。ignoreTimingはTMDが1111
// (時刻制御なし) のときで、begin/endを読まない (ARIB TR-B39 8.5.9.2)。
// 解析できなければ { "error": 文字列 } を返す。
nlohmann::json ParseTtmlCaption(std::string_view xml, bool superimpose, bool ignoreTiming);
