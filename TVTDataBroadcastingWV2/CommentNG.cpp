#include "pch.h"
#include "CommentNG.h"
#include <algorithm>

namespace {
constexpr const wchar_t* kSection = L"NG";
}

std::string CommentNG::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], len, nullptr, nullptr);
    return s;
}

std::wstring CommentNG::Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], len);
    return w;
}

std::vector<std::string> CommentNG::SplitMailTokens(const std::string& mail)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char ch : mail) {
        if (ch == ' ' || ch == '\t') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

void CommentNG::Load(const std::wstring& iniPath)
{
    m_iniPath = iniPath;
    m_regexes.clear();
    m_users.clear();
    m_commands.clear();

    // Read the whole [NG] section: returns "Key=Value\0Key=Value\0\0".
    std::vector<wchar_t> buf(4096);
    DWORD n;
    for (;;) {
        n = GetPrivateProfileSectionW(kSection, buf.data(),
                                      static_cast<DWORD>(buf.size()), iniPath.c_str());
        if (n < buf.size() - 2) break;
        buf.resize(buf.size() * 2);
    }

    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        auto eq = entry.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = entry.substr(0, eq);
        std::wstring val = entry.substr(eq + 1);
        if (val.empty()) continue;
        std::string valU8 = WideToUtf8(val);

        // Compare key prefix case-insensitively.
        auto startsWith = [&key](const wchar_t* prefix) {
            size_t len = wcslen(prefix);
            if (key.size() < len) return false;
            return _wcsnicmp(key.c_str(), prefix, len) == 0;
        };

        if (startsWith(L"Regex")) {
            try {
                RegexRule rule{ valU8, std::regex(valU8) };
                m_regexes.push_back(std::move(rule));
            } catch (const std::regex_error&) {
                // Skip invalid pattern.
            }
        } else if (startsWith(L"User")) {
            m_users.push_back(valU8);
        } else if (startsWith(L"Command")) {
            m_commands.push_back(valU8);
        }
    }
}

bool CommentNG::IsNG(const Comment& c) const
{
    if (!c.userId.empty()) {
        for (const auto& u : m_users) {
            if (u == c.userId) return true;
        }
    }
    if (!c.mail.empty() && !m_commands.empty()) {
        auto tokens = SplitMailTokens(c.mail);
        for (const auto& tok : tokens) {
            for (const auto& cmd : m_commands) {
                if (tok == cmd) return true;
            }
        }
    }
    for (const auto& r : m_regexes) {
        try {
            if (std::regex_search(c.text, r.re)) return true;
        } catch (const std::regex_error&) {
            // Ignore at match time as well.
        }
    }
    return false;
}

bool CommentNG::HasUser(const std::string& userId) const
{
    for (const auto& u : m_users) {
        if (u == userId) return true;
    }
    return false;
}

int CommentNG::NextFreeIndex(const wchar_t* keyPrefix) const
{
    // Probe indices until an empty value is found.
    for (int i = 0; i < 100000; ++i) {
        wchar_t key[64];
        swprintf_s(key, L"%s%d", keyPrefix, i);
        wchar_t val[8];
        DWORD r = GetPrivateProfileStringW(kSection, key, L"", val,
                                           _countof(val), m_iniPath.c_str());
        if (r == 0) return i;
    }
    return 0;
}

bool CommentNG::AddUser(const std::string& userId)
{
    if (userId.empty() || m_iniPath.empty()) return false;
    if (HasUser(userId)) return false;

    int idx = NextFreeIndex(L"User");
    wchar_t key[64];
    swprintf_s(key, L"User%d", idx);
    if (!WritePrivateProfileStringW(kSection, key, Utf8ToWide(userId).c_str(), m_iniPath.c_str())) {
        return false;
    }
    m_users.push_back(userId);
    return true;
}

bool CommentNG::RemoveUser(const std::string& userId)
{
    if (userId.empty() || m_iniPath.empty()) return false;

    bool removed = false;
    // Find and clear every key whose value equals userId, then reload.
    for (int i = 0; i < 100000; ++i) {
        wchar_t key[64];
        swprintf_s(key, L"User%d", i);
        wchar_t val[512];
        DWORD r = GetPrivateProfileStringW(kSection, key, L"", val,
                                           _countof(val), m_iniPath.c_str());
        if (r == 0) break;
        if (WideToUtf8(val) == userId) {
            WritePrivateProfileStringW(kSection, key, nullptr, m_iniPath.c_str());
            removed = true;
        }
    }
    if (removed) {
        m_users.erase(std::remove(m_users.begin(), m_users.end(), userId), m_users.end());
    }
    return removed;
}

bool CommentNG::AddRegex(const std::string& pattern)
{
    if (pattern.empty() || m_iniPath.empty()) return false;
    for (const auto& r : m_regexes) {
        if (r.pattern == pattern) return false;
    }
    RegexRule rule;
    try {
        rule = RegexRule{ pattern, std::regex(pattern) };
    } catch (const std::regex_error&) {
        return false;
    }

    int idx = NextFreeIndex(L"Regex");
    wchar_t key[64];
    swprintf_s(key, L"Regex%d", idx);
    if (!WritePrivateProfileStringW(kSection, key, Utf8ToWide(pattern).c_str(), m_iniPath.c_str())) {
        return false;
    }
    m_regexes.push_back(std::move(rule));
    return true;
}
