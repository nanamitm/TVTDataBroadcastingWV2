#pragma once
#include <winhttp.h>

class ProxySession
{
    HINTERNET session;
    bool ignoreCertificateErrors;
public:
    HINTERNET GetSession();
    // trueならサーバ証明書の検証エラーを無視する (INIのIgnoreCertificateErrors)
    bool IgnoreCertificateErrors() const;
    explicit ProxySession(bool ignoreCertificateErrors = false);
    ~ProxySession();
    ProxySession(const ProxySession&) = delete;
    ProxySession& operator=(const ProxySession&) = delete;
};

class ProxyRequest
{
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    // ストリーミングさせるのは面倒かつ必要がないので一旦全て読み込んでから返す
    std::vector<BYTE> data;
    std::function<void()> errorCallback;
    std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback;
    std::vector<BYTE> payload;
    // trueなら接続先がローカル・プライベートなアドレスのとき中止する
    bool blockPrivateAddresses = false;
    ProxyRequest
    (
        HINTERNET connect,
        HINTERNET request,
        std::function<void()> errorCallback,
        std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback,
        std::vector<BYTE> payload,
        bool blockPrivateAddresses
    );
    static void CALLBACK StaticAsyncCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength);
    void Fail();
    void AsyncCallback(HINTERNET hInternet, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength);
    void Close();
public:
    // 非同期HTTPリクエストを行う
    // 完了すると別スレッドでcallbackが呼ばれる。
    // blockPrivateAddressesがtrueなら、実際に接続した先 (名前解決やリダイレクトの後) が
    // ループバックやプライベートなどのアドレスのときは失敗させる
    static bool RequestAsync
    (
        ProxySession& session,
        LPCWSTR url,
        LPCWSTR verb,
        std::vector<BYTE> payload,
        std::vector<std::pair<LPCWSTR, LPCWSTR>>& headers,
        std::function<void()> errorCallback,
        std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback,
        bool blockPrivateAddresses = false
    );
    // インターネット上ではないアドレス (ループバック、プライベート、リンクローカルなど) か
    static bool IsPrivateAddress(LPCWSTR address);
    ~ProxyRequest();
    ProxyRequest(const ProxyRequest&) = delete;
    ProxyRequest& operator=(const ProxyRequest&) = delete;
};
