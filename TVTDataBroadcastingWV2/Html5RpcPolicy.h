#pragma once
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include "thirdparty/json.hpp"

namespace Html5RpcPolicy {
struct Request {
    std::string method;
    nlohmann::json params;
};
inline std::optional<Request> ParseRequest(std::string_view body) {
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("method") || !json["method"].is_string()) return std::nullopt;
    Request request{json["method"].get<std::string>(), json.value("params", nlohmann::json())};
    if (request.method.empty() || request.method.size() > 64
        || (!request.params.is_null() && !request.params.is_object())) return std::nullopt;
    const auto& p = request.params;
    auto string = [&](const char* key) { return p.is_object() && p.contains(key) && p[key].is_string(); };
    if (request.method == "storageGet" || request.method == "storageSet" || request.method == "storageRemove") {
        if (!string("key") || (request.method == "storageSet" && !string("value"))) return std::nullopt;
    } else if (request.method == "setGreg") {
        if (!string("value") || !p.contains("index") || !p["index"].is_number_integer()
            || p["index"] < 0 || p["index"] > 63) return std::nullopt;
    } else if (request.method == "readPersistentArray" || request.method == "writePersistentArray") {
        if (!string("filename") || (p.contains("structure") && !p["structure"].is_null() && !string("structure"))) return std::nullopt;
        if (request.method == "writePersistentArray" && (!string("structure") || !p.contains("data") || !p["data"].is_array())) return std::nullopt;
    }
    return request;
}
struct Url {
    INTERNET_SCHEME scheme;
    INTERNET_PORT port;
    std::wstring host, path;
};
inline std::optional<Url> ParseUrl(const std::wstring& value) {
    if (value.find(L'\0') != std::wstring::npos) return std::nullopt;
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(value.c_str(), 0, 0, &parts)
        || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
        || !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength) return std::nullopt;
    Url result{parts.nScheme, parts.nPort, {parts.lpszHostName, parts.dwHostNameLength},
        parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/"};
    for (auto& c : result.host) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return result;
}
inline std::optional<std::wstring> Wide(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::nullopt;
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
inline int MatchBoundary(const Url& url, const std::string& boundary) {
    auto wide = Wide(boundary);
    if (!wide) return -1;
    const bool scheme = wide->find(L"://") != std::wstring::npos;
    const auto text = scheme ? *wide : L"http://" + *wide;
    auto other = ParseUrl(text);
    if (!other || (scheme && other->scheme != url.scheme)) return -1;
    const auto start = text.find(L"://") + 3;
    const auto end = text.find_first_of(L"/?#", start);
    const auto authority = text.substr(start, end - start);
    const bool explicitPort = authority.starts_with(L"[") ? authority.find(L"]:") != std::wstring::npos
        : authority.find(L':') != std::wstring::npos;
    if ((scheme || explicitPort) && other->port != url.port) return -1;
    const bool path = other->path != L"/" || (scheme && end != std::wstring::npos && text[end] == L'/');
    if (path) return url.host == other->host && url.path.starts_with(other->path)
        ? 1000 + static_cast<int>(other->path.size()) : -1;
    return url.host == other->host || url.host.ends_with(L"." + other->host)
        ? static_cast<int>(other->host.size()) : -1;
}
// Same longest-boundary rule as the injected runtime, evaluated on trusted native state.
inline int Permission(const std::wstring& source, const nlohmann::json& boundaries, bool unmanaged) {
    auto url = ParseUrl(source);
    if (unmanaged || !url || !boundaries.is_array()) return -1;
    if (boundaries.empty()) return 0x1fff;
    int best = -3, permission = -1;
    for (const auto& boundary : boundaries) {
        if (!boundary.is_object() || !boundary.contains("managedURLs") || !boundary["managedURLs"].is_array()
            || !boundary.contains("permissionBitmaps") || !boundary["permissionBitmaps"].is_array()) continue;
        int bitmap = boundary["permissionBitmaps"].empty() ? 0 : -1;
        for (const auto& value : boundary["permissionBitmaps"]) {
            if (value.is_number_integer() && value >= 0 && value <= 0x1fff) { bitmap = value.get<int>(); break; }
        }
        if (bitmap < 0) continue;
        const auto& urls = boundary["managedURLs"];
        int score = urls.empty() ? -2 : -3;
        for (const auto& value : urls) if (value.is_string()) score = (std::max)(score, MatchBoundary(*url, value.get<std::string>()));
        if ((score >= 0 || urls.empty()) && score > best) { best = score; permission = bitmap; }
    }
    return permission;
}
inline bool Allowed(int permission, std::string_view method) {
    if (permission < 0) return false;
    int required;
    if (method == "readPersistentArray" || method == "writePersistentArray" || method == "storageGet"
        || method == "storageSet" || method == "storageRemove") required = 1 << 9;
    else if (method == "getCurrentEvent" || method == "getBroadcastAIT") required = 1 << 10;
    else if (method == "isCommonEvent" || method == "storeDataResource") required = 1 << 5;
    else if (method == "confirmIPNetwork") required = 1 << 6;
    else if (method == "getGreg" || method == "setGreg" || method == "isRecord") required = 0;
    else return false;
    return (permission & required) == required;
}
}
