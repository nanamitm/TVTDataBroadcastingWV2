// TtmlCaption.cpp (ARIB-TTMLの字幕をページに渡す形にする) のテスト
#include "TtmlCaption.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{

int checks = 0;
int failures = 0;

void Check(bool ok, const char* message)
{
    checks++;
    if (!ok)
    {
        failures++;
        std::printf("FAIL: %s\n", message);
    }
}

bool Near(const nlohmann::json& value, double expected)
{
    return value.is_number() && std::abs(value.get<double>() - expected) < 1e-6;
}

// 放送の字幕 (BSテレ東4K、2026-10-14) と同じ形の文書
const char kDocument[] =
    R"(<?xml version="1.0" encoding="utf-8"?><tt xml:lang="ja" xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" xmlns:arib-tt="http://www.arib.or.jp/ns/arib-tt">)"
    R"(<head><styling><style xml:id="s1" tts:fontFamily="丸ゴシック" tts:fontSize="144px 144px" tts:color="#FFFFFFFF" tts:backgroundColor="#00000080" tts:lineHeight="240px" arib-tt:letter-spacing="16px"/>)"
    R"(<style xml:id="s2" tts:fontFamily="丸ゴシック" tts:fontSize="72px 144px" tts:color="#FFFF00FF" tts:backgroundColor="#00000080" tts:lineHeight="240px" arib-tt:letter-spacing="8px"/></styling>)"
    R"(<layout><region xml:id="r1" tts:origin="1240px 1200px" tts:extent="1360px 480px" tts:writingMode="lrtb"/></layout></head>)"
    R"(<body><div begin="01:57:23.846" end="01:57:26.946"><p region="r1"><span style="s1">はーい</span><span style="s2">AB</span><br/><span style="s1" begin="00:00:01.000" end="00:00:02.000">食べて</span></p></div></body></tt>)";

void TestDocument()
{
    auto doc = ParseTtmlCaption(kDocument, false, false);
    Check(!doc.contains("error"), "the document parses");
    if (doc.contains("error"))
    {
        std::printf("  %s\n", doc["error"].get<std::string>().c_str());
    }
    Check(Near(doc["begin"], 7043.846) && Near(doc["end"], 7046.946), "the division timing is in seconds");
    auto& paragraphs = doc["paragraphs"];
    Check(paragraphs.is_array() && paragraphs.size() == 1, "one paragraph");
    auto& p = paragraphs[0];
    Check(p["region"]["id"] == "r1", "the region is named");
    Check(Near(p["region"]["style"]["origin"][0], 1240) && Near(p["region"]["style"]["origin"][1], 1200), "the region origin");
    Check(Near(p["region"]["style"]["extent"][0], 1360) && Near(p["region"]["style"]["extent"][1], 480), "the region extent");
    Check(p["region"]["style"]["writingMode"] == "lrtb", "the writing mode");
    auto& spans = p["spans"];
    Check(spans.is_array() && spans.size() == 4, "three spans and the line break");
    Check(spans[0]["content"] == nlohmann::json::array({ "はーい" }), "the text of the first span");
    Check(Near(spans[0]["style"]["fontSize"][0], 144) && Near(spans[1]["style"]["fontSize"][0], 72), "the half width span keeps its font size");
    Check(spans[1]["style"]["color"] == nlohmann::json::array({ 255, 255, 0, 255 }), "the colour is RGBA");
    Check(spans[0]["style"]["backgroundColor"] == nlohmann::json::array({ 0, 0, 0, 128 }), "the background colour is RGBA");
    Check(Near(spans[0]["style"]["lineHeight"], 240) && Near(spans[0]["style"]["letterSpacing"], 16), "line height and letter spacing");
    Check(spans[0]["style"]["fontFamily"] == "丸ゴシック", "the font family");
    Check(Near(spans[0]["begin"], 7043.846), "a span inherits the division timing");
    // <br/>は内容がnullだけのspanになる
    Check(spans[2]["content"] == nlohmann::json::array({ nullptr }), "the line break");
    Check(spans[3]["content"] == nlohmann::json::array({ "食べて" }), "the text after the line break");
    Check(Near(spans[3]["begin"], 7044.846) && Near(spans[3]["end"], 7045.846), "a span timing is relative to its parent");
}

void TestNoTiming()
{
    auto ignored = ParseTtmlCaption(kDocument, false, true);
    Check(!ignored.contains("error") && !ignored.contains("begin") && !ignored["paragraphs"][0]["spans"][0].contains("begin"),
        "TMD 1111 reads no begin/end");
    auto superimpose = ParseTtmlCaption(kDocument, true, false);
    Check(!superimpose.contains("error") && !superimpose["paragraphs"][0]["spans"][3].contains("end"),
        "the superimposition is asynchronous");
}

void TestErrors()
{
    Check(ParseTtmlCaption("<tt", false, false).contains("error"), "broken XML is an error");
    Check(ParseTtmlCaption("", false, false).contains("error"), "an empty document is an error");
    auto empty = ParseTtmlCaption(
        R"(<tt xmlns="http://www.w3.org/ns/ttml"><head/><body/></tt>)", false, false);
    Check(!empty.contains("error") && empty["paragraphs"].empty(), "a document without captions clears the screen");
}

}

int main()
{
    TestDocument();
    TestNoTiming();
    TestErrors();
    if (failures != 0)
    {
        std::printf("TTML caption tests failed: %d of %d checks\n", failures, checks);
        return 1;
    }
    std::printf("TTML caption tests passed: %d checks\n", checks);
    return 0;
}
