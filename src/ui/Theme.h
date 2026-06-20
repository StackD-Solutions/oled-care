// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

// Shared theme state for the tray app's UI components: the dark/light palette (reassigned at runtime
// by ApplyTheme), the resolved-dark flag, and the shared GDI font/brush objects. Components include
// this header instead of reaching into main.cpp's globals, so the UI pieces can live in their own
// files while still drawing with one consistent palette.
namespace theme {

// Palette - mutable: ApplyTheme() reassigns these for the dark / light / system selection.
extern COLORREF kBg;
extern COLORREF kMenuBg;
extern COLORREF kMenuHover;
extern COLORREF kMenuBorder;
extern COLORREF kSeparator;
extern COLORREF kText;
extern COLORREF kGroupBorder;

// Constant brand accent (#AFFF33) - checkbox fill + the help "?" accent.
inline const COLORREF kAccent = RGB(175, 255, 51);

// Resolved theme: true when the effective appearance is dark (system -> the OS's current app theme).
extern bool g_effectiveDark;

// Shared GDI objects, created at startup, reused across the tray app's windows.
extern HBRUSH g_darkBrush;
extern HFONT g_uiFont;
extern HFONT g_menuFont;

// Window / process dark-mode plumbing (undocumented uxtheme ordinals) + the OS light/dark query.
// ApplyTheme() and any window that needs an immersive dark frame call these.
void SetProcessDarkMode(bool dark);              // process-wide preferred app mode (force dark / light)
void AllowWindowDarkMode(HWND hwnd, bool dark);  // per-window dark frame opt-in
bool SystemUsesLightTheme();                     // OS "apps use light theme" flag (for System mode)

// Render a Segoe Fluent Icons glyph to a 32bpp premultiplied-ARGB bitmap in `color`. Builds the tray
// menu's themed glyph icons so they contrast with the menu background.
HBITMAP MakeGlyphBitmap(const wchar_t* glyph, int px, COLORREF color);

}  // namespace theme
