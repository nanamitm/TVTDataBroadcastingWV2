#include "../TVTDataBroadcastingWV2/pch.h"
#include <iostream>
#include <iterator>
#include <winhttp.h>
// Local TLS fixtures must not depend on the developer's automatic proxy.
static HINTERNET WINAPI FixtureOpen(LPCWSTR agent, DWORD, LPCWSTR, LPCWSTR, DWORD flags) {
    return WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, flags);
}
#define WinHttpOpen FixtureOpen
#include "../TVTDataBroadcastingWV2/proxy.cpp"
#undef WinHttpOpen

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
        check(store->Update({{"0.1.1", 1, 1, bytes}}).changed == 1, "TLS fixture root admitted by explicit test pin");
        ProxySession session; session.SetCertificateStore(store);
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
        check(success == (std::wstring(argv[4]) == L"success"), "TLS expected policy outcome");
        std::cout << "TLS fixture completed\n";
        return 0;
    }
    check(argc == 3, "unit arguments");
    const auto root = read(argv[1]); const auto path = std::filesystem::path(argv[2]);
    std::filesystem::remove(path);
    auto cert = Decode(root);
    check(cert && SHA256(cert.get()) == kCybertrustSHA256, "broadcast root matches verified official pin");
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
