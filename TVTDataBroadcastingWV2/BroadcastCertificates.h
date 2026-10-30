#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "thirdparty/json.hpp"
#pragma comment(lib, "crypt32.lib")

namespace BroadcastCertificates {
// Confirmed against the issuing CA's published fingerprint, not learned from TLS.
// https://cybertrust.co.jp/ssl/support/faq/fxuxiequbf9r.html
inline constexpr char kCybertrustSHA256[] = "8D94C61B219E6B9109BEAC136B3A59098D43B967C44F3730CA6E40B19D1FFBCD";
inline constexpr size_t kMaxCertificateBytes = 16384;
inline constexpr size_t kMaxRoots = 8;
inline std::string Hex(const BYTE* bytes, size_t size) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text; text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { text += digits[bytes[i] >> 4]; text += digits[bytes[i] & 15]; }
    return text;
}
inline std::vector<BYTE> Unhex(const std::string& text) {
    if (text.size() % 2 || text.size() > kMaxCertificateBytes * 2) return {};
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::vector<BYTE> bytes;
    for (size_t i = 0; i < text.size(); i += 2) {
        int a = digit(text[i]), b = digit(text[i + 1]); if (a < 0 || b < 0) return {};
        bytes.push_back(static_cast<BYTE>((a << 4) | b));
    }
    return bytes;
}
struct CertificateDeleter { void operator()(const CERT_CONTEXT* value) const { if (value) CertFreeCertificateContext(value); } };
using Certificate = std::unique_ptr<const CERT_CONTEXT, CertificateDeleter>;
inline Certificate Decode(const std::vector<BYTE>& der) {
    // A complete DER SEQUENCE only; reject appended certificates or metadata.
    if (der.size() < 4 || der.size() > kMaxCertificateBytes || der[0] != 0x30) return {};
    size_t header = 2, length = der[1];
    if (length & 0x80) {
        const size_t n = length & 0x7f;
        if (n == 0 || n > 3 || 2 + n > der.size()) return {};
        length = 0; header += n;
        for (size_t i = 0; i < n; ++i) length = (length << 8) | der[2 + i];
    }
    if (length != der.size() - header) return {};
    return Certificate(CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size())));
}
inline std::string SHA256(PCCERT_CONTEXT cert) {
    BYTE hash[32]; DWORD size = sizeof(hash);
    return cert && CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, cert->pbCertEncoded, cert->cbCertEncoded, hash, &size)
        ? Hex(hash, size) : std::string();
}
struct Candidate {
    std::string location;
    int moduleVersion = 0;
    int dataEventId = 0;
    std::vector<BYTE> der;
};
struct UpdateResult { size_t changed = 0; std::vector<std::wstring> messages; };

class Trust {
    HCERTSTORE roots = nullptr;
    HCERTCHAINENGINE engine = nullptr;
public:
    explicit Trust(const std::vector<Candidate>& certificates) {
        roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
        if (!roots) return;
        for (const auto& c : certificates) {
            if (!CertAddEncodedCertificateToStore(roots, X509_ASN_ENCODING, c.der.data(), static_cast<DWORD>(c.der.size()), CERT_STORE_ADD_ALWAYS, nullptr)) return;
        }
        CERT_CHAIN_ENGINE_CONFIG config{}; config.cbSize = sizeof(config);
        config.hExclusiveRoot = roots;
        config.dwUrlRetrievalTimeout = 8000;
        CertCreateCertificateChainEngine(&config, &engine);
    }
    ~Trust() { if (engine) CertFreeCertificateChainEngine(engine); if (roots) CertCloseStore(roots, 0); }
    Trust(const Trust&) = delete;
    Trust& operator=(const Trust&) = delete;
    bool Ready() const { return engine != nullptr; }
    // Verify the full TLS policy, including SAN/CN, EKU, validity and online
    // revocation (excluding the explicitly trusted root). Never ignore errors.
    static DWORD VerifyWithEngine(HCERTCHAINENGINE engine, PCCERT_CONTEXT server, const std::wstring& host, HCERTSTORE additional) {
        if (!server || host.empty()) return CERT_E_CN_NO_MATCH;
        CERT_CHAIN_PARA parameters{}; parameters.cbSize = sizeof(parameters);
        LPSTR oid = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
        parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &oid;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (!CertGetCertificateChain(engine, server, nullptr, additional, &parameters,
            CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT, nullptr, &chain)) return GetLastError();
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{}; ssl.cbSize = sizeof(ssl); ssl.dwAuthType = AUTHTYPE_SERVER;
        ssl.pwszServerName = const_cast<LPWSTR>(host.c_str());
        CERT_CHAIN_POLICY_PARA policy{}; policy.cbSize = sizeof(policy); policy.pvExtraPolicyPara = &ssl;
        CERT_CHAIN_POLICY_STATUS status{}; status.cbSize = sizeof(status);
        const bool ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) != FALSE;
        const DWORD result = ok ? status.dwError : GetLastError();
        CertFreeCertificateChain(chain);
        return result;
    }
    DWORD Verify(PCCERT_CONTEXT server, const std::wstring& host, HCERTSTORE additional = nullptr) const {
        DWORD system = VerifyWithEngine(nullptr, server, host, additional);
        if (system == ERROR_SUCCESS) return ERROR_SUCCESS;
        // A private root can only repair a trust-anchor failure, not revocation,
        // hostname, expiry, signature or usage failures on a trusted chain.
        if (system != CERT_E_UNTRUSTEDROOT && system != CERT_E_CHAINING) return system;
        return engine ? VerifyWithEngine(engine, server, host, additional) : system;
    }
};

class Store {
    mutable std::mutex mutex;
    std::filesystem::path path;
    const std::vector<std::string> pins;
    std::map<std::string, Candidate> accepted;
    std::shared_ptr<const Trust> trust;
    std::string Validate(const Candidate& candidate, std::wstring& error) const {
        if (candidate.location.empty() || candidate.location.size() > 64 ||
            candidate.location.find_first_not_of("0123456789.") != std::string::npos ||
            candidate.moduleVersion < 0 || candidate.moduleVersion > 255 || candidate.dataEventId < 0 || candidate.dataEventId > 15) {
            error = L"証明書モジュールの識別情報が不正"; return {};
        }
        auto cert = Decode(candidate.der);
        if (!cert) { error = L"DER証明書の形式またはサイズが不正"; return {}; }
        const auto hash = SHA256(cert.get());
        if (std::find(pins.begin(), pins.end(), hash) == pins.end()) {
            error = L"事前許可されていないルート指紋: " + std::wstring(hash.begin(), hash.end()); return {};
        }
        if (CertVerifyTimeValidity(nullptr, cert->pCertInfo) != 0) { error = L"ルート証明書が有効期間外"; return {}; }
        if (!CertCompareCertificateName(X509_ASN_ENCODING, &cert->pCertInfo->Subject, &cert->pCertInfo->Issuer) ||
            !CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
                const_cast<CERT_CONTEXT*>(cert.get()), CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, const_cast<CERT_CONTEXT*>(cert.get()), 0, nullptr)) {
            error = L"ルート証明書の自己署名が不正"; return {};
        }
        BYTE usage = 0;
        if (!CertGetIntendedKeyUsage(X509_ASN_ENCODING, cert->pCertInfo, &usage, sizeof(usage)) || !(usage & CERT_KEY_CERT_SIGN_KEY_USAGE)) {
            error = L"証明書署名用の鍵用途がない"; return {};
        }
        auto extension = CertFindExtension(szOID_BASIC_CONSTRAINTS2, cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
        CERT_BASIC_CONSTRAINTS2_INFO* constraints = nullptr; DWORD size = 0;
        if (!extension || !CryptDecodeObjectEx(X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, extension->Value.pbData,
            extension->Value.cbData, CRYPT_DECODE_ALLOC_FLAG, nullptr, &constraints, &size)) {
            error = L"CA基本制約がない"; return {};
        }
        const bool isCA = constraints->fCA != FALSE; LocalFree(constraints);
        if (!isCA) { error = L"ルートCA証明書ではない"; return {}; }
        return hash;
    }
    bool SaveLocked() const {
        if (path.empty()) return true;
        try {
            nlohmann::json data = nlohmann::json::array();
            for (const auto& [hash, c] : accepted) data.push_back({{"sha256", hash}, {"location", c.location},
                {"moduleVersion", c.moduleVersion}, {"dataEventId", c.dataEventId}, {"der", Hex(c.der.data(), c.der.size())}});
            const auto temporary = std::filesystem::path(path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
            std::filesystem::create_directories(path.parent_path());
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << data.dump(); out.flush(); if (!out) return false; }
            return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        } catch (...) { return false; }
    }
    void RebuildLocked() {
        std::vector<Candidate> roots;
        for (const auto& [hash, c] : accepted) {
            auto cert = Decode(c.der);
            if (cert && CertVerifyTimeValidity(nullptr, cert->pCertInfo) == 0) roots.push_back(c);
        }
        auto next = roots.empty() ? nullptr : std::make_shared<Trust>(roots);
        trust = next && next->Ready() ? next : nullptr;
    }
public:
    explicit Store(std::filesystem::path path = {}, std::vector<std::string> approved = {kCybertrustSHA256})
        : path(std::move(path)), pins(std::move(approved)) {}
    std::shared_ptr<const Trust> Snapshot() const { std::lock_guard lock(mutex); return trust; }
    UpdateResult Update(const std::vector<Candidate>& candidates, bool persist = true) {
        UpdateResult result;
        if (candidates.size() > kMaxRoots) { result.messages.push_back(L"放送ルート証明書の件数が上限を超えています"); return result; }
        std::lock_guard lock(mutex);
        for (const auto& c : candidates) {
            std::wstring error;
            const auto hash = Validate(c, error);
            if (hash.empty()) { result.messages.push_back(L"放送ルート証明書を拒否: " + error); continue; }
            auto found = accepted.find(hash);
            if (found != accepted.end() && found->second.location == c.location && found->second.moduleVersion == c.moduleVersion && found->second.dataEventId == c.dataEventId) continue;
            if (found == accepted.end() && accepted.size() >= kMaxRoots) { result.messages.push_back(L"放送ルート証明書の保存上限に達しました"); continue; }
            accepted[hash] = c; ++result.changed;
        }
        if (result.changed) {
            RebuildLocked();
            if (!trust) result.messages.push_back(L"放送ルート証明書の検証エンジンを作成できません");
            if (persist && !SaveLocked()) result.messages.push_back(L"放送ルート証明書の保存に失敗しました");
        }
        return result;
    }
    UpdateResult Load() {
        UpdateResult result;
        try {
            if (path.empty() || !std::filesystem::exists(path)) return result;
            if (std::filesystem::file_size(path) > kMaxRoots * kMaxCertificateBytes * 2 + 8192) throw std::runtime_error("large store");
            std::ifstream in(path, std::ios::binary);
            const auto data = nlohmann::json::parse(in);
            if (!data.is_array() || data.size() > kMaxRoots) throw std::runtime_error("invalid store");
            std::vector<Candidate> candidates;
            for (const auto& c : data) candidates.push_back({c.at("location").get<std::string>(), c.at("moduleVersion").get<int>(), c.at("dataEventId").get<int>(), Unhex(c.at("der").get<std::string>())});
            return Update(candidates, false); // Revalidate pins/signature/expiry after every restart.
        } catch (...) { result.messages.push_back(L"保存済み放送ルート証明書の形式が不正"); return result; }
    }
};
}
