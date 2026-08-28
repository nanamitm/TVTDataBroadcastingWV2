#pragma once
#include "pch.h"
#include "CommentFetcher.h"
#include <regex>
#include <string>
#include <vector>

// NG (あぼーん) filter for jikkyo comments.
//
// Rules are stored in the [NG] section of the plugin INI:
//   Regex0=, Regex1=, ...    regular expression matched against comment text
//   User0=,  User1=,  ...    exact user_id match
//   Command0=, Command1=, ... exact mail-field token match (e.g. "184")
//
// NG filtering works on the already-parsed Comment struct, which suits the
// WebView rendering pipeline. The [CustomReplace] substitutions below are
// separate and run over the raw <chat> tag, as NicoJK does.
class CommentNG
{
public:
    // (Re)load all NG rules from the [NG] section of iniPath.
    void Load(const std::wstring& iniPath);

    // Returns true if the comment should be hidden.
    bool IsNG(const Comment& c) const;
    // NG by user_id only.
    bool IsNGUser(const Comment& c) const;
    // NG by regex/command (everything except user_id).
    bool IsNGExceptUser(const Comment& c) const;

    // Current NG user_id list (for UI display).
    const std::vector<std::string>& GetUsers() const { return m_users; }

    // Add a user_id to the NG list and persist to the INI.
    // Returns false if the id is empty or already present.
    bool AddUser(const std::string& userId);

    // Remove a user_id from the NG list and persist. Returns false if absent.
    bool RemoveUser(const std::string& userId);

    // Add a text regex to the NG list and persist.
    // Returns false if the pattern is empty, invalid, or already present.
    bool AddRegex(const std::string& pattern);

    bool HasUser(const std::string& userId) const;

    // Apply the [CustomReplace] sed-style substitutions to a whole <chat> tag
    // in place, in ascending key order. Returns true if the tag was modified.
    bool ApplyReplace(std::string& tag) const;
    bool HasReplaces() const { return !m_replaces.empty(); }

    // Keys whose pattern could not be compiled at the last Load() (for logging).
    const std::vector<std::wstring>& GetReplaceErrors() const { return m_replaceErrors; }

private:
    struct RegexRule {
        std::string pattern; // original UTF-8 pattern (as stored in INI)
        std::regex  re;
    };
    struct ReplaceRule {
        int         key = 0; // {n} of Pattern{n}; rules run in ascending order
        std::regex  re;
        std::string fmt; // std::regex_replace format ($1 etc.)
    };

    std::wstring              m_iniPath;
    std::vector<RegexRule>    m_regexes;
    std::vector<std::string>  m_users;    // exact user_id match
    std::vector<std::string>  m_commands; // exact mail token match
    std::vector<ReplaceRule>  m_replaces; // [CustomReplace] tag substitutions
    std::vector<std::wstring> m_replaceErrors; // keys with an invalid pattern

    void LoadReplaces(const std::wstring& iniPath);

    // Find the smallest free numeric index for keyPrefix (e.g. "User") in [NG].
    int NextFreeIndex(const wchar_t* keyPrefix) const;

    static std::vector<std::string> SplitMailTokens(const std::string& mail);
    static std::string  WideToUtf8(const std::wstring& w);
    static std::wstring Utf8ToWide(const std::string& s);
};
