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

void CALLBACK ProxyRequest::StaticAsyncCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
    auto self = (ProxyRequest*)dwContext;
    if (self)
    {
        self->AsyncCallback(hInternet, dwInternetStatus, lpvStatusInformation, dwStatusInformationLength);
    }
}

void ProxyRequest::Fail()
{
    if (errorCallback)
    {
        std::exchange(errorCallback, nullptr)();
        Close();
    }
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
            // 閉じた後の通知がこのオブジェクトに届かないようにしてから失敗させる
            DWORD_PTR context = 0;
            WinHttpSetOption(hInternet, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));
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
            callback(statusCode, statusText ? statusText.get() : L"", headers ? headers.get() : L"", data.size(), data.data());
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
    delete this;
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
    URL_COMPONENTSW components = { sizeof(URL_COMPONENTSW) };
    components.dwHostNameLength = 1;
    components.dwUrlPathLength = 1;
    if (!WinHttpCrackUrl(url, 0, 0, &components))
    {
        return false;
    }
    std::wstring hostName(components.lpszHostName, components.lpszHostName + components.dwHostNameLength);
    std::wstring urlPath(components.lpszUrlPath, components.lpszUrlPath + components.dwUrlPathLength);
    auto connect = WinHttpConnect(session.GetSession(), hostName.c_str(), components.nPort, 0);
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
    std::unique_ptr<ProxyRequest> preq(new ProxyRequest(connect, request, std::move(errorCallback), std::move(callback), std::move(payload), blockPrivateAddresses));
    if (WinHttpSetStatusCallback(request, StaticAsyncCallback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
    {
        return false;
    }
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
    if (!WinHttpSendRequest(request, payloadSize == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS : additionalHeader, -1, preq->payload.data(), static_cast<DWORD>(payloadSize), WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, (DWORD_PTR)preq.get()))
    {
        return false;
    }
    // AsyncCallback中でdelete thisされる
    preq.release();
    return true;
}

ProxyRequest::~ProxyRequest()
{
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
}

ProxySession::ProxySession(bool ignoreCertificateErrors) : ignoreCertificateErrors(ignoreCertificateErrors)
{
    this->session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
}

bool ProxySession::IgnoreCertificateErrors() const
{
    return this->ignoreCertificateErrors;
}

ProxySession::~ProxySession()
{
    WinHttpCloseHandle(this->session);
}

HINTERNET ProxySession::GetSession()
{
    return this->session;
}
