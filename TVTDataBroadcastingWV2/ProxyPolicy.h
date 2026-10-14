#pragma once
#include <string_view>

namespace ProxyPolicy {
inline bool EqualOrigin(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
    for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
inline bool LegacyBridgeAllowed(std::wstring_view origin, std::wstring_view referrer) {
    constexpr std::wstring_view trusted = L"https://TVTDataBroadcastingWV2.invalid";
    if (!origin.empty()) return EqualOrigin(origin, trusted);
    return referrer.size() >= trusted.size() && EqualOrigin(referrer.substr(0, trusted.size()), trusted) &&
        (referrer.size() == trusted.size() || referrer[trusted.size()] == L'/');
}
}
