#include "../TVTDataBroadcastingWV2/pch.h"
#include "../TVTDataBroadcastingWV2/Html5RpcPolicy.h"
#include <iostream>
#include <stdexcept>

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        using namespace Html5RpcPolicy;
        const nlohmann::json boundaries = {{{"managedURLs", {"https://example.test/app/"}}, {"permissionBitmaps", {0x1fff}}}};
        const auto allowed = Permission(L"https://example.test/app/index.html", boundaries, false);
        Check(Allowed(allowed, "readPersistentArray") && Allowed(allowed, "writePersistentArray"), "Managed NVRAM access rejected");
        Check(!Allowed(Permission(L"https://example.test/outside/", boundaries, false), "readPersistentArray"), "Outside path can read NVRAM");
        Check(!Allowed(Permission(L"https://outside.test/", boundaries, false), "writePersistentArray"), "Outside origin can write NVRAM");
        Check(!Allowed(Permission(L"https://example.test:8443/app/", boundaries, false), "readPersistentArray"), "Different port was accepted");
        Check(!Allowed(Permission(L"https://example.test/app/", boundaries, true), "getGreg"), "Explicit unmanaged state was ignored");
        auto limited = boundaries;
        limited[0]["permissionBitmaps"] = {1 << 10};
        const auto events = Permission(L"https://example.test/app/", limited, false);
        Check(Allowed(events, "getCurrentEvent") && !Allowed(events, "readPersistentArray"), "Method permission was ignored");
        limited.push_back({{"managedURLs", {"https://example.test/app/restricted/"}}, {"permissionBitmaps", {0}}});
        Check(!Allowed(Permission(L"https://example.test/app/restricted/page", limited, false), "getCurrentEvent"), "Longest boundary did not win");
        Check(Allowed(Permission(L"https://example.test/", nlohmann::json::array(), false), "readPersistentArray"), "Absent descriptor compatibility lost");
        Check(!Allowed(0x1fff, "unknownMethod") && !Allowed(-1, "isRecord"), "Unknown or unmanaged method allowed");
        Check(Permission(L"https://example.test/app/", nlohmann::json::array({42}), false) < 0, "Malformed boundary accepted");
        for (const auto* body : {"[]", "null", "{", R"({"method":123})", R"({"method":"storageGet","params":[]})",
            R"({"method":"storageGet"})", R"({"method":"storageSet","params":{"key":"_common0","value":false}})",
            R"({"method":"setGreg","params":{"index":18446744073709551615,"value":"x"}})",
            R"({"method":"readPersistentArray","params":{"filename":false}})",
            R"({"method":"writePersistentArray","params":{"filename":"nvram://x","structure":"U:1B","data":42}})"}) {
            Check(!ParseRequest(body), "Malformed RPC request was accepted");
        }
        Check(ParseRequest(R"({"method":"getCurrentEvent","params":null})").has_value(), "Null no-argument params rejected");
        Check(ParseRequest(R"({"method":"storageGet","params":{"key":"_common0"}})").has_value(), "Storage request rejected");
        Check(ParseRequest(R"({"method":"readPersistentArray","params":{"filename":"nvram://receiverinfo/zipcode"}})").has_value(), "Optional structure rejected");
        Check(ParseRequest(R"({"method":"writePersistentArray","params":{"filename":"nvram://x","structure":"U:1B","data":[1]}})").has_value(), "Valid array write rejected");
        std::cout << "HTML5 native RPC authorization tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
