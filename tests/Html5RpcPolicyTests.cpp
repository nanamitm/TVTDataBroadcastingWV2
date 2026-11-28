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
        for (const auto* key : {"greg0", "ureg63", "_common255", "_wlocal10", "_local0", "_zipcode"}) {
            const auto body = nlohmann::json{{"method", "storageSet"}, {"params", {{"key", key}, {"value", "x"}}}}.dump();
            Check(ParseRequest(body).has_value(), "Valid storage key rejected");
        }
        for (const auto* key : {"", "greg64", "greg01", "ureg", "_common256", "_common-1", "_local1a", "_other0", "foo", "_prefecture", "_regioncode"}) {
            const auto body = nlohmann::json{{"method", "storageSet"}, {"params", {{"key", key}, {"value", "x"}}}}.dump();
            Check(!ParseRequest(body), "Invalid or read-only storage key accepted");
        }
        Check(ParseRequest(R"({"method":"storageGet","params":{"key":"_prefecture"}})").has_value(), "Residence information not readable");
        Check(!ParseRequest(R"({"method":"storageRemove","params":{"key":"_regioncode"}})"), "Residence information removable");
        Check(!ParseRequest(R"({"method":"storageGet","params":{"key":"arbitrary"}})"), "Arbitrary storage key readable");
        Check(StorageValueLimit("greg1") == 512 * 3 && StorageValueLimit("_common0") == kMaxStorageValue, "Storage value limit");
        Check(ParseRequest(R"({"method":"readPersistentArray","params":{"filename":"nvram://receiverinfo/zipcode"}})").has_value(), "Optional structure rejected");
        Check(ParseRequest(R"({"method":"writePersistentArray","params":{"filename":"nvram://x","structure":"U:1B","data":[1]}})").has_value(), "Valid array write rejected");
        // chrome.webview messages are authorized natively, not by the page's runtime.
        Check(!MessageAllowed(-1, "html5Tune") && !MessageAllowed(0, "html5Tune") && MessageAllowed(kPermissionMisc, "html5Tune"),
            "Tune requires the managed MISC permission");
        Check(!MessageAllowed(0, "html5Replace") && !MessageAllowed(0, "html5LaunchDataBroadcasting"), "Application control requires MISC");
        Check(!MessageAllowed(-1, "html5ExitManaged") && MessageAllowed(0, "html5ExitManaged") && !MessageAllowed(-1, "html5Destroy"),
            "Owner application messages require the managed state");
        Check(!MessageAllowed(kPermissionMisc, "html5Caption") && MessageAllowed(kPermissionAV, "html5Caption"), "Caption visibility requires AV");
        Check(MessageAllowed(-1, "html5KeySet") && MessageAllowed(-1, "html5Viewport") && MessageAllowed(-1, "html5Video"),
            "Layout and key messages remain available");
        Check(!MessageAllowed(0x1fff, "html5Unknown") && !MessageAllowed(0x1fff, ""), "Unknown message accepted");
        const nlohmann::json video{{"type", "html5Video"}, {"present", true}, {"audioMute", true}, {"fullscreen", true}, {"cssLeft", 10.0}};
        Check(RestrictVideoMessage(video, -1) == nlohmann::json{{"type", "html5Video"}, {"present", false}}, "Unmanaged document placed video");
        const auto noAV = RestrictVideoMessage(video, kPermissionMisc);
        Check(noAV["present"] == true && noAV["audioMute"] == false && noAV["fullscreen"] == false && noAV["cssLeft"] == 10.0,
            "Video without AV may be placed but not muted or made fullscreen");
        Check(RestrictVideoMessage(video, kPermissionAV) == video, "AV permission video message changed");
        std::cout << "HTML5 native RPC authorization tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
