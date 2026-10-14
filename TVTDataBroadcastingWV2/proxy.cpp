#include "pch.h"
#include "proxy.h"
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

ProxyRequest::ProxyRequest
(
    HINTERNET connect,
    HINTERNET request,
    std::function<void()> errorCallback,
    std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback,
    std::vector<BYTE> payload,
    bool blockPrivateAddresses
) : connect(connect), request(request), errorCallback(std::move(errorCallback)), callback(std::move(callback)), payload(std::move(payload)),
    blockPrivateAddresses(blockPrivateAddresses)
{

}

bool ProxyRequest::IsPrivateAddress(LPCWSTR address)
{
    if (!address)
    {
        return true;
    }
    // IPv6はスコープ (%n) を除いて読む
    std::wstring text(address);
    if (const auto scope = text.find(L'%'); scope != std::wstring::npos)
    {
        text.resize(scope);
    }
    IN_ADDR v4{};
    IN6_ADDR v6{};
    const uint8_t* b;
    if (InetPtonW(AF_INET, text.c_str(), &v4) == 1)
    {
        b = reinterpret_cast<const uint8_t*>(&v4);
    }
    else if (InetPtonW(AF_INET6, text.c_str(), &v6) == 1)
    {
        const auto* a = v6.u.Byte;
        // ::ffff:a.b.c.d はIPv4として調べる
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (std::equal(a, a + 12, mapped))
        {
            b = a + 12;
        }
        else
        {
            static const uint8_t zero[16] = {};
            const bool unspecifiedOrLoopback = std::equal(a, a + 15, zero) && (a[15] == 0 || a[15] == 1);
            return unspecifiedOrLoopback ||
                (a[0] & 0xFE) == 0xFC ||                 // fc00::/7 ユニークローカル
                (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) || // fe80::/10 リンクローカル
                (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) || // fec0::/10 サイトローカル
                a[0] == 0xFF;                            // マルチキャスト
        }
    }
    else
    {
        // 読めないものは通さない
        return true;
    }
    return b[0] == 0 ||                                  // 0.0.0.0/8
        b[0] == 10 ||                                    // 10.0.0.0/8
        b[0] == 127 ||                                   // ループバック
        (b[0] == 100 && (b[1] & 0xC0) == 64) ||          // 100.64.0.0/10 (CGN)
        (b[0] == 169 && b[1] == 254) ||                  // リンクローカル
        (b[0] == 172 && (b[1] & 0xF0) == 16) ||          // 172.16.0.0/12
        (b[0] == 192 && b[1] == 168) ||                  // 192.168.0.0/16
        (b[0] == 192 && b[1] == 0 && b[2] == 0) ||       // 192.0.0.0/24
        (b[0] == 198 && (b[1] & 0xFE) == 18) ||          // 198.18.0.0/15
        b[0] >= 224;                                     // マルチキャスト、予約、ブロードキャスト
}

namespace {
// Also counts RequestAsync setup: shutdown cannot close the session while a
// request is being opened but has not yet joined the registry.
struct ProxyActivity {
    std::shared_ptr<ProxySessionState> state;
    explicit ProxyActivity(std::shared_ptr<ProxySessionState> state) : state(std::move(state)) {}
    ~ProxyActivity() {
        std::lock_guard lock(state->mutex);
        --state->active;
        state->idle.notify_all();
    }
};
}

void CALLBACK ProxyRequest::StaticAsyncCallback(HINTERNET hInternet, DWORD_PTR context,
    DWORD status, LPVOID information, DWORD length)
{
    if (!context) return;
    // The registry holds this object until HANDLE_CLOSING, which WinHTTP
    // guarantees is the last notification and cannot overlap another one.
    auto state = reinterpret_cast<ProxyRequest*>(context)->state;
    {
        std::lock_guard lock(state->mutex);
        ++state->active;
    }
    ProxyActivity activity(state);
    auto self = reinterpret_cast<ProxyRequest*>(context)->shared_from_this();
    std::lock_guard lock(self->mutex);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        self->bound = false;
        if (self->connect) WinHttpCloseHandle(std::exchange(self->connect, nullptr));
        std::lock_guard sessionLock(self->state->mutex);
        self->state->requests.erase(self.get());
        return;
    }
    if (self->closing) return;
    try { self->AsyncCallback(hInternet, status, information, length); }
    catch (...) { self->Fail(); } // Never unwind through a Windows callback.
}

void ProxyRequest::Fail()
{
    if (!completed) {
        completed = true;
        callback = nullptr;
        auto error = std::exchange(errorCallback, nullptr);
        try { if (error) error(); } catch (...) {}
    }
    Close();
}

void ProxyRequest::Cancel()
{
    std::lock_guard lock(mutex);
    Fail();
}

void ProxyRequest::AsyncCallback(HINTERNET hInternet, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
    switch (dwInternetStatus)
    {
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
    {
        // 名前解決やリダイレクトの後に実際につながった先を調べる
        if (blockPrivateAddresses && IsPrivateAddress(static_cast<LPCWSTR>(lpvStatusInformation)))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    {
        if (!WinHttpReceiveResponse(hInternet, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    {
        if (!WinHttpQueryDataAvailable(hInternet, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    {
        auto const size = *(DWORD*)lpvStatusInformation;
        if (size == 0)
        {
            DWORD statusCode = 0, statusCodeSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusCodeSize, nullptr))
            {
                Fail();
                return;
            }
            DWORD statusTextSize;
            std::unique_ptr<WCHAR[]> statusText;
            if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_TEXT, nullptr, WINHTTP_NO_OUTPUT_BUFFER, &statusTextSize, nullptr) && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                statusText = std::unique_ptr<WCHAR[]>(new WCHAR[statusTextSize / sizeof(WCHAR)]);
                if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_TEXT, nullptr, statusText.get(), &statusTextSize, nullptr))
                {
                    Fail();
                    return;
                }
            }
            DWORD headersSize;
            std::unique_ptr<WCHAR[]> headers;
            if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &headersSize, WINHTTP_NO_HEADER_INDEX) && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                headers = std::unique_ptr<WCHAR[]>(new WCHAR[headersSize / sizeof(WCHAR)]);
                if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, headers.get(), &headersSize, WINHTTP_NO_HEADER_INDEX))
                {
                    Fail();
                    return;
                }
            }
            // ステータス行やヘッダが取得できないことがある (HTTP/2など)
            auto success = std::exchange(callback, nullptr);
            if (success) success(statusCode, statusText ? statusText.get() : L"", headers ? headers.get() : L"", data.size(), data.data());
            completed = true;
            errorCallback = nullptr;
            Close();
            break;
        }
        auto prevSize = data.size();
        data.resize(prevSize + size);
        if (!WinHttpReadData(hInternet, data.data() + prevSize, size, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_FLAG_READ_COMPLETE:
    {
        if (!WinHttpQueryDataAvailable(hInternet, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
        auto asyncResut = (LPWINHTTP_ASYNC_RESULT*)lpvStatusInformation;
        Fail();
        break;
    }
    }
}

void ProxyRequest::Close()
{
    if (closing) return;
    closing = true;
    if (request) WinHttpCloseHandle(std::exchange(request, nullptr));
    if (!bound && state) {
        if (connect) WinHttpCloseHandle(std::exchange(connect, nullptr));
        std::lock_guard lock(state->mutex);
        state->requests.erase(this);
        state->idle.notify_all();
    }
}

bool ProxyRequest::RequestAsync
(
    ProxySession& session,
    LPCWSTR url,
    LPCWSTR verb,
    std::vector<BYTE> payload,
    std::vector<std::pair<LPCWSTR, LPCWSTR>>& headers,
    std::function<void()> errorCallback,
    std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback,
    bool blockPrivateAddresses
)
{
    auto sharedState = session.state;
    {
        std::lock_guard lock(sharedState->mutex);
        if (sharedState->stopping) return false;
        ++sharedState->active;
    }
    ProxyActivity activity(sharedState);
    URL_COMPONENTSW components = { sizeof(URL_COMPONENTSW) };
    components.dwHostNameLength = 1;
    components.dwUrlPathLength = 1;
    if (!WinHttpCrackUrl(url, 0, 0, &components))
    {
        return false;
    }
    std::wstring hostName(components.lpszHostName, components.lpszHostName + components.dwHostNameLength);
    std::wstring urlPath(components.lpszUrlPath, components.lpszUrlPath + components.dwUrlPathLength);
    auto connect = WinHttpConnect(session.GetSession(blockPrivateAddresses), hostName.c_str(), components.nPort, 0);
    if (!connect)
    {
        return false;
    }
    LPCWSTR acceptTypes[] = { L"*/*", nullptr };
    auto request = WinHttpOpenRequest(connect, verb, urlPath.c_str(), nullptr, WINHTTP_NO_REFERER, acceptTypes, components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        return false;
    }
    if (session.IgnoreCertificateErrors())
    {
        // 証明書の検証を無効にすると通信内容を第三者に読み書きされうるので、
        // 既定では有効のままにしてINIで明示的に指定された場合だけ無視する
        DWORD securityFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    }
    std::shared_ptr<ProxyRequest> preq(new ProxyRequest(connect, request, std::move(errorCallback), std::move(callback), std::move(payload), blockPrivateAddresses));
    for (auto&& header : headers)
    {
        auto headers = std::wstring(header.first) + L": " + header.second;
        // CRLFが含まれている不正なヘッダを除外
        // CRLFの直後に空白かタブがあればヘッダの区切りではなくトークンの区切りとして扱われるためこの処理は正しくない
        // ただし含めたリクエストを送る処理はないしfetchに含めることもできないし問題ない
        if (headers.find(L'\r') != std::wstring::npos || headers.find(L'\n') != std::wstring::npos)
        {
            continue;
        }
        if (!WinHttpAddRequestHeaders(request, headers.data(), static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD))
        {
            return false;
        }
    }
    auto payloadSize = preq->payload.size();
    if (payloadSize > std::numeric_limits<DWORD>::max())
    {
        return false;
    }
    WCHAR additionalHeader[100];
    if (payloadSize != 0)
    {
        swprintf_s(additionalHeader, L"Content-Length: %zu", payloadSize);
    }
    preq->state = sharedState;
    std::lock_guard requestLock(preq->mutex);
    {
        std::lock_guard lock(sharedState->mutex);
        if (sharedState->stopping) return false;
        sharedState->requests.emplace(preq.get(), preq);
    }
    if (WinHttpSetStatusCallback(request, StaticAsyncCallback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER | WINHTTP_CALLBACK_FLAG_HANDLES,
        0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        preq->Close();
        return false;
    }
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(preq.get());
    if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
        preq->Close();
        return false;
    }
    preq->bound = true;
    if (blockPrivateAddresses) {
        DWORD features = WINHTTP_DISABLE_KEEP_ALIVE;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features))) {
            preq->Close();
            return false;
        }
    }
    if (!WinHttpSendRequest(request, payloadSize == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS : additionalHeader,
        -1, preq->payload.data(), static_cast<DWORD>(payloadSize), WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, context)) {
        const bool reported = preq->completed;
        preq->Close();
        return reported;
    }
    return true;
}

ProxyRequest::~ProxyRequest()
{
    // An unbound startup failure has no context callbacks. Bound requests have
    // already closed both handles in their final notification.
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
}

ProxySession::ProxySession(bool ignoreCertificateErrors)
    : state(std::make_shared<ProxySessionState>()), ignoreCertificateErrors(ignoreCertificateErrors)
{
    state->session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    // Never share a pooled BML connection with address-restricted B62 traffic.
    state->protectedSession = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
}

bool ProxySession::IgnoreCertificateErrors() const { return ignoreCertificateErrors; }
HINTERNET ProxySession::GetSession(bool protectedRequest)
{
    return protectedRequest ? state->protectedSession : state->session;
}

void ProxySession::Shutdown()
{
    std::vector<std::shared_ptr<ProxyRequest>> pending;
    {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
        for (const auto& [_, request] : state->requests) pending.push_back(request);
    }
    for (const auto& request : pending) request->Cancel();
    pending.clear();
    std::unique_lock lock(state->mutex);
    state->idle.wait(lock, [&] { return state->requests.empty() && state->active == 0; });
    auto session = std::exchange(state->session, nullptr);
    auto protectedSession = std::exchange(state->protectedSession, nullptr);
    lock.unlock();
    if (session) WinHttpCloseHandle(session);
    if (protectedSession) WinHttpCloseHandle(protectedSession);
}

ProxySession::~ProxySession() { Shutdown(); }
