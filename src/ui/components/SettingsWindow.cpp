// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/components/SettingsWindow.h"

#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <gdiplus.h>

#include <string>

#include "ui/AppShared.h"
#include "ui/SettingsModel.h"
#include "ui/Theme.h"
#include "ui/components/StyledCombo.h"
#include "ui/components/AboutDialog.h"
#include "service.h"
#include "AppContract.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace theme;     // shared palette + fonts (ui/Theme.h)
using namespace settings;  // the settings model (ui/SettingsModel.h)

namespace {

const wchar_t* kSettingsClass = L"OledCareSettingsWnd";

// Settings-window control ids.
enum CtrlId : UINT {
    IDC_STARTUP = 2001, IDC_HIDE = 2002, IDC_HIPRIO_BTN = 2003, IDC_PROTECT = 2004,
    IDC_SAFEHIBER = 2005, IDC_HELP_PROTECT = 2006, IDC_HELP_HIBER = 2007, IDC_HOTKEY = 2008,
    IDC_PEEKSECS = 2009, IDC_THEME = 2010, IDC_FADEIN = 2012, IDC_FADEOUT = 2013,
    IDC_TASKBAR = 2014, IDC_HELP_STARTUP = 2015, IDC_TASKBAR_MON = 2016, IDC_HIDE_TARGET = 2017,
    IDC_DIM_AMOUNT = 2018, IDC_DIM_AFTER = 2019
};

const int kCheckCol = 270;    // x of the aligned checkbox/button column (DIPs)
const int kRow1Center = 518;  // vertical center of the "Start with Windows" row (DIPs)

// All settings-window layout metrics, in DIPs (scaled by DPI at use). Single source of truth so the
// controls (WM_CREATE) and the section group boxes (WM_PAINT) can never drift apart. Each section's
// box spans [Top, Bottom]; each r* is a row's vertical center.
namespace Layout {
    const int WinWidth = 500, WinHeight = 718;

    const int SysTrayTop = 24, SysTrayBottom = 254;
    const int rHide = 50, rHideAll = 86, rFadeIn = 122, rFadeOut = 158, rHotkey = 194, rPeek = 230;

    const int TaskbarTop = 276, TaskbarBottom = 362;
    const int rTaskbar = 302, rTaskbarMon = 338;

    const int DimTop = 384, DimBottom = 470;
    const int rDimAmount = 410, rDimAfter = 446;

    const int StartupTop = 492, StartupBottom = 622;
    const int r1 = kRow1Center, r2 = 558, r3 = 598;

    const int AppearanceTop = 644, AppearanceBottom = 694;
    const int rTheme = 670;
}

bool g_serviceOn = false;        // high-priority (service) state for the button
bool g_capturingHotkey = false;  // the window is waiting to capture a key

}  // namespace

namespace ui {

// Forward decl so EnsureSettingsClass can name the window proc defined further down.
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Register the settings window class once, lazily on first open.
void EnsureSettingsClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW sc = {};
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = SettingsProc;
    sc.hInstance = g_hInst;
    sc.lpszClassName = kSettingsClass;
    sc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    sc.hIcon = g_appIcon;
    sc.hIconSm = g_appIconSm;
    // No class brush: WM_ERASEBKGND paints with the current theme brush (recreated on theme changes).
    RegisterClassExW(&sc);
    registered = true;
}

// Position the high-priority button: when the service is on it takes the checkbox's place
// (left edge = checkbox column); when off it sits just right of the "Start with Windows"
// checkbox. Both stretch to the right edge of the group box.
void LayoutHiprioButton(HWND dlg) {
    HWND btn = GetDlgItem(dlg, IDC_HIPRIO_BTN);
    if (btn == nullptr) {
        return;
    }
    int dpi = GetDpiForWindow(dlg);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    RECT cr;
    GetClientRect(dlg, &cr);
    int btnRight = cr.right - S(16) - S(14);  // 14px inside the group border, matching the label's left inset
    int cbLeft = S(kCheckCol);
    int left = g_serviceOn ? cbLeft : (cbLeft + S(20) + S(12));
    SetWindowPos(btn, nullptr, left, S(kRow1Center - 15), btnRight - left, S(30), SWP_NOZORDER);
}

// Re-apply the effective light/dark theme to the settings window, its dropdowns and the About
// window (invoked when the Theme dropdown changes).
void ApplySettingsTheme(HWND hwnd) {
    ApplyTheme();
    BOOL darkAttr = g_effectiveDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkAttr, sizeof(darkAttr));
    AllowWindowDarkMode(hwnd, g_effectiveDark);
    const UINT comboIds[] = { IDC_PEEKSECS, IDC_THEME, IDC_FADEIN, IDC_FADEOUT, IDC_TASKBAR, IDC_TASKBAR_MON, IDC_HIDE_TARGET, IDC_DIM_AMOUNT, IDC_DIM_AFTER };
    for (UINT cid : comboIds) {
        SetWindowTheme(GetDlgItem(hwnd, cid), g_effectiveDark ? L"DarkMode_CFD" : L"CFD", nullptr);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    ui::RefreshAboutTheme();  // track the theme on the open About window (if any)
}

// Toggle the "high priority" Windows service from its button and reflect the new state in the
// startup controls.
void ToggleHighPriority(HWND hwnd) {
    if (g_serviceOn) {
        RunElevated(L"--uninstall-service");
        g_serviceOn = false;
        SetStartup(true);   // fall back to run-key autostart
    } else {
        SetStartup(false);  // the service replaces the run key
        RunElevated(L"--install-service");
        g_serviceOn = true;
    }
    ShowWindow(GetDlgItem(hwnd, IDC_STARTUP), g_serviceOn ? SW_HIDE : SW_SHOW);
    Button_SetCheck(GetDlgItem(hwnd, IDC_STARTUP), BST_CHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_PROTECT), g_serviceOn);
    EnableWindow(GetDlgItem(hwnd, IDC_SAFEHIBER), g_serviceOn);
    LayoutHiprioButton(hwnd);
    InvalidateRect(GetDlgItem(hwnd, IDC_HIPRIO_BTN), nullptr, TRUE);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_capturingHotkey = false;
        int dpi = GetDpiForWindow(hwnd);
        auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
        const int labelX = 30;
        const int helpCol = kCheckCol - 26;  // "?" sits just left of the control column
        // Rows are positioned by their vertical center (DIPs); each control derives its top
        // from that center so labels, "?" glyphs, and checkboxes all line up.
        auto label = [&](const wchar_t* t, int cy, int w) {
            HWND s = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE,
                                   S(labelX), S(cy - 10), S(w), S(20), hwnd, nullptr, g_hInst, nullptr);
            SendMessageW(s, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        };
        auto check = [&](int cy, UINT id) {
            CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                          S(kCheckCol), S(cy - 10), S(20), S(20), hwnd,
                          reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), g_hInst, nullptr);
        };
        auto help = [&](int cy, UINT id) {
            CreateWindowW(L"STATIC", L"?", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                          S(helpCol), S(cy - 9), S(16), S(18), hwnd,
                          reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), g_hInst, nullptr);
        };
        // A labelled duration dropdown (used for fade in / fade out); stores ms in HKCU.
        auto fadeCombo = [&](const wchar_t* lbl, int cy, UINT id, const wchar_t* regName, DWORD def) {
            label(lbl, cy, 180);
            HWND c = ui::CreateStyledCombo(hwnd, id, kCheckCol, cy, 180);
            const wchar_t* names[] = { L"Instant", L"Fast", L"Normal", L"Slow" };
            const DWORD vals[] = { 0, 150, 300, 600 };
            DWORD cur = GetRegDword(regName, def);
            for (int i = 0; i < 4; ++i) {
                int idx = static_cast<int>(SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i])));
                SendMessageW(c, CB_SETITEMDATA, idx, static_cast<LPARAM>(vals[i]));
                if (vals[i] == cur) {
                    SendMessageW(c, CB_SETCURSEL, idx, 0);
                }
            }
            if (SendMessageW(c, CB_GETCURSEL, 0, 0) == CB_ERR) {
                SendMessageW(c, CB_SETCURSEL, 2, 0);  // default to "Normal"
            }
        };

        // Row centers come from the shared Layout table (so they stay in lockstep with the section
        // group boxes drawn in WM_PAINT). The sections themselves are drawn there.
        using namespace Layout;

        label(L"Theme:", rTheme, 180);
        HWND themeCombo = ui::CreateStyledCombo(hwnd, IDC_THEME, kCheckCol, rTheme, 160);
        {
            const wchar_t* names[] = { L"Dark", L"Light", L"System" };
            const int modes[] = { 2, 1, 0 };
            for (int i = 0; i < 3; ++i) {
                int idx = static_cast<int>(SendMessageW(themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i])));
                SendMessageW(themeCombo, CB_SETITEMDATA, idx, modes[i]);
                if (modes[i] == g_themeMode) {
                    SendMessageW(themeCombo, CB_SETCURSEL, idx, 0);
                }
            }
        }

        // Taskbar section: a "Color" dropdown (Normal / Clear).
        label(L"Color:", rTaskbar, 180);
        HWND tbCombo = ui::CreateStyledCombo(hwnd, IDC_TASKBAR, kCheckCol, rTaskbar, 200);
        {
            // Two modes only: Normal (the Windows-configured taskbar) and Clear (fully transparent).
            const wchar_t* names[] = { L"Normal", L"Clear" };
            const int modes[] = { 0, 2 };
            int curMode = static_cast<int>(GetTaskbarMode());
            for (int i = 0; i < 2; ++i) {
                int idx = static_cast<int>(SendMessageW(tbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i])));
                SendMessageW(tbCombo, CB_SETITEMDATA, idx, modes[i]);
                if (modes[i] == curMode) {
                    SendMessageW(tbCombo, CB_SETCURSEL, idx, 0);
                }
            }
            if (SendMessageW(tbCombo, CB_GETCURSEL, 0, 0) == CB_ERR) {
                SendMessageW(tbCombo, CB_SETCURSEL, 0, 0);  // default to "Normal"
            }
        }

        // Taskbar section: which monitor the color applies to - "All monitors" or a specific one.
        label(L"Target:", rTaskbarMon, 180);
        HWND monCombo = ui::CreateStyledCombo(hwnd, IDC_TASKBAR_MON, kCheckCol, rTaskbarMon, 240);
        PopulateMonitorCombo(monCombo, GetTaskbarMonitor());

        label(L"Hide system tray:", rHide, 180);
        check(rHide, IDC_HIDE);

        // System tray: which monitor to hide it on - "All monitors" or a specific one.
        label(L"Target:", rHideAll, 180);
        HWND hideMonCombo = ui::CreateStyledCombo(hwnd, IDC_HIDE_TARGET, kCheckCol, rHideAll, 240);
        PopulateMonitorCombo(hideMonCombo, GetHideMonitor());

        fadeCombo(L"Fade in:", rFadeIn, IDC_FADEIN, contract::kRegFadeInMs, 150);
        fadeCombo(L"Fade out:", rFadeOut, IDC_FADEOUT, contract::kRegFadeOutMs, 300);

        label(L"Peek hotkey:", rHotkey, 180);
        HWND hk = CreateWindowW(L"BUTTON", HotkeyToString(g_hotkeyMods, g_hotkeyVk).c_str(),
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                S(kCheckCol), S(rHotkey - 14), S(150), S(28), hwnd,
                                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_HOTKEY)), g_hInst, nullptr);
        SendMessageW(hk, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);

        label(L"Peek duration:", rPeek, 180);
        HWND combo = ui::CreateStyledCombo(hwnd, IDC_PEEKSECS, kCheckCol, rPeek, 180);
        {
            const int presets[] = { 2, 3, 5, 10 };
            int cur = GetPeekSeconds();
            for (int sv : presets) {
                wchar_t buf[32];
                wsprintfW(buf, L"%d seconds", sv);
                int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf)));
                SendMessageW(combo, CB_SETITEMDATA, idx, sv);
                if (sv == cur) {
                    SendMessageW(combo, CB_SETCURSEL, idx, 0);
                }
            }
            if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR) {
                SendMessageW(combo, CB_SETCURSEL, 1, 0);  // default to "3 seconds"
            }
        }

        // Screen Dimming: how dark to go when idle, and after how long.
        label(L"Dim amount:", rDimAmount, 180);
        HWND dimCombo = ui::CreateStyledCombo(hwnd, IDC_DIM_AMOUNT, kCheckCol, rDimAmount, 180);
        {
            const wchar_t* names[] = { L"Off", L"25%", L"50%", L"75%", L"90%" };
            const int vals[] = { 0, 25, 50, 75, 90 };
            int cur = GetDimAmount();
            for (int i = 0; i < 5; ++i) {
                int idx = static_cast<int>(SendMessageW(dimCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i])));
                SendMessageW(dimCombo, CB_SETITEMDATA, idx, vals[i]);
                if (vals[i] == cur) {
                    SendMessageW(dimCombo, CB_SETCURSEL, idx, 0);
                }
            }
            if (SendMessageW(dimCombo, CB_GETCURSEL, 0, 0) == CB_ERR) {
                SendMessageW(dimCombo, CB_SETCURSEL, 0, 0);  // default to Off
            }
        }

        label(L"Dim after:", rDimAfter, 180);
        HWND afterCombo = ui::CreateStyledCombo(hwnd, IDC_DIM_AFTER, kCheckCol, rDimAfter, 180);
        {
            const wchar_t* names[] = { L"1 minute", L"2 minutes", L"5 minutes", L"10 minutes", L"15 minutes", L"30 minutes" };
            const int vals[] = { 60, 120, 300, 600, 900, 1800 };
            int cur = GetDimAfterSeconds();
            for (int i = 0; i < 6; ++i) {
                int idx = static_cast<int>(SendMessageW(afterCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(names[i])));
                SendMessageW(afterCombo, CB_SETITEMDATA, idx, vals[i]);
                if (vals[i] == cur) {
                    SendMessageW(afterCombo, CB_SETCURSEL, idx, 0);
                }
            }
            if (SendMessageW(afterCombo, CB_GETCURSEL, 0, 0) == CB_ERR) {
                SendMessageW(afterCombo, CB_SETCURSEL, 2, 0);  // default to 5 minutes
            }
        }

        // Row 1: "Start with Windows" checkbox and the high-priority button share the control
        // column. When the service is on the checkbox is hidden and the button takes its place.
        help(r1, IDC_HELP_STARTUP);
        label(L"Start with Windows:", r1, 180);
        check(r1, IDC_STARTUP);
        CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      S(kCheckCol), S(r1 - 15), S(180), S(30), hwnd,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_HIPRIO_BTN)), g_hInst, nullptr);

        help(r2, IDC_HELP_PROTECT);
        label(L"Protect against crashes:", r2, 200);
        check(r2, IDC_PROTECT);

        help(r3, IDC_HELP_HIBER);
        label(L"Safe start after hibernation:", r3, 208);
        check(r3, IDC_SAFEHIBER);

        bool service = IsServiceInstalled();
        g_serviceOn = service;
        bool startup = IsStartupEnabled() || service;
        Button_SetCheck(GetDlgItem(hwnd, IDC_STARTUP), startup ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(GetDlgItem(hwnd, IDC_PROTECT), GetProtectAgainstCrashes() ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(GetDlgItem(hwnd, IDC_SAFEHIBER), IsSafeStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(GetDlgItem(hwnd, IDC_HIDE), IsEnabled() ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(GetDlgItem(hwnd, IDC_STARTUP), service ? SW_HIDE : SW_SHOW);
        EnableWindow(GetDlgItem(hwnd, IDC_HIPRIO_BTN), startup);
        EnableWindow(GetDlgItem(hwnd, IDC_PROTECT), service);
        EnableWindow(GetDlgItem(hwnd, IDC_SAFEHIBER), service);   // WE: requires the high-priority service
        LayoutHiprioButton(hwnd);

        // Dark tooltips on the "?" glyphs. Dropping the visual style (SetWindowTheme L"",L"")
        // lets the TTM_SETTIP*COLOR messages take effect - a themed tooltip ignores them.
        HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   hwnd, nullptr, g_hInst, nullptr);
        if (tip != nullptr) {
            SetWindowTheme(tip, L"", L"");
            SendMessageW(tip, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
            SendMessageW(tip, TTM_SETTIPBKCOLOR, static_cast<WPARAM>(kMenuBg), 0);
            SendMessageW(tip, TTM_SETTIPTEXTCOLOR, static_cast<WPARAM>(kText), 0);
            RECT tipMargin = { S(12), S(8), S(12), S(8) };
            SendMessageW(tip, TTM_SETMARGIN, 0, reinterpret_cast<LPARAM>(&tipMargin));
            SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, S(260));
            // Round the corners and recolor the DWM border so it matches the dark UI
            // (otherwise Win11 draws a square, white-bordered popup).
            DWM_WINDOW_CORNER_PREFERENCE tipCorner = DWMWCP_ROUND;
            DwmSetWindowAttribute(tip, DWMWA_WINDOW_CORNER_PREFERENCE, &tipCorner, sizeof(tipCorner));
            COLORREF tipBorder = kMenuBorder;
            DwmSetWindowAttribute(tip, DWMWA_BORDER_COLOR, &tipBorder, sizeof(tipBorder));
            auto addTip = [&](UINT id, const wchar_t* text) {
                TOOLINFOW ti = {};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd = hwnd;
                ti.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(hwnd, id));
                ti.lpszText = const_cast<LPWSTR>(text);
                SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            };
            addTip(IDC_HELP_STARTUP, L"\"Start with Windows\" launches OledCare automatically at sign-in "
                                     L"using the normal Windows startup list. Windows delays that list, so "
                                     L"there is a short gap after login before OledCare is running and its "
                                     L"settings take effect.\r\n\r\n"
                                     L"\"Set high priority\" instead installs a small Windows service that "
                                     L"starts OledCare earlier in the boot sequence, so it is up and running "
                                     L"sooner and more reliably. Without high priority OledCare still starts "
                                     L"automatically - just a little later.");
            addTip(IDC_HELP_PROTECT, L"When on, the high-priority service automatically relaunches "
                                     L"OledCare if it ever crashes.");
            addTip(IDC_HELP_HIBER, L"After the PC resumes from sleep or hibernation, re-apply tray hiding in "
                                   L"case the shell rebuilt the taskbar. Requires the high-priority service.");
            SendMessageW(tip, TTM_ACTIVATE, TRUE, 0);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        int dpi = GetDpiForWindow(hwnd);
        auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
        RECT cr;
        GetClientRect(hwnd, &cr);
        FillRect(dc, &cr, g_darkBrush);
        SetBkMode(dc, TRANSPARENT);
        HFONT of = static_cast<HFONT>(SelectObject(dc, g_uiFont));
        // Draw a group box whose header sits on (and breaks) the top border line.
        auto group = [&](int top, int bottom, const wchar_t* hdr) {
            int left = S(16), right = cr.right - S(16), t = S(top);
            HPEN pen = CreatePen(PS_SOLID, 1, kGroupBorder);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HGDIOBJ op = SelectObject(dc, pen);
            RoundRect(dc, left, t, right, S(bottom), S(8), S(8));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            SIZE ts;
            GetTextExtentPoint32W(dc, hdr, lstrlenW(hdr), &ts);
            int hx = S(32), pad = S(6);
            RECT clear = { hx - pad, t - ts.cy / 2, hx + ts.cx + pad, t + ts.cy / 2 };
            FillRect(dc, &clear, g_darkBrush);   // break the border line behind the header text
            SetTextColor(dc, kText);
            TextOutW(dc, hx, t - ts.cy / 2, hdr, lstrlenW(hdr));
        };
        using namespace Layout;
        group(SysTrayTop, SysTrayBottom, L"System Tray");
        group(TaskbarTop, TaskbarBottom, L"Taskbar");
        group(DimTop, DimBottom, L"Screen Dimming");
        group(StartupTop, StartupBottom, L"Automatic startup");
        group(AppearanceTop, AppearanceBottom, L"Appearance");
        SelectObject(dc, of);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MEASUREITEM: {
        auto mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlType == ODT_COMBOBOX) {
            mis->itemHeight = MulDiv(24, GetDpiForWindow(hwnd), 96);
        }
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_COMBOBOX) {
            if ((dis->itemState & ODS_COMBOBOXEDIT) != 0) {
                return TRUE;  // closed field is painted by ComboSubclass
            }
            if (dis->itemID != static_cast<UINT>(-1)) {
                int dpi = GetDpiForWindow(hwnd);
                bool selected = (dis->itemState & ODS_SELECTED) != 0;
                HBRUSH bb = CreateSolidBrush(selected ? kMenuHover : kMenuBg);
                FillRect(dis->hDC, &dis->rcItem, bb);
                DeleteObject(bb);
                wchar_t text[80] = {};
                SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, kText);
                HFONT of = static_cast<HFONT>(SelectObject(dis->hDC, g_uiFont));
                RECT tr = dis->rcItem;
                tr.left += MulDiv(12, dpi, 96);  // align with the closed box's text
                DrawTextW(dis->hDC, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, of);
            }
            return TRUE;
        }
        if (dis->CtlID == IDC_HOTKEY) {
            int dpi = GetDpiForWindow(hwnd);
            auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
            RECT rc = dis->rcItem;
            FillRect(dis->hDC, &rc, g_darkBrush);  // blend the rounded corners into the background
            HBRUSH br = CreateSolidBrush(g_effectiveDark ? RGB(45, 45, 45) : RGB(251, 251, 251));
            HPEN pen = CreatePen(PS_SOLID, 1, kMenuBorder);
            HGDIOBJ ob = SelectObject(dis->hDC, br);
            HGDIOBJ op = SelectObject(dis->hDC, pen);
            RoundRect(dis->hDC, rc.left, rc.top, rc.right - 1, rc.bottom - 1, S(6), S(6));
            SelectObject(dis->hDC, ob);
            SelectObject(dis->hDC, op);
            DeleteObject(br);
            DeleteObject(pen);
            wchar_t text[80] = {};
            GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, kText);
            HFONT of = static_cast<HFONT>(SelectObject(dis->hDC, g_uiFont));
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, of);
            return TRUE;
        }
        if (dis->CtlID == IDC_HIPRIO_BTN) {
            int dpi = GetDpiForWindow(hwnd);
            bool on = g_serviceOn;
            bool enabled = IsWindowEnabled(dis->hwndItem) != FALSE;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF col = !enabled ? RGB(70, 70, 70)
                           : on ? (pressed ? RGB(46, 122, 50) : RGB(56, 142, 60))
                                : (pressed ? RGB(28, 88, 180) : RGB(33, 103, 210));
            HBRUSH br = CreateSolidBrush(col);
            HPEN pen = CreatePen(PS_SOLID, 1, col);
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(dis->hDC, br));
            HPEN op = static_cast<HPEN>(SelectObject(dis->hDC, pen));
            int rad = MulDiv(8, dpi, 96);
            RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, rad, rad);
            SelectObject(dis->hDC, ob);
            SelectObject(dis->hDC, op);
            DeleteObject(br);
            DeleteObject(pen);
            int ico = MulDiv(16, dpi, 96);
            int iy = dis->rcItem.top + ((dis->rcItem.bottom - dis->rcItem.top) - ico) / 2;
            if (g_shieldIcon != nullptr) {
                DrawIconEx(dis->hDC, dis->rcItem.left + MulDiv(12, dpi, 96), iy, g_shieldIcon, ico, ico, 0, nullptr, DI_NORMAL);
            }
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, RGB(255, 255, 255));
            HFONT of = static_cast<HFONT>(SelectObject(dis->hDC, g_uiFont));
            RECT tr = dis->rcItem;
            tr.left += MulDiv(34, dpi, 96);
            DrawTextW(dis->hDC, on ? L"Remove high priority" : L"Set high priority", -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, of);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        const UINT code = HIWORD(wParam);
        HWND ctrl = reinterpret_cast<HWND>(lParam);

        if (code == CBN_SELCHANGE) {
            const LRESULT data = ui::SelectedComboData(ctrl);
            if (data == CB_ERR) {
                return 0;
            }
            switch (id) {
            case IDC_PEEKSECS:    SetPeekSeconds(static_cast<int>(data)); return 0;
            case IDC_FADEIN:      SetRegDword(contract::kRegFadeInMs, static_cast<DWORD>(data)); return 0;
            case IDC_FADEOUT:     SetRegDword(contract::kRegFadeOutMs, static_cast<DWORD>(data)); return 0;
            case IDC_THEME:       g_themeMode = static_cast<int>(data); SetThemeMode(g_themeMode); ApplySettingsTheme(hwnd); return 0;
            case IDC_TASKBAR:     SetTaskbarMode(static_cast<TaskbarMode>(data)); return 0;
            case IDC_TASKBAR_MON: SetTaskbarMonitor(static_cast<int>(data)); return 0;
            case IDC_HIDE_TARGET: SetHideMonitor(static_cast<int>(data)); ApplyHideAllEvent(); return 0;
            case IDC_DIM_AMOUNT:  SetDimAmount(static_cast<int>(data)); return 0;
            case IDC_DIM_AFTER:   SetDimAfterSeconds(static_cast<int>(data)); return 0;
            default:              return 0;
            }
        }

        if (code == BN_CLICKED) {
            switch (id) {
            case IDC_HIPRIO_BTN:
                ToggleHighPriority(hwnd);
                break;
            case IDC_STARTUP: {
                bool checked = Button_GetCheck(ctrl) == BST_CHECKED;
                SetStartup(checked);
                EnableWindow(GetDlgItem(hwnd, IDC_HIPRIO_BTN), checked);
                break;
            }
            case IDC_PROTECT:
                RunElevated(Button_GetCheck(ctrl) == BST_CHECKED ? L"--set-protect 1" : L"--set-protect 0");
                break;
            case IDC_SAFEHIBER:
                SetSafeStartEnabled(Button_GetCheck(ctrl) == BST_CHECKED);
                break;
            case IDC_HIDE:
                SetEnabled(Button_GetCheck(ctrl) == BST_CHECKED);
                break;
            case IDC_HOTKEY:
                g_capturingHotkey = true;
                SetWindowTextW(GetDlgItem(hwnd, IDC_HOTKEY), L"Press a key...");
                SetFocus(hwnd);  // route the next keystroke to this window, not the button
                break;
            default:
                break;
            }
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto nmh = reinterpret_cast<LPNMHDR>(lParam);
        UINT id = static_cast<UINT>(nmh->idFrom);
        bool isCheck = (id == IDC_HIDE || id == IDC_STARTUP || id == IDC_PROTECT ||
                        id == IDC_SAFEHIBER);
        if (nmh->code == NM_CUSTOMDRAW && isCheck) {
            auto cd = reinterpret_cast<LPNMCUSTOMDRAW>(lParam);
            if (cd->dwDrawStage != CDDS_PREPAINT) {
                return CDRF_DODEFAULT;
            }
            HDC dc = cd->hdc;
            RECT rc = cd->rc;
            int dpi = GetDpiForWindow(nmh->hwndFrom);
            auto SS = [dpi](int v) { return MulDiv(v, dpi, 96); };
            bool checked = Button_GetCheck(nmh->hwndFrom) == BST_CHECKED;
            bool enabled = IsWindowEnabled(nmh->hwndFrom) != FALSE;
            FillRect(dc, &rc, g_darkBrush);  // blend rounded corners into the window background
            COLORREF boxFill = checked ? (enabled ? kAccent : RGB(82, 82, 82)) : kBg;
            COLORREF boxBorder = checked ? boxFill : kMenuBorder;
            HBRUSH br = CreateSolidBrush(boxFill);
            HPEN pen = CreatePen(PS_SOLID, 1, boxBorder);
            HGDIOBJ ob = SelectObject(dc, br);
            HGDIOBJ op = SelectObject(dc, pen);
            RoundRect(dc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, SS(4), SS(4));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
            DeleteObject(pen);
            if (checked) {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen cpen(Gdiplus::Color(255, GetRValue(kText), GetGValue(kText), GetBValue(kText)),
                                  static_cast<Gdiplus::REAL>(SS(2)));
                cpen.SetStartCap(Gdiplus::LineCapRound);
                cpen.SetEndCap(Gdiplus::LineCapRound);
                cpen.SetLineJoin(Gdiplus::LineJoinRound);
                int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
                Gdiplus::Point p[3] = {
                    { cx - SS(4), cy },
                    { cx - SS(1), cy + SS(3) },
                    { cx + SS(4), cy - SS(3) },
                };
                g.DrawLines(&cpen, p, 3);
            }
            return CDRF_SKIPDEFAULT;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        int cid = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        bool isHelp = (cid == IDC_HELP_STARTUP || cid == IDC_HELP_PROTECT || cid == IDC_HELP_HIBER);
        SetTextColor(dc, isHelp ? kAccent : kText);
        SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(g_darkBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(g_darkBrush);
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, g_darkBrush);
        return 1;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capturingHotkey) {
            UINT vk = static_cast<UINT>(wParam);
            if (vk == VK_ESCAPE) {  // cancel - restore the current binding
                g_capturingHotkey = false;
                SetWindowTextW(GetDlgItem(hwnd, IDC_HOTKEY), HotkeyToString(g_hotkeyMods, g_hotkeyVk).c_str());
                return 0;
            }
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                vk == VK_LWIN || vk == VK_RWIN) {
                return 0;  // a modifier alone isn't a hotkey - wait for the real key
            }
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) { mods |= MOD_CONTROL; }
            if (GetKeyState(VK_MENU) & 0x8000)    { mods |= MOD_ALT; }
            if (GetKeyState(VK_SHIFT) & 0x8000)   { mods |= MOD_SHIFT; }
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) { mods |= MOD_WIN; }
            UINT prevMods = g_hotkeyMods, prevVk = g_hotkeyVk;
            g_hotkeyMods = mods;
            g_hotkeyVk = vk;
            g_capturingHotkey = false;
            if (RegisterRevealHotkey()) {
                SaveHotkey();
            } else {  // taken by another app - revert
                g_hotkeyMods = prevMods;
                g_hotkeyVk = prevVk;
                RegisterRevealHotkey();
                MessageBoxW(hwnd, L"That shortcut is already in use by another application. Choose a different one.",
                            L"Peek hotkey", MB_OK | MB_ICONWARNING);
            }
            SetWindowTextW(GetDlgItem(hwnd, IDC_HOTKEY), HotkeyToString(g_hotkeyMods, g_hotkeyVk).c_str());
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

HWND g_settingsWnd = nullptr;

void ShowSettings() {
    EnsureSettingsClass();  // register the window class on first open
    if (g_settingsWnd != nullptr && IsWindow(g_settingsWnd)) {
        SetForegroundWindow(g_settingsWnd);
        return;
    }
    int dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    // Size to an exact client area so the box layout's top inset equals the bottom inset,
    // regardless of the title-bar/frame thickness.
    RECT rc = { 0, 0, MulDiv(Layout::WinWidth, dpi, 96), MulDiv(Layout::WinHeight, dpi, 96) };
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_settingsWnd = CreateWindowExW(0, kSettingsClass, L"Settings",
                                    style, x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
    if (g_settingsWnd == nullptr) {
        return;
    }
    BOOL dark = g_effectiveDark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_settingsWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    AllowWindowDarkMode(g_settingsWnd, g_effectiveDark);
    // Set the caption/taskbar icon explicitly; DWM dark captions don't reliably pick up the
    // class icon, so a per-window WM_SETICON is needed for the title-bar icon to appear.
    SendMessageW(g_settingsWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIconSm));
    SendMessageW(g_settingsWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
    ShowWindow(g_settingsWnd, SW_SHOW);
    SetForegroundWindow(g_settingsWnd);
}

}  // namespace ui
