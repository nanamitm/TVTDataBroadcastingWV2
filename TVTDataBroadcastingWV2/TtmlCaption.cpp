// プリコンパイル済みヘッダーを使わない (テストでも単独でビルドする)
#include "TtmlCaption.h"

#include <optional>
#include <variant>

#include "ttml/parser.h"
#include "ttml/resolver.h"

namespace ttml = arib::ttml;

namespace
{

// 秒。未指定は書かない (ページでは文書が表示されている間ずっと)
void SetTiming(nlohmann::json& out, const ttml::resolved::Timing& timing)
{
    if (timing.begin)
    {
        out["begin"] = static_cast<double>(timing.begin->count()) / 1000.0;
    }
    if (timing.end)
    {
        out["end"] = static_cast<double>(timing.end->count()) / 1000.0;
    }
}

nlohmann::json ToColor(const ttml::StyleColorValue& color)
{
    return nlohmann::json::array({ color.r(), color.g(), color.b(), color.a() });
}

nlohmann::json ToPair(const ttml::LengthPair& pair)
{
    return nlohmann::json::array({ pair.x, pair.y });
}

nlohmann::json ToStyle(const ttml::StyleProperties& style)
{
    nlohmann::json out = nlohmann::json::object();
    if (style.background_color)
    {
        out["backgroundColor"] = ToColor(*style.background_color);
    }
    if (style.color)
    {
        out["color"] = ToColor(*style.color);
    }
    if (style.origin)
    {
        out["origin"] = ToPair(*style.origin);
    }
    if (style.extent)
    {
        out["extent"] = ToPair(*style.extent);
    }
    if (style.font_size)
    {
        out["fontSize"] = ToPair(*style.font_size);
    }
    if (style.font_family)
    {
        out["fontFamily"] = *style.font_family;
    }
    if (style.font_style)
    {
        out["fontStyle"] = *style.font_style == ttml::StyleFontStyleValue::Italic ? "italic" : "normal";
    }
    if (style.font_weight)
    {
        out["fontWeight"] = *style.font_weight == ttml::StyleFontWeightValue::Bold ? "bold" : "normal";
    }
    if (style.line_height)
    {
        // normalはnull、数値はpx
        if (const auto* px = std::get_if<double>(&*style.line_height))
        {
            out["lineHeight"] = *px;
        }
        else
        {
            out["lineHeight"] = nullptr;
        }
    }
    if (style.text_decoration)
    {
        out["underline"] = *style.text_decoration == ttml::StyleTextDecorationValue::Underline;
    }
    if (style.text_outline)
    {
        if (const auto* outline = std::get_if<ttml::TextOutline>(&*style.text_outline))
        {
            nlohmann::json o{ { "width", outline->border_width }, { "blur", outline->blur_width } };
            if (outline->color)
            {
                o["color"] = ToColor(*outline->color);
            }
            out["textOutline"] = std::move(o);
        }
    }
    if (style.writing_mode)
    {
        out["writingMode"] = *style.writing_mode == ttml::StyleWritingModeValue::Tbrl ? "tbrl" : "lrtb";
    }
    if (style.letter_spacing)
    {
        out["letterSpacing"] = *style.letter_spacing;
    }
    if (style.ruby)
    {
        out["ruby"] = *style.ruby;
    }
    if (style.border_top)
    {
        out["borderTop"] = *style.border_top;
    }
    if (style.border_right)
    {
        out["borderRight"] = *style.border_right;
    }
    if (style.border_bottom)
    {
        out["borderBottom"] = *style.border_bottom;
    }
    if (style.border_left)
    {
        out["borderLeft"] = *style.border_left;
    }
    if (style.flashing)
    {
        out["flashing"] = *style.flashing;
    }
    return out;
}

void SetRegion(nlohmann::json& out, const std::optional<ttml::resolved::Region>& region)
{
    if (region)
    {
        out["region"] = { { "id", region->id }, { "style", ToStyle(region->style) } };
    }
}

}

nlohmann::json ParseTtmlCaption(std::string_view xml, bool superimpose, bool ignoreTiming)
{
    // 字幕は同期型、文字スーパーは非同期型 (dantto4kのB24変換と同じ)
    const auto mode = superimpose ? ttml::SyncMode::Async : ttml::SyncMode::Sync;
    try
    {
        auto parsed = ttml::parse(xml, mode, ignoreTiming);
        if (parsed.has_error() || !parsed.document)
        {
            return { { "error", parsed.error ? parsed.error->message : "parse failed" } };
        }
        auto resolved = ttml::resolve(*parsed.document, mode);
        if (resolved.has_error() || !resolved.document)
        {
            return { { "error", resolved.error ? resolved.error->message : "resolve failed" } };
        }

        nlohmann::json result = nlohmann::json::object();
        nlohmann::json paragraphs = nlohmann::json::array();
        if (const auto& division = resolved.document->division)
        {
            SetTiming(result, division->timing);
            for (const auto& paragraph : division->paragraphs)
            {
                nlohmann::json p = nlohmann::json::object();
                SetTiming(p, paragraph.timing);
                SetRegion(p, paragraph.region);
                p["style"] = ToStyle(paragraph.style);
                nlohmann::json spans = nlohmann::json::array();
                for (const auto& span : paragraph.spans)
                {
                    nlohmann::json s = nlohmann::json::object();
                    SetTiming(s, span.timing);
                    s["style"] = ToStyle(span.style);
                    SetRegion(s, span.region);
                    // 文字列はそのまま、<br/>はnull
                    nlohmann::json content = nlohmann::json::array();
                    for (const auto& item : span.content)
                    {
                        if (const auto* text = std::get_if<std::string>(&item))
                        {
                            content.push_back(*text);
                        }
                        else
                        {
                            content.push_back(nullptr);
                        }
                    }
                    s["content"] = std::move(content);
                    spans.push_back(std::move(s));
                }
                p["spans"] = std::move(spans);
                paragraphs.push_back(std::move(p));
            }
        }
        result["paragraphs"] = std::move(paragraphs);
        return result;
    }
    catch (const std::exception& e)
    {
        // 放送から届いた文書で例外を外に出さない
        return { { "error", e.what() } };
    }
}
