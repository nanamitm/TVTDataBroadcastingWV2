#pragma once
#include <functional>
#include "AsyncLifetime.h"

class NVRAMSettingsDialog
{
    wil::com_ptr<ICoreWebView2> webView;
    HWND hDlg = nullptr;
    AsyncLifetime lifetime;
    HRESULT ReadNVRAM(std::function<HRESULT(HRESULT, nlohmann::json&)> callback, const char* filename, const char* structure);
public:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    NVRAMSettingsDialog(wil::com_ptr<ICoreWebView2> webView) : webView(webView) {}
    NVRAMSettingsDialog(const NVRAMSettingsDialog&) = delete;
    NVRAMSettingsDialog& operator=(const NVRAMSettingsDialog&) = delete;
};
