#pragma once

namespace StartupPolicy
{
// TVTest restores the previously selected panel item after plugins register
// their items. An automatically enabled plugin must therefore keep its items
// visible until that restoration has completed.
constexpr bool ShouldHidePanelItemsAfterRegistration(bool isPluginEnabled, bool autoEnable) noexcept
{
    return !isPluginEnabled && !autoEnable;
}
}
