#pragma once

#include "NativeCaptionRenderer.h"

// 字幕の表示設定ダイアログ
class CaptionSettingsDialog
{
    NativeCaptionSettings settings;
public:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    explicit CaptionSettingsDialog(const NativeCaptionSettings& settings) : settings(settings) {}
    CaptionSettingsDialog(const CaptionSettingsDialog&) = delete;
    CaptionSettingsDialog& operator=(const CaptionSettingsDialog&) = delete;
    const NativeCaptionSettings& GetSettings() const { return settings; }
};
