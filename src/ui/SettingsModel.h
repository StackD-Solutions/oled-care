// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

#include <string>

// The settings model: every registry-backed setting (HKCU\Software\OledCare), theme resolution,
// monitor enumeration, the reveal hotkey, peek duration, and the high-priority elevation hook. This
// is pure business logic over the registry + the shared app events - no window code. Both the
// settings window (ui::) and the tray host (main.cpp) drive it; main brings it in with
// `using namespace settings;` so its many call sites stay unqualified.
namespace settings {

// Registry-backed DWORD accessors - the single place that touches HKCU\Software\OledCare; every
// typed setting below funnels through these.
DWORD GetRegDword(const wchar_t* name, DWORD def);
void SetRegDword(const wchar_t* name, DWORD val);

// Theme: read/write the ThemeMode setting and resolve it -> effective colours, background brush,
// process theme, and menu glyphs. The dark-mode / glyph-rendering plumbing lives in ui/Theme.
int GetThemeMode();
void SetThemeMode(int mode);
void ApplyTheme();

// Enable/disable tray hiding + run-at-startup.
bool IsEnabled();
void SetEnabled(bool enabled);
bool IsStartupEnabled();
void SetStartup(bool enable);
bool IsSafeStartEnabled();
void SetSafeStartEnabled(bool on);

// Taskbar appearance (Normal = Windows default, Clear = fully transparent) + which monitor it / the
// tray-hide target apply to (0 = all monitors, 1..N = a specific one).
enum class TaskbarMode { Normal = 0, Clear = 2 };
TaskbarMode GetTaskbarMode();
void SetTaskbarMode(TaskbarMode mode);
int GetTaskbarMonitor();
void SetTaskbarMonitor(int monitor);
int GetHideMonitor();
void SetHideMonitor(int monitor);
int CountMonitors();
void PopulateMonitorCombo(HWND combo, int current);
void ApplyHideAllEvent();

// Reveal hotkey.
void LoadHotkey();
void SaveHotkey();
bool RegisterRevealHotkey();
std::wstring HotkeyToString(UINT mods, UINT vk);

// Peek auto-hide duration + privileged relaunch.
int GetPeekSeconds();
void SetPeekSeconds(int seconds);
void RunElevated(const wchar_t* args);

// Screen Dimming: idle-dim amount (0 = Off, else % of black overlay) + the idle threshold.
int GetDimAmount();
void SetDimAmount(int percent);
int GetDimAfterSeconds();
void SetDimAfterSeconds(int seconds);

// Live settings state.
extern int g_themeMode;          // 0 = system, 1 = light, 2 = dark
extern UINT g_hotkeyMods;        // reveal-hotkey modifiers
extern UINT g_hotkeyVk;          // reveal-hotkey virtual key

// Tray-menu glyph bitmaps, rebuilt by ApplyTheme so they contrast with the menu background. The tray
// host's menu items point at these (by address) and the host sets g_iconPx before the first
// ApplyTheme.
extern int g_iconPx;
extern HBITMAP g_iconSettings;
extern HBITMAP g_iconQuit;
extern HBITMAP g_iconInfo;

}  // namespace settings
