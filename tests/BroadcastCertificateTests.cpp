#include "../TVTDataBroadcastingWV2/pch.h"
#include <iostream>
#include <iterator>
#include <winhttp.h>
#include <wincrypt.h>
// Simulate a platform-trusted fixture root entirely in memory. No certificates
// are installed into Windows, and the plugin's broadcast store remains empty.
static HCERTCHAINENGINE fixtureSystemEngine = nullptr;
static BOOL WINAPI FixtureGetChain(HCERTCHAINENGINE engine, PCCERT_CONTEXT certificate,
    LPFILETIME time, HCERTSTORE additional, PCERT_CHAIN_PARA parameters, DWORD flags,
    LPVOID reserved, PCCERT_CHAIN_CONTEXT* chain) {
    return CertGetCertificateChain(engine ? engine : fixtureSystemEngine, certificate, time,
        additional, parameters, flags, reserved, chain);
}
static BOOL WINAPI FixtureSend(HINTERNET request, LPCWSTR headers, DWORD headerLength,
    LPVOID optional, DWORD optionalLength, DWORD totalLength, DWORD_PTR context) {
    if (fixtureSystemEngine) {
        // Schannel cannot see our in-memory platform root. Defer this trust
        // anchor check to the real chain verifier using the fixture engine.
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) return FALSE;
    }
    return WinHttpSendRequest(request, headers, headerLength, optional, optionalLength, totalLength, context);
}
// Local TLS fixtures must not depend on the developer's automatic proxy.
static HINTERNET WINAPI FixtureOpen(LPCWSTR agent, DWORD, LPCWSTR, LPCWSTR, DWORD flags) {
    return WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, flags);
}
#define WinHttpOpen FixtureOpen
#define CertGetCertificateChain FixtureGetChain
#define WinHttpSendRequest FixtureSend
#include "../TVTDataBroadcastingWV2/proxy.cpp"
#undef WinHttpOpen
#undef CertGetCertificateChain
#undef WinHttpSendRequest

static void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static std::vector<BYTE> read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<BYTE>((std::istreambuf_iterator<char>(input)), {});
}
int wmain(int argc, wchar_t** argv) {
    using namespace BroadcastCertificates;
    if (argc >= 5 && std::wstring(argv[1]) == L"--tls") {
        auto bytes = read(argv[2]); auto cert = Decode(bytes);
        check(cert != nullptr, "TLS fixture root decoded");
        auto store = std::make_shared<Store>(std::filesystem::path(), std::vector<std::string>{SHA256(cert.get())});
        HCERTSTORE platformRoots = nullptr;
        const bool systemOnly = argc >= 6 && std::wstring(argv[5]) == L"system";
        if (systemOnly) {
            platformRoots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
            check(platformRoots && CertAddCertificateContextToStore(platformRoots, cert.get(), CERT_STORE_ADD_ALWAYS, nullptr), "fixture platform root loaded");
            CERT_CHAIN_ENGINE_CONFIG config{}; config.cbSize = sizeof(config); config.hExclusiveRoot = platformRoots;
            config.dwUrlRetrievalTimeout = 8000;
            check(CertCreateCertificateChainEngine(&config, &fixtureSystemEngine), "fixture platform chain engine created");
            check(!store->Snapshot(), "system-only TLS fixture has an empty broadcast store");
        } else {
            check(store->Update({{"0.1.1", 1, 1, bytes}}).changed == 1, "TLS fixture root admitted by explicit test pin");
        }
        const bool ignoreErrors = argc >= 6 && std::wstring(argv[5]) == L"ignore";
        ProxySession session(ignoreErrors); session.SetCertificateStore(store);
        session.SetDiagnosticCallback([](const ProxyDiagnostic& d) {
            std::wcerr << d.stage << L" error=0x" << std::hex << d.error << L" flags=0x" << d.secureFlags << L"\n";
        });
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr); std::atomic<bool> success{false};
        std::vector<std::pair<LPCWSTR,LPCWSTR>> headers;
        const std::string secret = "fixture-secret";
        const bool started = ProxyRequest::RequestAsync(session, argv[3], L"POST", std::vector<BYTE>(secret.begin(), secret.end()), headers,
            [&] { SetEvent(done); }, [&](DWORD status, LPCWSTR, LPCWSTR, size_t, BYTE*) { success = status == 200; SetEvent(done); });
        check(started, "TLS request started");
        check(WaitForSingleObject(done, 20000) == WAIT_OBJECT_0, "TLS request bounded completion");
        session.Shutdown(); CloseHandle(done);
        if (fixtureSystemEngine) { CertFreeCertificateChainEngine(fixtureSystemEngine); fixtureSystemEngine = nullptr; }
        if (platformRoots) CertCloseStore(platformRoots, 0);
        check(success == (std::wstring(argv[4]) == L"success"), "TLS expected policy outcome");
        std::cout << "TLS fixture completed\n";
        return 0;
    }
    check(argc == 3, "unit arguments");
    const auto root = read(argv[1]); const auto path = std::filesystem::path(argv[2]);
    std::filesystem::remove(path);
    auto cert = Decode(root);
    check(cert && SHA256(cert.get()) == kCybertrustSHA256, "broadcast root matches verified official pin");
    {
        Store unpinned(std::filesystem::path(), std::vector<std::string>{});
        auto rejected = unpinned.Update({{"0.3.4", 1, 2, root}});
        check(rejected.changed == 0 && rejected.messages.empty() && rejected.notices.size() == 1 && !unpinned.Snapshot(), "unknown fingerprint is noted but never trusted");
        check(rejected.notices[0].find(L"Subject: ") != std::wstring::npos && rejected.notices[0].find(L"Cybertrust") != std::wstring::npos &&
            rejected.notices[0].find(std::wstring(kCybertrustSHA256, kCybertrustSHA256 + 64)) != std::wstring::npos, "notice identifies the certificate");
        auto malformed = root; malformed.push_back(0);
        auto invalid = unpinned.Update({{"0.3.4", 1, 2, malformed}});
        check(invalid.messages.size() == 1 && invalid.notices.empty() && !unpinned.Snapshot(), "malformed certificate stays a warning");
    }
    Store store(path);
    check(!store.Snapshot(), "empty store has no private trust");
    auto accepted = store.Update({{"0.3.4", 1, 2, root}});
    check(accepted.changed == 1 && accepted.messages.empty() && store.Snapshot(), "valid root admitted and persisted");
    auto previous = store.Snapshot();
    check(store.Update({{"0.3.4", 1, 2, root}}).changed == 0, "identical receipt is not rewritten");
    auto bad = root; bad.back() ^= 1;
    check(store.Update({{"0.3.4", 2, 2, bad}}).changed == 0, "tampered root rejected");
    bad = root; bad.push_back(0);
    check(store.Update({{"0.3.4", 2, 2, bad}}).changed == 0, "trailing DER data rejected");
    check(store.Update({{"../../root", 1, 2, root}}).changed == 0, "invalid identity rejected");
    check(store.Snapshot() == previous, "rejected receipts retain last known good trust");
    check(store.Update({{"0.3.4", 2, 2, root}}).changed == 1, "accepted module metadata is updated");
    check(store.Snapshot() != previous && previous->Ready(), "requests retain immutable prior trust snapshot");
    Store loaded(path);
    check(loaded.Load().changed == 1 && loaded.Snapshot(), "saved root revalidated on load");
    {
        std::ifstream in(path); auto json = nlohmann::json::parse(in); in.close();
        json[0]["der"] = Hex(bad.data(), bad.size());
        std::ofstream out(path, std::ios::trunc); out << json.dump();
    }
    Store tampered(path);
    check(tampered.Load().changed == 0 && !tampered.Snapshot(), "edited disk cache cannot introduce trust");
    check(loaded.Snapshot()->Verify(cert.get(), L"bridge.tver.jp") != ERROR_SUCCESS, "CA certificate cannot impersonate TLS server");
    std::filesystem::remove(path);
    std::cout << "Broadcast certificate pin, update, persistence and trust tests passed\n";
}
