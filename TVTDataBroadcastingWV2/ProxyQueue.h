#pragma once
#include <windows.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct QueuedProxyRequest
{
    std::wstring url;
    std::wstring method;
    std::vector<BYTE> payload;
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::function<void(DWORD, LPCWSTR, LPCWSTR, size_t, BYTE*)> respond;
    // Every caller's destination is chosen by broadcast or network content.
    bool blockPrivateAddresses = true;
};

// UI-thread queue. Bound both retained request data and the number of WebView
// deferrals, and expire requests even while all WinHTTP slots are occupied.
class ProxyQueue
{
public:
    struct Limits {
        size_t requests = 64;
        size_t bytes = 16 * 1024 * 1024;
        uint64_t waitMillis = 30000;
    };
    ProxyQueue() = default;
    explicit ProxyQueue(Limits limits) : limits(limits) {}
    bool empty() const { return requests.empty(); }
    size_t size() const { return requests.size(); }
    size_t bytes() const { return retainedBytes; }
    // On rejection leave ownership with the caller so it can complete the deferral.
    bool TryPush(QueuedProxyRequest&& request, uint64_t now)
    {
        if (requests.size() >= limits.requests) return false;
        size_t cost = sizeof(QueuedProxyRequest);
        const auto addBytes = [&](size_t bytes) {
            if (cost > limits.bytes || bytes > limits.bytes - cost) return false;
            cost += bytes;
            return true;
        };
        const auto add = [&](size_t characters) {
            if (cost > limits.bytes || characters > (limits.bytes - cost) / sizeof(wchar_t)) return false;
            cost += characters * sizeof(wchar_t);
            return true;
        };
        if (!addBytes(request.payload.capacity()) || !add(request.url.capacity()) || !add(request.method.capacity())) return false;
        if (cost > limits.bytes || request.headers.capacity() > (limits.bytes - cost) / sizeof(request.headers[0])) return false;
        cost += request.headers.capacity() * sizeof(request.headers[0]);
        for (const auto& [name, value] : request.headers)
            if (!add(name.capacity()) || !add(value.capacity())) return false;
        if (cost > limits.bytes || retainedBytes > limits.bytes - cost) return false;
        requests.push_back({std::move(request), now, cost});
        retainedBytes += cost;
        return true;
    }
    template<typename HasCapacity, typename Start>
    void Service(bool enabled, bool finish, uint64_t now, HasCapacity hasCapacity, Start start)
    {
        while (!requests.empty()) {
            const bool unavailable = finish || !enabled;
            const bool expired = now - requests.front().queuedAt >= limits.waitMillis;
            if (!unavailable && !expired && !hasCapacity()) break;
            auto entry = std::move(requests.front());
            requests.pop_front();
            retainedBytes -= entry.bytes;
            if (unavailable) entry.request.respond(503, L"Service Unavailable", L"", 0, nullptr);
            else if (expired) entry.request.respond(504, L"Gateway Timeout", L"", 0, nullptr);
            else start(std::move(entry.request));
        }
    }
private:
    struct Entry { QueuedProxyRequest request; uint64_t queuedAt; size_t bytes; };
    Limits limits;
    std::deque<Entry> requests;
    size_t retainedBytes = 0;
};
