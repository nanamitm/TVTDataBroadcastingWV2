#include "../../TVTDataBroadcastingWV2/pch.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <winhttp.h>
#include <winsock2.h>
#include <iostream>
#include <thread>
#include "../../TVTDataBroadcastingWV2/AsyncLifetime.h"
#include "../../TVTDataBroadcastingWV2/ProxyPolicy.h"
#include "../../TVTDataBroadcastingWV2/B62Navigation.h"
#include "../../TVTDataBroadcastingWV2/B62RomSound.h"
#include <string>
// Expose only our driver types for deterministic completion fixtures. External
// headers are included first, and the real WinHTTP path is tested below too.
#define class struct
#include "../../TVTDataBroadcastingWV2/proxy.h"
#undef class
static bool intercept = false;
static LPVOID destination = nullptr;
static std::string fragment;
BOOL WINAPI TestReadData(HINTERNET request, LPVOID buffer, DWORD length, LPDWORD read) {
    if (!intercept) return ::WinHttpReadData(request, buffer, length, read);
    destination = buffer;
    memcpy(buffer, fragment.data(), (std::min)(fragment.size(), static_cast<size_t>(length)));
    return TRUE;
}
BOOL WINAPI TestAvailable(HINTERNET request, LPDWORD size) {
    return intercept ? TRUE : ::WinHttpQueryDataAvailable(request, size);
}
HINTERNET WINAPI TestOpen(LPCWSTR agent, DWORD, LPCWSTR, LPCWSTR, DWORD flags) {
    return ::WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, flags);
}
#define WinHttpOpen TestOpen
#define WinHttpReadData TestReadData
#define WinHttpQueryDataAvailable TestAvailable
#include "../../TVTDataBroadcastingWV2/proxy.cpp"
#undef WinHttpOpen
#undef WinHttpReadData
#undef WinHttpQueryDataAvailable
static unsigned checks = 0;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static bool waitFor(const std::function<bool()>& ready) {
    const ULONGLONG end = GetTickCount64() + 5000;
    do { if (ready()) return true; Sleep(1); } while (GetTickCount64() < end);
    return false;
}
struct Outcome {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<unsigned> failures{0}, successes{0};
    std::string body;
    ~Outcome() { CloseHandle(ready); }
};
struct Server {
    SOCKET listener;
    std::thread worker;
    std::atomic<bool> received{false}, accepted{false};
    unsigned short port;
    Server(std::string response, bool hold = false) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "fixture bind");
        check(listen(listener, 8) == 0, "fixture listen");
        int length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        worker = std::thread([this, response, hold] {
            fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval wait{5, 0};
            if (select(0, &set, nullptr, nullptr, &wait) <= 0) return;
            SOCKET client = accept(listener, nullptr, nullptr); accepted = true;
            DWORD timeout = 5000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
            char buffer[4096]; std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                const int n = recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, n);
            }
            received = !request.empty();
            if (received) {
                size_t sent = 0;
                while (sent < response.size()) {
                    const int n = send(client, response.data() + sent, (int)(response.size() - sent), 0);
                    if (n <= 0) break;
                    sent += n;
                }
                if (hold) recv(client, buffer, sizeof(buffer), 0); // cancellation releases this wait
            }
            shutdown(client, SD_BOTH); closesocket(client);
        });
    }
    ~Server() { worker.join(); closesocket(listener); }
    std::wstring url() const { return L"http://127.0.0.1:" + std::to_wstring(port) + L"/fixture"; }
};
static void localProxy(ProxySession& session) {
    check(session.GetSession() != nullptr && session.GetSession(true) != nullptr, "create real WinHTTP fixture sessions");
}
static bool start(ProxySession& session, const std::wstring& url, std::shared_ptr<Outcome> outcome,
                  bool restricted = false, std::vector<BYTE> payload = {}, LPCWSTR verb = L"GET") {
    std::vector<std::pair<LPCWSTR, LPCWSTR>> headers;
    return ProxyRequest::RequestAsync(session, url.c_str(), verb, std::move(payload), headers,
        [outcome] { ++outcome->failures; SetEvent(outcome->ready); },
        [outcome](DWORD status, LPCWSTR, LPCWSTR, size_t size, BYTE* body) {
            outcome->body = size ? std::string(reinterpret_cast<char*>(body), size) : "";
            ++outcome->successes; SetEvent(outcome->ready);
        }, restricted);
}
int main() {
    WSADATA wsa; check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock startup");
    const std::wstring host = L"https://TVTDataBroadcastingWV2.invalid";
    check(ProxyPolicy::LegacyBridgeAllowed(host, L""), "trusted Origin allowed");
    check(ProxyPolicy::LegacyBridgeAllowed(L"", host + L"/index.html"), "trusted Referer fallback");
    check(!ProxyPolicy::LegacyBridgeAllowed(L"", L""), "missing request source denied");
    check(!ProxyPolicy::LegacyBridgeAllowed(L"https://tvtdatabroadcastingwv2-b62.invalid", host), "B62 Origin cannot fall back to host Referer");
    {
        using B62Navigation::SplitUrl;
        const auto page = SplitUrl(L"https://shv.nhk.jp/4k-commu/source/index4k.html?t=1792021519575#top");
        check(page && page->first == L"https://shv.nhk.jp" &&
            page->second == L"4k-commu/source/index4k.html?t=1792021519575", "navigation keeps path and query, drops fragment");
        const auto port = SplitUrl(L"HTTP://Example.jp:8080?q=1");
        check(port && port->first == L"HTTP://Example.jp:8080" && port->second == L"?q=1", "navigation origin with port and no path");
        check(!SplitUrl(L"https://user@evil.example/"), "navigation rejects credentials");
        check(!SplitUrl(L"ftp://example.jp/"), "navigation rejects other schemes");
        check(!SplitUrl(L"https:///path"), "navigation rejects an empty host");
        check(!SplitUrl(L"https://example.jp/a\r\nLocation: x"), "navigation rejects header control characters");
        check(B62Navigation::ScriptStringLiteral(L"https://a.invalid/p?t=1&x=<\"'\\") ==
            L"\"https://a.invalid/p?t=1&x=\\u003c\\u0022\\u0027\\u005c\"", "redirect script string is escaped");
    }
    {
        const auto wav = B62RomSound::SilentWav();
        auto u32 = [&](size_t at) { return wav[at] | (wav[at + 1] << 8) | (wav[at + 2] << 16) | (static_cast<uint32_t>(wav[at + 3]) << 24); };
        check(wav.size() == 44 + 160 && std::string(wav.begin(), wav.begin() + 4) == "RIFF" &&
            std::string(wav.begin() + 8, wav.begin() + 16) == "WAVEfmt " && std::string(wav.begin() + 36, wav.begin() + 40) == "data",
            "romsound silence is a WAV file");
        check(u32(4) == wav.size() - 8 && u32(40) == wav.size() - 44 && u32(24) == 8000,
            "romsound silence sizes and rate");
        bool silent = true;
        for (size_t i = 44; i < wav.size(); i++) silent = silent && wav[i] == 0;
        check(silent, "romsound silence has only zero samples");
    }
    check(!ProxyPolicy::LegacyBridgeAllowed(L"null", host), "opaque Origin denied");
    check(!ProxyPolicy::LegacyBridgeAllowed(L"", host + L".evil/path"), "host-prefix impersonation denied");
    check(!ProxyPolicy::LegacyBridgeAllowed(L"", L"http://TVTDataBroadcastingWV2.invalid/"), "wrong scheme denied");
    for (const auto* address : {L"127.0.0.1", L"10.1.2.3", L"192.168.1.1", L"::1", L"::ffff:127.0.0.1", L"fe80::1", L"fc00::1"})
        check(ProxyRequest::IsPrivateAddress(address), "private address blocked");
    check(!ProxyRequest::IsPrivateAddress(L"8.8.8.8"), "public IPv4 classified correctly");
    check(!ProxyRequest::IsPrivateAddress(L"2606:4700::1111"), "public IPv6 classified correctly");
    {
        AsyncLifetime lifetime; lifetime.Begin(); auto old = lifetime.GetTicket();
        auto held = old.lock(); check(AsyncLifetime::IsCurrent(old), "current initialization ticket");
        lifetime.Cancel(); check(!AsyncLifetime::IsCurrent(old), "cancel even a strongly held ticket");
        lifetime.Begin(); check(!AsyncLifetime::IsCurrent(old) && AsyncLifetime::IsCurrent(lifetime.GetTicket()), "re-enable rejects old callback generation");
        AsyncLifetime::Ticket destroyed;
        { AsyncLifetime owner; owner.Begin(); destroyed = owner.GetTicket(); }
        check(!AsyncLifetime::IsCurrent(destroyed), "destroyed owner ticket rejected");
    }
    {
        intercept = true; unsigned errors = 0;
        auto request = std::make_shared<ProxyRequest>(nullptr, nullptr, [&] { ++errors; },
            [](DWORD, LPCWSTR, LPCWSTR, size_t, BYTE*) {}, std::vector<BYTE>{}, false);
        request->state = std::make_shared<ProxySessionState>();
        DWORD available = 8; fragment = "ABC";
        request->AsyncCallback(nullptr, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &available, sizeof(available));
        request->AsyncCallback(nullptr, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, destination, 3);
        check(std::string(request->data.begin(), request->data.end()) == "ABC", "short read discards reserved zero tail");
        available = 4; fragment = "DE";
        request->AsyncCallback(nullptr, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &available, sizeof(available));
        request->AsyncCallback(nullptr, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, destination, 2);
        check(std::string(request->data.begin(), request->data.end()) == "ABCDE", "next read appends at actual end");
        request->state->limits.responseBytes = 5; available = 1;
        request->AsyncCallback(nullptr, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, &available, sizeof(available));
        check(errors == 1 && request->data.size() == 5, "response limit fails before allocation");
        intercept = false;
    }
    {
        Server server("HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\nABCDEFGH");
        ProxySession session(false, {8, 4, 1}); localProxy(session);
        auto outcome = std::make_shared<Outcome>();
        check(start(session, server.url(), outcome), "start valid HTTP request");
        check(WaitForSingleObject(outcome->ready, 5000) == WAIT_OBJECT_0, "valid HTTP completion");
        session.Shutdown();
        check(outcome->successes == 1 && outcome->failures == 0 && outcome->body == "ABCDEFGH", "exact response limit retains bytes");
        check(session.state->requests.empty() && session.state->active == 0, "final close notification drained");
        check(!start(session, server.url(), outcome), "shutdown rejects new requests");
    }
    for (const auto* status : {"200 OK", "204 No Content", "304 Not Modified"}) {
        Server server(std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 100\r\nConnection: close\r\n\r\n");
        ProxySession session(false, {8, 4, 1}); localProxy(session);
        auto outcome = std::make_shared<Outcome>();
        check(start(session, server.url(), outcome, false, {}, std::string(status).starts_with("200") ? L"HEAD" : L"GET"), "start bodyless response");
        check(WaitForSingleObject(outcome->ready, 5000) == WAIT_OBJECT_0, "bodyless response completes");
        session.Shutdown(); check(outcome->successes == 1 && outcome->body.empty(), "bodyless Content-Length is metadata, not received bytes");
    }
    for (const std::string response : {
        "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\nABCDEFGHI",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n4\r\nABCD\r\n4\r\nEFGH\r\n1\r\nI\r\n0\r\n\r\n"}) {
        Server server(response); ProxySession session(false, {8, 4, 1}); localProxy(session);
        auto outcome = std::make_shared<Outcome>(); check(start(session, server.url(), outcome), "start oversized response");
        check(WaitForSingleObject(outcome->ready, 5000) == WAIT_OBJECT_0, "oversized response completion");
        session.Shutdown(); check(outcome->failures == 1 && outcome->successes == 0, "declared/chunked limit rejects response once");
    }
    {
        Server server("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nA", true);
        ProxySession session(false, {128, 4, 1}); localProxy(session);
        auto outcome = std::make_shared<Outcome>(); check(start(session, server.url(), outcome), "start pending request");
        check(waitFor([&] { return server.received.load(); }), "pending server received request");
        auto rejected = std::make_shared<Outcome>();
        check(!start(session, server.url(), rejected), "concurrent request cap enforced");
        check(!start(session, server.url(), rejected, false, std::vector<BYTE>(5)), "upload cap enforced");
        session.Shutdown();
        check(outcome->failures == 1 && outcome->successes == 0 && session.state->requests.empty(), "cancel pending read and retain context through close");
        const auto count = outcome->failures.load(); Sleep(30);
        check(outcome->failures == count, "no owner callback after shutdown");
    }
    {
        Server server("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nA", true);
        ProxySession session(false, {128, 4, 8}); localProxy(session);
        std::vector<std::shared_ptr<Outcome>> outcomes;
        for (int i = 0; i < 8; ++i) {
            auto outcome = std::make_shared<Outcome>();
            check(start(session, server.url(), outcome), "start concurrent cancellation fixture");
            outcomes.push_back(std::move(outcome));
        }
        check(waitFor([&] { return server.received.load(); }), "concurrent fixture has live pending I/O");
        session.Shutdown();
        check(std::all_of(outcomes.begin(), outcomes.end(), [](const auto& outcome) {
            return outcome->failures == 1 && outcome->successes == 0;
        }), "all eight requests cancel exactly once");
        check(session.state->requests.empty() && session.state->active == 0, "all concurrent close notifications drained");
    }
    {
        Server server("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        ProxySession session; localProxy(session); auto outcome = std::make_shared<Outcome>();
        check(session.GetSession() != session.GetSession(true), "B62 has isolated connection pool");
        check(start(session, server.url(), outcome, true), "start restricted request");
        check(WaitForSingleObject(outcome->ready, 5000) == WAIT_OBJECT_0, "restricted connection completes");
        session.Shutdown(); check(outcome->failures == 1 && !server.received, "private peer receives no HTTP request");
    }
    WSACleanup(); std::cout << "Proxy/lifetime safety tests passed: " << checks << " checks\n";
}
