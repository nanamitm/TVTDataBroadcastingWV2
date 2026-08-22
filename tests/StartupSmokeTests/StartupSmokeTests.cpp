#include <cstdlib>
#include <iostream>
#include <string_view>

#include "StartupPolicy.h"

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

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
