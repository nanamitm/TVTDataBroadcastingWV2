#pragma once
#include <winhttp.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class ProxyRequest;
struct ProxyLimits {
    size_t responseBytes = 16 * 1024 * 1024;
    size_t payloadBytes = 4 * 1024 * 1024;
    size_t concurrentRequests = 8;
};
struct ProxySessionState {
    std::mutex mutex;
    std::condition_variable idle;
    std::unordered_map<ProxyRequest*, std::shared_ptr<ProxyRequest>> requests;
    ProxyLimits limits;
    size_t active = 0;
    bool stopping = false;
    HINTERNET session = nullptr;
    HINTERNET protectedSession = nullptr;
};

class ProxySession {
    friend class ProxyRequest;
    std::shared_ptr<ProxySessionState> state;
    bool ignoreCertificateErrors;
public:
    HINTERNET GetSession(bool protectedRequest = false);
    bool IgnoreCertificateErrors() const;
    explicit ProxySession(bool ignoreCertificateErrors = false, ProxyLimits limits = {});
    ~ProxySession();
    void Shutdown();
    ProxySession(const ProxySession&) = delete;
    ProxySession& operator=(const ProxySession&) = delete;
};

class ProxyRequest : public std::enable_shared_from_this<ProxyRequest> {
    friend class ProxySession;
    std::shared_ptr<ProxySessionState> state;
    std::recursive_mutex mutex;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    bool bound = false;
    bool closing = false;
    bool completed = false;
    bool headerOnly = false;
    std::vector<BYTE> data;
    size_t readStart = 0;
    size_t readCapacity = 0;
    bool reading = false;
    std::function<void()> errorCallback;
    std::function<void(DWORD, LPCWSTR, LPCWSTR, size_t, BYTE*)> callback;
    std::vector<BYTE> payload;
    bool blockPrivateAddresses = false;
    ProxyRequest(HINTERNET connect, HINTERNET request,
        std::function<void()> errorCallback,
        std::function<void(DWORD, LPCWSTR, LPCWSTR, size_t, BYTE*)> callback,
        std::vector<BYTE> payload, bool blockPrivateAddresses);
    static void CALLBACK StaticAsyncCallback(HINTERNET, DWORD_PTR, DWORD, LPVOID, DWORD);
    void Fail();
    void AsyncCallback(HINTERNET, DWORD, LPVOID, DWORD);
    void Close();
    void Cancel();
public:
    // Completion runs on a WinHTTP callback thread. Shutdown waits for every
    // request's final HANDLE_CLOSING notification and all callback bodies.
    static bool RequestAsync(ProxySession& session, LPCWSTR url, LPCWSTR verb,
        std::vector<BYTE> payload, std::vector<std::pair<LPCWSTR, LPCWSTR>>& headers,
        std::function<void()> errorCallback,
        std::function<void(DWORD, LPCWSTR, LPCWSTR, size_t, BYTE*)> callback,
        bool blockPrivateAddresses = false);
    static bool IsPrivateAddress(LPCWSTR address);
    ~ProxyRequest();
    ProxyRequest(const ProxyRequest&) = delete;
    ProxyRequest& operator=(const ProxyRequest&) = delete;
};
