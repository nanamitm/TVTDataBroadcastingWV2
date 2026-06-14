#include "pch.h"
#include "JikkyoStreamTable.h"
#include <set>
#include <vector>

namespace {
// Built-in nicovideo chatStreamID table (from NicoJK JKIDNameTable.h).
// Only channels with a known nicovideo stream id are listed.
struct BuiltinEntry { int jkID; const char* chatStreamID; };
constexpr BuiltinEntry kBuiltin[] = {
    {   1, "ch2646436" }, //   NHK総合
    {   2, "ch2646437" }, //   NHK Eテレ
    {   4, "ch2646438" }, //   日本テレビ
    {   5, "ch2646439" }, //   テレビ朝日
    {   6, "ch2646440" }, //   TBSテレビ
    {   7, "ch2646441" }, //   テレビ東京
    {   8, "ch2646442" }, //   フジテレビ
    {   9, "ch2646485" }, //   TOKYO MX
    { 101, "ch2647992" }, //   NHK BS
    { 211, "ch2646846" }, //   BS11イレブン
};
}

std::string JikkyoStreamTable::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], len, nullptr, nullptr);
    return s;
}

void JikkyoStreamTable::Load(const std::wstring& iniPath)
{
    m_map.clear();
    for (const auto& b : kBuiltin) {
        JikkyoStream s;
        s.chatStreamID = b.chatStreamID;
        s.refugeChatStreamID = s.chatStreamID; // default: refuge uses the same id
        m_map[b.jkID] = std::move(s);
    }
    MergeChatStreams(iniPath);
}

bool JikkyoStreamTable::Resolve(int jkID, JikkyoStream& out) const
{
    auto it = m_map.find(jkID);
    if (it == m_map.end()) return false;
    out = it->second;
    return true;
}

void JikkyoStreamTable::MergeChatStreams(const std::wstring& iniPath)
{
    // Read the whole [ChatStreams] section: "key=value\0key=value\0\0".
    std::vector<wchar_t> buf(4096);
    for (;;) {
        DWORD n = GetPrivateProfileSectionW(L"ChatStreams", buf.data(),
                                            static_cast<DWORD>(buf.size()), iniPath.c_str());
        if (n < buf.size() - 2) break;
        buf.resize(buf.size() * 2);
    }

    // NicoJK uses the FIRST value for a duplicate key, so apply each jkID once.
    std::set<int> applied;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        auto eq = entry.find(L'=');
        if (eq == std::wstring::npos) continue;
        int jkID = _wtoi(entry.substr(0, eq).c_str());
        if (jkID <= 0) continue;
        if (!applied.insert(jkID).second) continue; // first occurrence wins

        // Parse value char by char (NicoJK rule): before the first comma goes to
        // both chatStreamID and refugeChatStreamID; the first comma switches to
        // refuge-only; any other non-alphanumeric char is invalid.
        std::string val = WideToUtf8(entry.substr(eq + 1));
        JikkyoStream s;
        bool firstVal = true, invalid = false;
        for (char c : val) {
            bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            if (alnum) {
                s.refugeChatStreamID += c;
                if (firstVal) s.chatStreamID += c;
            } else if (c == ',' && firstVal) {
                s.refugeChatStreamID.clear();
                firstVal = false;
            } else {
                invalid = true;
                break;
            }
        }
        if (invalid) continue;

        if (s.chatStreamID.empty() && s.refugeChatStreamID.empty()) {
            m_map.erase(jkID); // explicit removal
        } else {
            m_map[jkID] = std::move(s);
        }
    }
}
