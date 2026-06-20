// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/SettingsModel.h"

#include <shellapi.h>

#include "ui/AppShared.h"
#include "ui/SettingsLogic.h"
#include "ui/Theme.h"
#include "injector.h"
#include "AppContract.h"

using namespace theme;  // ApplyTheme draws on the shared palette + dark-mode/glyph helpers (ui/Theme.h)

namespace settings {

// HKCU locations this model reads/writes.
const wchar_t* kRegPath = contract::kRegPath;     // settings root (HKCU\Software\OledCare)
const wchar_t* kRunKey = contract::kRunKey;       // run-at-startup
const wchar_t* kRunValue = contract::kRunValue;

// Segoe Fluent Icons glyphs (built from codepoints so source encoding never matters).
const wchar_t kGlyphGear[] = { (wchar_t)0xE713, 0 };   // Settings (gear)
const wchar_t kGlyphPower[] = { (wchar_t)0xE7E8, 0 };  // PowerButton
const wchar_t kGlyphInfo[] = { (wchar_t)0xE946, 0 };   // Info

// Live settings state (declared extern in SettingsModel.h).
int g_themeMode = 0;                          // 0 = system, 1 = light, 2 = dark
UINT g_hotkeyMods = MOD_CONTROL | MOD_ALT;    // reveal hotkey, default Ctrl+Alt+F10
UINT g_hotkeyVk = VK_F10;
int g_iconPx = 16;
HBITMAP g_iconSettings = nullptr;
HBITMAP g_iconQuit = nullptr;
HBITMAP g_iconInfo = nullptr;

// ---- theme ----------------------------------------------------------------
// Dark-mode + glyph-bitmap plumbing lives in ui/Theme; ApplyTheme calls it through theme::.

int GetThemeMode() {
    return clampThemeMode(GetRegDword(L"ThemeMode", 0));
}

void SetThemeMode(int mode) {
    SetRegDword(L"ThemeMode", static_cast<DWORD>(mode));
}

// Resolve g_themeMode -> effective colors, background brush, process theme, and the menu
// glyph bitmaps (which must contrast with the menu background).
void ApplyTheme() {
    g_effectiveDark = (g_themeMode == 2) || (g_themeMode == 0 && !SystemUsesLightTheme());
    if (g_effectiveDark) {
        kBg = RGB(32, 32, 32);   kText = RGB(243, 243, 243);
        kMenuBg = RGB(43, 43, 43);   kMenuHover = RGB(61, 61, 61);
        kMenuBorder = RGB(60, 60, 60);   kSeparator = RGB(70, 70, 70);
        kGroupBorder = RGB(80, 80, 80);
    } else {
        kBg = RGB(243, 243, 243);   kText = RGB(26, 26, 26);
        kMenuBg = RGB(249, 249, 249);   kMenuHover = RGB(229, 229, 229);
        kMenuBorder = RGB(197, 197, 197);   kSeparator = RGB(214, 214, 214);
        kGroupBorder = RGB(189, 189, 189);
    }
    if (g_darkBrush != nullptr) {
        DeleteObject(g_darkBrush);
    }
    g_darkBrush = CreateSolidBrush(kBg);
    SetProcessDarkMode(g_effectiveDark);
    if (g_iconPx > 0) {
        if (g_iconSettings != nullptr) { DeleteObject(g_iconSettings); }
        if (g_iconQuit != nullptr) { DeleteObject(g_iconQuit); }
        if (g_iconInfo != nullptr) { DeleteObject(g_iconInfo); }
        g_iconSettings = MakeGlyphBitmap(kGlyphGear, g_iconPx, kText);
        g_iconQuit = MakeGlyphBitmap(kGlyphPower, g_iconPx, kText);
        g_iconInfo = MakeGlyphBitmap(kGlyphInfo, g_iconPx, kText);
    }
}

// ---- enable/disable + run-at-startup --------------------------------------

bool IsEnabled() {
    return g_disabledEvent == nullptr || WaitForSingleObject(g_disabledEvent, 0) != WAIT_OBJECT_0;
}

void SetEnabled(bool enabled) {
    if (g_disabledEvent == nullptr) {
        return;
    }
    if (enabled) {
        ResetEvent(g_disabledEvent);
        InjectHook();
    } else {
        SetEvent(g_disabledEvent);
    }
}

bool IsStartupEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

void SetStartup(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// "Safe start after hibernation": re-apply tray hiding after the PC resumes (HKCU, no admin).
bool IsSafeStartEnabled() {
    return GetRegDword(L"SafeStartAfterResume", 0) != 0;
}
void SetSafeStartEnabled(bool on) {
    SetRegDword(L"SafeStartAfterResume", on ? 1 : 0);
}

// Generic HKCU\Software\OledCare DWORD accessors - the single place that touches the registry;
// every typed setting above/below funnels through these.
DWORD GetRegDword(const wchar_t* name, DWORD def) {
    HKEY key;
    DWORD v = def, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &sz);
        RegCloseKey(key);
    }
    return v;
}

void SetRegDword(const wchar_t* name, DWORD val) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }
}

// ---- taskbar appearance ---------------------------------------------------
//
// Two modes: Normal (the Windows-configured taskbar, untouched) and Clear (fully transparent -
// the hook hides the taskbar's background fill so the desktop shows through). The hook, injected
// in explorer, reads the mode from the registry and applies it; the app just persists the choice.
// OledCare does not touch any Windows appearance/transparency settings.
// TaskbarMode is declared in SettingsModel.h.

TaskbarMode GetTaskbarMode() {
    // Default to Clear (transparent) when unset - that's OledCare's out-of-the-box look.
    return taskbarModeIsClear(GetRegDword(contract::kRegTaskbarMode, 2)) ? TaskbarMode::Clear : TaskbarMode::Normal;
}
void SetTaskbarMode(TaskbarMode mode) {
    SetRegDword(contract::kRegTaskbarMode, static_cast<DWORD>(mode));
}

// Which monitor the taskbar color applies to: 0 = all monitors, 1..N = that specific monitor.
int GetTaskbarMonitor() {
    return static_cast<int>(GetRegDword(L"TaskbarMonitor", 0));  // default: all monitors
}
void SetTaskbarMonitor(int monitor) {
    SetRegDword(L"TaskbarMonitor", static_cast<DWORD>(monitor));
}

// Number of physical monitors, so the target dropdowns can list Monitor 1..N.
int CountMonitors() {
    int n = GetSystemMetrics(SM_CMONITORS);
    return (n < 1) ? 1 : n;
}

// Which monitor "Hide system tray" applies to: 0 = all monitors, 1..N = that specific monitor.
int GetHideMonitor() {
    return static_cast<int>(GetRegDword(L"HideMonitor", 0));  // default: all monitors
}
void SetHideMonitor(int monitor) {
    SetRegDword(L"HideMonitor", static_cast<DWORD>(monitor));
}

// Fill a combo with "All monitors" + "Monitor 1..N" and select the entry matching `current`
// (0 = all). Shared by the taskbar-color target and the tray-hide target dropdowns.
void PopulateMonitorCombo(HWND combo, int current) {
    int idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All monitors")));
    SendMessageW(combo, CB_SETITEMDATA, idx, 0);
    const int n = CountMonitors();
    for (int m = 1; m <= n; ++m) {
        wchar_t buf[32];
        wsprintfW(buf, L"Monitor %d", m);
        idx = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf)));
        SendMessageW(combo, CB_SETITEMDATA, idx, m);
    }
    int sel = 0;  // default to "All monitors" if `current` is out of range
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, i, 0)) == current) {
            sel = i;
            break;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

// Mirror the setting into the shared event the hook polls.
void ApplyHideAllEvent() {
    if (g_hideAllEvent == nullptr) {
        return;
    }
    // Target "All monitors" (0) hides the tray on every taskbar; a specific monitor hides only the
    // primary for now (per-monitor tray targeting is wired into the hook separately).
    if (GetHideMonitor() == 0) {
        SetEvent(g_hideAllEvent);
    } else {
        ResetEvent(g_hideAllEvent);
    }
}

// ---- reveal hotkey --------------------------------------------------------

void LoadHotkey() {
    g_hotkeyMods = GetRegDword(L"HotkeyMods", g_hotkeyMods);  // keep current default if unset
    DWORD vk = GetRegDword(L"HotkeyVk", 0);
    if (vk != 0) {
        g_hotkeyVk = vk;
    }
}

void SaveHotkey() {
    SetRegDword(L"HotkeyMods", g_hotkeyMods);
    SetRegDword(L"HotkeyVk", g_hotkeyVk);
}

// (Re)register the global hotkey on the main window. Returns false if the combo is taken.
bool RegisterRevealHotkey() {
    if (g_mainWnd == nullptr) {
        return false;
    }
    UnregisterHotKey(g_mainWnd, kHotkeyId);
    return RegisterHotKey(g_mainWnd, kHotkeyId, g_hotkeyMods | MOD_NOREPEAT, g_hotkeyVk) != FALSE;
}

// Human-readable form, e.g. "Ctrl + Alt + F10", for the settings button.
std::wstring HotkeyToString(UINT mods, UINT vk) {
    return formatHotkey(mods, vk);  // pure logic lives in ui/SettingsLogic.h (unit-tested)
}

// Peek auto-hide duration (seconds) for the reveal hotkey. Stored in HKCU; default 3.
int GetPeekSeconds() {
    return clampPeekSeconds(GetRegDword(L"PeekSeconds", 3));
}

void SetPeekSeconds(int seconds) {
    SetRegDword(L"PeekSeconds", static_cast<DWORD>(seconds));
}

// Screen Dimming. Stored in HKCU. Dim amount defaults to 0 (Off); idle threshold to 300s.
int GetDimAmount() {
    return clampDimAmount(GetRegDword(L"DimAmount", 0));
}
void SetDimAmount(int percent) {
    SetRegDword(L"DimAmount", static_cast<DWORD>(percent));
}
int GetDimAfterSeconds() {
    return clampDimAfterSeconds(GetRegDword(L"DimAfterSeconds", 300));
}
void SetDimAfterSeconds(int seconds) {
    SetRegDword(L"DimAfterSeconds", static_cast<DWORD>(seconds));
}

// Re-launch ourselves elevated to run a privileged mode (service install/uninstall, etc.).
void RunElevated(const wchar_t* args) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ShellExecuteW(nullptr, L"runas", exe, args, nullptr, SW_HIDE);
}
}  // namespace settings
