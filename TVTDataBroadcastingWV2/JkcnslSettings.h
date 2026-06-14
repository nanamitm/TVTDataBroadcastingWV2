#pragma once
#include "pch.h"
#include <string>

// One-shot helper for applying/querying jkcnsl persistent settings (the 'S'
// command). jkcnsl reads its settings file at the start of each command, so a
// value written here takes effect on the next stream connection.
class JkcnslSettings
{
public:
    struct LoginInfo {
        bool        loggedIn = false; // a nicovideo_cookie is stored
        std::string mail;             // configured login mail (may be empty)
        std::string cacheServerUrl;   // jkcnsl cache_server_url (empty => nicovideo)
    };

    // Query jkcnsl's current login state ("S" with no argument). Returns false
    // if the query itself failed (jkcnsl missing / no response).
    static bool QueryLogin(const std::wstring& jkcnslPath, LoginInfo& out);

    // Run a single jkcnsl command (e.g. "Scache_server_url https://...") then
    // quit. Returns true if jkcnsl acknowledged it with a '.' terminator.
    // Optionally captures jkcnsl's '-' output lines (without the leading '-').
    static bool RunCommand(const std::wstring& jkcnslPath,
                           const std::string& command,
                           std::string* output = nullptr);

    // Set (non-empty) or clear (empty) jkcnsl's cache_server_url. When set,
    // 'L{jkChannel}' streams route through that cache/refuge server (e.g.
    // NX-Jikkyo); when cleared, jkcnsl uses the direct nicovideo path.
    static bool SetCacheServerUrl(const std::wstring& jkcnslPath, const std::string& url);
};
