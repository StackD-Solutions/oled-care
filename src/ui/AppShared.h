// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

// App-lifecycle globals owned by the tray host (main.cpp) but needed by the settings model
// (settings::) and the settings window (ui::). main defines them at global scope; the other two
// translation units see them through these extern declarations. Keeping the boundary explicit here
// means neither the model nor the window reaches back into main.cpp's internals for anything else.

extern HINSTANCE g_hInst;       // app instance (child-control + window creation)
extern HWND g_mainWnd;          // the hidden host window (reveal-hotkey target, quit)
extern HANDLE g_disabledEvent;  // signalled => tray hiding is OFF (the injected hook polls it)
extern HANDLE g_hideAllEvent;   // signalled => hide the tray on every monitor
extern HICON g_appIcon;         // large window / taskbar icon
extern HICON g_appIconSm;       // small title-bar / tray icon
extern HICON g_shieldIcon;      // UAC shield glyph (the high-priority button)

inline constexpr int kHotkeyId = 1;  // RegisterHotKey id for the reveal hotkey (shared by model + host)
