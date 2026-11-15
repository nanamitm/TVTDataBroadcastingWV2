// NG の正規表現は、元の本文と NFKC でそろえた本文の両方に当てる。
#include "CommentNG.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static std::wstring WriteIni(const std::wstring& body)
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"CommentNGTests.ini";
    // UTF-16LE (BOM 付き) にしないと GetPrivateProfileSectionW が日本語を読めない。
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    f.write("\xFF\xFE", 2);
    f.write(reinterpret_cast<const char*>(body.data()), body.size() * sizeof(wchar_t));
    return path;
}

static bool IsNGText(const CommentNG& ng, const char* text)
{
    Comment c;
    c.text = text;
    return ng.IsNG(c);
}

static void Expect(bool cond, const char* what)
{
    if (!cond) throw std::runtime_error(what);
}

int main()
{
    try {
        Expect(CommentNG::NormalizeForMatch("ａｂＣ１２３") == "abC123", "Full-width ASCII was not narrowed");
        Expect(CommentNG::NormalizeForMatch("ｱｲｳ") == "アイウ", "Half-width kana was not widened");
        Expect(CommentNG::NormalizeForMatch("plain ascii") == "plain ascii", "ASCII text changed");
        Expect(CommentNG::NormalizeForMatch("漢字かな") == "漢字かな", "Ordinary Japanese changed");

        auto path = WriteIni(L"[NG]\r\nRegex0=spam\r\nRegex1=ｗｗｗ\r\nRegex2=アホ\r\n");
        CommentNG ng;
        ng.Load(path);
        Expect(IsNGText(ng, "spam"), "ASCII NG word was missed");
        Expect(IsNGText(ng, "ｓｐａｍ"), "Full-width NG word was missed");
        Expect(IsNGText(ng, "ｓpａm です"), "Mixed-width NG word was missed");
        Expect(IsNGText(ng, "ｗｗｗ"), "Full-width pattern no longer matches full-width text");
        Expect(IsNGText(ng, "ｱﾎ"), "Half-width kana NG word was missed");
        Expect(!IsNGText(ng, "spa m ｓｐａ"), "Unrelated comment was hidden");
        Expect(!IsNGText(ng, "ふつうのコメント"), "Ordinary comment was hidden");
        std::filesystem::remove(path);

        std::cout << "CommentNG tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
