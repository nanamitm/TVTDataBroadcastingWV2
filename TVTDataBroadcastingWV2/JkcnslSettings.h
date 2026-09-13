#pragma once
#include "pch.h"
#include <string>

// One-shot helper for applying/querying jkcnsl persistent settings (the 'S'
// command). jkcnsl reads its settings file at the start of each command, so a
// value written here takes effect on the next stream connection.
class JkcnslSettings
{
public:
    // jkcnslの応答を待つ既定の上限。ワーカーから呼ぶ場合の値。
    static constexpr DWORD kDefaultTimeoutMs = 8000;
    // UIスレッドを止めて待つ場合の上限 (モーダルダイアログなど)。
    static constexpr DWORD kUiTimeoutMs = 3000;

    struct LoginInfo {
        bool        loggedIn = false; // a nicovideo_cookie is stored
        std::string cacheServerUrl;   // jkcnsl cache_server_url (empty => nicovideo)
    };

    // Query jkcnsl's current login state ("S" with no argument). Returns false
    // if the query itself failed (jkcnsl missing / no response).
    // cancelEvent: signalling it aborts the wait (see RunCommand).
    // timeoutMs: 応答を待つ上限。UIスレッドから呼ぶならkUiTimeoutMsを渡す。
    static bool QueryLogin(const std::wstring& jkcnslPath, LoginInfo& out,
                           HANDLE cancelEvent = nullptr,
                           DWORD timeoutMs = kDefaultTimeoutMs);

    // Run a single jkcnsl command (e.g. "Scache_server_url https://...") then
    // quit. Returns true if jkcnsl acknowledged it with a '.' terminator.
    // Optionally captures jkcnsl's '-' output lines (without the leading '-').
    // cancelEvent: 呼び出し元が終了したいときに合図するイベント。応答を待たずに
    // 打ち切るので、プラグイン無効化時にUIスレッドが待たされない。
    static bool RunCommand(const std::wstring& jkcnslPath,
                           const std::string& command,
                           std::string* output = nullptr,
                           HANDLE cancelEvent = nullptr,
                           DWORD timeoutMs = kDefaultTimeoutMs);

    // Set (non-empty) or clear (empty) jkcnsl's cache_server_url. When set,
    // 'L{chatStreamID}' streams route through that cache server (jkcnslが
    // {cache_server_url}/watch/{id} へ避難所プロトコルで繋ぐ); when cleared,
    // jkcnsl uses the direct nicovideo path. 利用者の設定なので、UIからの
    // 明示的な保存操作以外で呼ばないこと。
    static bool SetCacheServerUrl(const std::wstring& jkcnslPath, const std::string& url,
                                  DWORD timeoutMs = kDefaultTimeoutMs);
};
