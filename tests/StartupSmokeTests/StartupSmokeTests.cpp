#include <cstdlib>
#include <iostream>
#include <string_view>

#include "StartupPolicy.h"
#include "AsyncLifetime.h"
#include "PostedPayload.h"

namespace
{
bool Check(bool condition, std::string_view description)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << description << '\n';
        return false;
    }
    std::cout << "PASSED: " << description << '\n';
    return true;
}

bool PreviousPanelCanBeRestored(bool isPluginEnabled, bool autoEnable)
{
    // RegisterPanelItem initially registers an enabled item. TVTest can only
    // restore a selection while that item remains enabled.
    bool panelItemEnabled = true;
    if (StartupPolicy::ShouldHidePanelItemsAfterRegistration(isPluginEnabled, autoEnable))
    {
        panelItemEnabled = false;
    }
    return panelItemEnabled;
}

bool CheckCanceledCreation()
{
    bool passed = true;
    AsyncCreation creation;
    const auto first = creation.TryBegin();
    if (!first) return Check(false, "initial creation starts");
    passed &= Check(!creation.TryBegin(), "a panel cannot request two controllers concurrently");
    creation.Cancel();
    const auto retry = creation.TryBegin();
    if (!retry) return Check(false, "re-enable retries canceled panel creation");
    passed &= Check(!creation.Complete(*first), "late completion from before disable is rejected");
    passed &= Check(!creation.TryBegin(), "late completion cannot clear the new pending request");
    passed &= Check(creation.Complete(*retry), "the new panel creation can complete");
    passed &= Check(!creation.Complete(*retry), "a completion is consumed only once");
    const auto failed = creation.TryBegin();
    if (!failed) return Check(false, "a subsequent creation starts");
    passed &= Check(creation.Complete(*failed) && creation.TryBegin().has_value(),
        "a failed API call can release the request for retry");
    creation.Cancel();
    return passed;
}

struct CountedPayload
{
    int& destroyed;
    ~CountedPayload() { ++destroyed; }
};

bool PostPayload(HWND window, UINT id, PostedPayloadParameter parameter, int& destroyed)
{
    auto payload = std::make_unique<CountedPayload>(destroyed);
    const auto raw = payload.get();
    if (!PostMessageW(window, id,
        parameter == PostedPayloadParameter::WParam ? reinterpret_cast<WPARAM>(raw) : 0,
        parameter == PostedPayloadParameter::LParam ? reinterpret_cast<LPARAM>(raw) : 0)) return false;
    payload.release();
    return true;
}

bool CheckPostedPayloadCleanup()
{
    const auto createWindow = [] {
        return CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    };
    const auto window = createWindow();
    const auto other = createWindow();
    if (!window || !other)
    {
        if (window) DestroyWindow(window);
        if (other) DestroyWindow(other);
        return Check(false, "message windows can be created");
    }
    constexpr UINT login = WM_APP + 6, input = WM_APP + 3, unrelated = WM_APP + 100;
    int loginDeleted = 0, inputDeleted = 0, otherDeleted = 0, unrelatedDeleted = 0;
    bool passed = true;
    passed &= Check(PostPayload(window, login, PostedPayloadParameter::WParam, loginDeleted) &&
        PostPayload(window, login, PostedPayloadParameter::WParam, loginDeleted) &&
        PostPayload(window, input, PostedPayloadParameter::LParam, inputDeleted) &&
        PostPayload(other, login, PostedPayloadParameter::WParam, otherDeleted) &&
        PostPayload(window, unrelated, PostedPayloadParameter::WParam, unrelatedDeleted),
        "ownership can be transferred through queued notifications");
    DiscardPostedPayloads<CountedPayload>(nullptr, login, PostedPayloadParameter::WParam);
    passed &= Check(loginDeleted == 0 && otherDeleted == 0, "a missing window does not drain the thread queue");
    DiscardPostedPayloads<CountedPayload>(window, login, PostedPayloadParameter::WParam);
    DiscardPostedPayloads<CountedPayload>(window, input, PostedPayloadParameter::LParam);
    passed &= Check(loginDeleted == 2 && inputDeleted == 1,
        "shutdown destroys every pending login and input payload");
    passed &= Check(otherDeleted == 0 && unrelatedDeleted == 0,
        "cleanup leaves other windows and message types alone");
    DiscardPostedPayloads<CountedPayload>(window, login, PostedPayloadParameter::WParam);
    passed &= Check(loginDeleted == 2, "repeated shutdown cannot free a notification twice");
    DiscardPostedPayloads<CountedPayload>(other, login, PostedPayloadParameter::WParam);
    DiscardPostedPayloads<CountedPayload>(window, unrelated, PostedPayloadParameter::WParam);
    DestroyWindow(window);
    DestroyWindow(other);
    return passed;
}
}

int main()
{
    bool passed = true;
    passed &= Check(
        StartupPolicy::ShouldHidePanelItemsAfterRegistration(false, false),
        "a disabled plugin without auto-enable hides its panel items");
    passed &= Check(
        !StartupPolicy::ShouldHidePanelItemsAfterRegistration(true, false),
        "an already enabled plugin keeps its panel items visible");
    passed &= Check(
        !StartupPolicy::ShouldHidePanelItemsAfterRegistration(false, true),
        "auto-enable keeps panel items visible during TVTest startup");
    passed &= Check(
        PreviousPanelCanBeRestored(false, true),
        "the previous panel selection is restorable before delayed auto-enable");
    passed &= Check(
        !PreviousPanelCanBeRestored(false, false),
        "ordinary disabled startup does not leave inactive panel tabs visible");

    passed &= CheckCanceledCreation();
    passed &= CheckPostedPayloadCleanup();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
