#include "pch.h"
#include "CaptionSettingsDialog.h"
#include "resource.h"

#include <algorithm>
#include <cstddef>
#include <set>

namespace
{
constexpr int MAX_DELAY_MS = 5000;

std::set<std::wstring> EnumerateFontFamilies()
{
    std::set<std::wstring> families;
    HDC hDC = GetDC(nullptr);
    if (!hDC)
    {
        return families;
    }
    LOGFONTW logFont = {};
    logFont.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(hDC, &logFont, [](const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lParam) -> int
    {
        // '@'で始まるものは縦書き用の別名なので除く
        if (lf->lfFaceName[0] != L'@')
        {
            reinterpret_cast<std::set<std::wstring>*>(lParam)->insert(lf->lfFaceName);
        }
        return 1;
    }, reinterpret_cast<LPARAM>(&families), 0);
    ReleaseDC(nullptr, hDC);
    return families;
}

void InitFontCombo(HWND hDlg, int id, const std::set<std::wstring>& families, const std::wstring& selected)
{
    // 空の項目を選ぶとフォントを指定しない(libaribcaptionに任せる)
    SendDlgItemMessageW(hDlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
    for (const auto& family : families)
    {
        SendDlgItemMessageW(hDlg, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(family.c_str()));
    }
    SetDlgItemTextW(hDlg, id, selected.c_str());
}

std::wstring GetComboText(HWND hDlg, int id)
{
    // 列挙したフォント名はLOGFONT由来だが、コンボボックスには
    // それより長いDirectWriteのファミリ名も入力できる
    HWND hControl = GetDlgItem(hDlg, id);
    if (!hControl)
    {
        return {};
    }
    const int length = GetWindowTextLengthW(hControl);
    if (length <= 0)
    {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hControl, &text[0], length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return text;
}

void SetCheck(HWND hDlg, int id, bool checked)
{
    SendDlgItemMessageW(hDlg, id, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool GetCheck(HWND hDlg, int id)
{
    return SendDlgItemMessageW(hDlg, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}
}

INT_PTR CALLBACK CaptionSettingsDialog::DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    auto pThis = static_cast<CaptionSettingsDialog*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        const auto& settings = pThis->settings;
        const auto families = EnumerateFontFamilies();
        InitFontCombo(hDlg, IDC_COMBO_FACE, families, settings.faceName);
        InitFontCombo(hDlg, IDC_COMBO_FACE1, families, settings.faceName1);
        InitFontCombo(hDlg, IDC_COMBO_FACE2, families, settings.faceName2);
        SetCheck(hDlg, IDC_CHECK_SHOW_CAPTION, settings.showCaption);
        SetCheck(hDlg, IDC_CHECK_SHOW_SUPERIMPOSE, settings.showSuperimpose);
        SetDlgItemInt(hDlg, IDC_EDIT_CAPTION_DELAY, settings.delayTime, TRUE);
        SetDlgItemInt(hDlg, IDC_EDIT_STROKE_WIDTH, settings.strokeWidth, FALSE);
        SetDlgItemInt(hDlg, IDC_EDIT_ORN_STROKE_WIDTH, settings.ornStrokeWidth, FALSE);
        SetCheck(hDlg, IDC_CHECK_NO_BACKGROUND, settings.noBackground);
        SetCheck(hDlg, IDC_CHECK_REPLACE_FULL_ALNUM, settings.replaceFullAlnum);
        SetCheck(hDlg, IDC_CHECK_REPLACE_FULL_JAPANESE, settings.replaceFullJapanese);
        SetCheck(hDlg, IDC_CHECK_REPLACE_DRCS, settings.replaceDrcs);
        SetCheck(hDlg, IDC_CHECK_IGNORE_SMALL, settings.ignoreSmall);
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK)
        {
            auto& settings = pThis->settings;
            settings.faceName = GetComboText(hDlg, IDC_COMBO_FACE);
            settings.faceName1 = GetComboText(hDlg, IDC_COMBO_FACE1);
            settings.faceName2 = GetComboText(hDlg, IDC_COMBO_FACE2);
            settings.showCaption = GetCheck(hDlg, IDC_CHECK_SHOW_CAPTION);
            settings.showSuperimpose = GetCheck(hDlg, IDC_CHECK_SHOW_SUPERIMPOSE);
            BOOL valid = FALSE;
            int delayTime = static_cast<int>(GetDlgItemInt(hDlg, IDC_EDIT_CAPTION_DELAY, &valid, TRUE));
            settings.delayTime = valid ? std::clamp(delayTime, -MAX_DELAY_MS, MAX_DELAY_MS) : 450;
            int strokeWidth = static_cast<int>(GetDlgItemInt(hDlg, IDC_EDIT_STROKE_WIDTH, &valid, FALSE));
            settings.strokeWidth = valid ? std::clamp(strokeWidth, 0, 100) : 30;
            int ornStrokeWidth = static_cast<int>(GetDlgItemInt(hDlg, IDC_EDIT_ORN_STROKE_WIDTH, &valid, FALSE));
            settings.ornStrokeWidth = valid ? std::clamp(ornStrokeWidth, 0, 100) : 50;
            settings.noBackground = GetCheck(hDlg, IDC_CHECK_NO_BACKGROUND);
            settings.replaceFullAlnum = GetCheck(hDlg, IDC_CHECK_REPLACE_FULL_ALNUM);
            settings.replaceFullJapanese = GetCheck(hDlg, IDC_CHECK_REPLACE_FULL_JAPANESE);
            settings.replaceDrcs = GetCheck(hDlg, IDC_CHECK_REPLACE_DRCS);
            settings.ignoreSmall = GetCheck(hDlg, IDC_CHECK_IGNORE_SMALL);
            EndDialog(hDlg, IDOK);
            return 1;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDCANCEL);
            return 1;
        }
        return 1;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    }
    return 0;
}
