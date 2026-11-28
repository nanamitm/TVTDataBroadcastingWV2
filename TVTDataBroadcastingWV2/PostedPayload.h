#pragma once
#include <windows.h>

enum class PostedPayloadParameter { WParam, LParam };

// Call after producers have stopped, and before destroying the target window.
// A null HWND would inspect every window on the thread, so never drain it.
template<typename T>
void DiscardPostedPayloads(HWND window, UINT messageId, PostedPayloadParameter parameter)
{
    if (!window) return;
    MSG message;
    while (PeekMessageW(&message, window, messageId, messageId, PM_REMOVE))
    {
        if (parameter == PostedPayloadParameter::WParam)
            delete reinterpret_cast<T*>(message.wParam);
        else
            delete reinterpret_cast<T*>(message.lParam);
    }
}
