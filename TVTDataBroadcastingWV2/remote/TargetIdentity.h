#pragma once
// SPDX-License-Identifier: MIT
// Persistent routing identity, published to LAN clients. This is not a credential.
#include <windows.h>
#include <objbase.h>
#include <string>

namespace TvtRemote {
inline bool ValidTargetToken(const std::wstring& token) {
    if (token.empty() || token.size() > 64) return false;
    for (const auto c : token)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.')) return false;
    return true;
}
inline bool LoadOrCreateTargetToken(const std::wstring& ini, std::wstring& token) {
    // Serialize first-run generation when processes share a plugin INI.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\TVTDataBroadcastingRemote TargetIdentity v1");
    if (!mutex) return false;
    const auto wait = WaitForSingleObject(mutex, 5000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return false; }
    wchar_t value[66]{};
    const auto count = GetPrivateProfileStringW(L"TVTDataBroadcastingWV2", L"RemoteTargetToken", L"", value, 66, ini.c_str());
    token.assign(value, count);
    bool ok = ValidTargetToken(token);
    if (token.empty()) {
        GUID guid{}; wchar_t text[40]{};
        if (SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, text, 40)) {
            token.assign(text + 1, 36);
            ok = WritePrivateProfileStringW(L"TVTDataBroadcastingWV2", L"RemoteTargetToken", token.c_str(), ini.c_str()) != FALSE;
        }
    }
    ReleaseMutex(mutex); CloseHandle(mutex);
    if (!ok) token.clear();
    return ok;
}
}
