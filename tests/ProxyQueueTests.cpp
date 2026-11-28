#include "../TVTDataBroadcastingWV2/ProxyQueue.h"
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
QueuedProxyRequest Request(std::vector<DWORD>& statuses, size_t payload = 0)
{
    return {L"https://fixture.invalid/", L"POST", std::vector<BYTE>(payload), {},
        [&statuses](DWORD status, LPCWSTR, LPCWSTR, size_t, BYTE*) { statuses.push_back(status); }, false};
}
int main()
{
    try {
        std::vector<DWORD> statuses;
        unsigned starts = 0;
        auto start = [&](QueuedProxyRequest) { ++starts; };
        auto full = [] { return false; };
        auto free = [] { return true; };
        ProxyQueue queue({2, 4096, 100});
        Check(queue.TryPush(Request(statuses), 0) && queue.TryPush(Request(statuses), 1), "queue admits its configured count");
        auto rejected = Request(statuses);
        Check(!queue.TryPush(std::move(rejected), 2) && rejected.respond, "count rejection retains caller ownership");
        queue.Service(true, false, 99, full, start);
        Check(starts == 0 && statuses.empty(), "full session leaves unexpired requests waiting");
        queue.Service(true, false, 100, full, start);
        Check(queue.size() == 1 && statuses == std::vector<DWORD>{504}, "oldest request expires with no connection slot available");
        queue.Service(false, false, 100, free, start);
        Check(queue.empty() && queue.bytes() == 0 && starts == 0 && statuses.back() == 503,
            "communication OFF rejects pending POSTs and releases their budget");
        queue.Service(false, true, 100, free, start);
        Check(statuses.size() == 2, "a repeated drain cannot complete a request twice");
        Check(queue.TryPush(Request(statuses), 200), "capacity is reusable after disabling");
        queue.Service(true, false, 201, free, start);
        Check(starts == 1 && queue.empty() && queue.bytes() == 0, "fresh request starts after re-enable");
        Check(queue.TryPush(Request(statuses), 300), "new pending request admitted");
        queue.Service(true, false, 400, free, start);
        Check(starts == 1 && statuses.back() == 504, "expired request cannot start even when a slot becomes free");
        Check(queue.TryPush(Request(statuses), 500), "request admitted before finalization");
        queue.Service(true, true, 500, free, start);
        Check(starts == 1 && statuses.back() == 503, "finalization cancels waiting requests while enabled");
        ProxyQueue measured;
        Check(measured.TryPush(Request(statuses, 256), 0), "measure one request");
        const auto cost = measured.bytes();
        ProxyQueue limited({64, cost, 100});
        Check(limited.TryPush(Request(statuses, 256), 0), "exact retained-byte budget admitted");
        Check(!limited.TryPush(Request(statuses), 0), "aggregate retained-byte budget enforced");
        auto headers = Request(statuses);
        headers.headers.emplace_back(L"Content-Type", std::wstring(4096, L'x'));
        ProxyQueue headerLimit({64, 1024, 100});
        Check(!headerLimit.TryPush(std::move(headers), 0), "headers count toward retained bytes");
        auto reserved = Request(statuses, 8192); reserved.payload.resize(1);
        Check(!headerLimit.TryPush(std::move(reserved), 0), "short reads cannot hide reserved payload capacity");
        ProxyQueue fifo;
        for (BYTE i = 0; i < 3; ++i) {
            auto request = Request(statuses, 1); request.payload[0] = i;
            Check(fifo.TryPush(std::move(request), i), "FIFO request admitted");
        }
        std::vector<BYTE> order;
        fifo.Service(true, false, 4, free, [&](QueuedProxyRequest request) { order.push_back(request.payload[0]); });
        Check(order == std::vector<BYTE>({0, 1, 2}), "non-expired requests retain FIFO order");
        std::cout << "Proxy queue disable, limits, expiry, ownership and FIFO tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
