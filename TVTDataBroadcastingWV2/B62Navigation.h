#pragma once
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// 4Kデータ放送のアプリケーションが自分のiframeを外部のページ (通信で配信される
// コンテンツ) へ移動させたときの扱い。移動先をそのまま開くとオリジンが変わり、
// B62Shim.hのAPIやキー入力が使えず、ページの読み込みもアプリケーションの通信と
// 見なせなくなる。そこで同じパスでアプリケーションのオリジンのページとして開き直し、
// 放送に無いファイルは移動先のサーバーから取得する。
namespace B62Navigation {

// "https://host[:port]/path?query#fragment" -> ("https://host[:port]", "path?query")
// http/httpsで、ホストがURLに安全な文字だけのものに限る (認証情報などは受け付けない)。
inline std::optional<std::pair<std::wstring, std::wstring>> SplitUrl(std::wstring_view url)
{
    size_t schemeEnd;
    if (url.size() >= 7 && _wcsnicmp(url.data(), L"http://", 7) == 0)
    {
        schemeEnd = 7;
    }
    else if (url.size() >= 8 && _wcsnicmp(url.data(), L"https://", 8) == 0)
    {
        schemeEnd = 8;
    }
    else
    {
        return std::nullopt;
    }
    const size_t authorityEnd = std::min(url.find_first_of(L"/?#", schemeEnd), url.size());
    const std::wstring_view authority = url.substr(schemeEnd, authorityEnd - schemeEnd);
    if (authority.empty() ||
        authority.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:[]") != std::wstring_view::npos)
    {
        return std::nullopt;
    }
    std::wstring_view rest = url.substr(authorityEnd);
    rest = rest.substr(0, std::min(rest.find(L'#'), rest.size()));
    if (!rest.empty() && rest.front() == L'/')
    {
        rest.remove_prefix(1);
    }
    // ヘッダー (Location) に入れるので、制御文字を含むものは受け付けない
    for (wchar_t c : rest)
    {
        if (c < 0x20 || c == 0x7f)
        {
            return std::nullopt;
        }
    }
    return std::make_pair(std::wstring(url.substr(0, authorityEnd)), std::wstring(rest));
}

// JavaScriptの文字列リテラル。英数字と一部の記号以外は\uXXXXにする
// (HTMLの<script>内に置くので、<や"もそのままにしない)。
inline std::wstring ScriptStringLiteral(std::wstring_view value)
{
    std::wstring literal = L"\"";
    for (wchar_t c : value)
    {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
            std::wstring_view(L"/:.-_?=&%~+,;").find(c) != std::wstring_view::npos)
        {
            literal += c;
        }
        else
        {
            wchar_t escaped[7];
            swprintf_s(escaped, L"\\u%04x", static_cast<unsigned>(c));
            literal += escaped;
        }
    }
    return literal + L"\"";
}

} // namespace B62Navigation
