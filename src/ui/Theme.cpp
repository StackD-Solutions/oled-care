// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/Theme.h"

#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

// Definitions for the shared theme state + the window/theme plumbing the palette is applied through.
// The color globals start at the dark palette; ApplyTheme() (in SettingsModel.cpp) reassigns them for
// the light/system selection, and the fonts/brush are created at startup. Kept in their own
// translation unit so every UI component links one shared copy.
namespace theme {

COLORREF kBg = RGB(32, 32, 32);
COLORREF kMenuBg = RGB(43, 43, 43);
COLORREF kMenuHover = RGB(61, 61, 61);
COLORREF kMenuBorder = RGB(60, 60, 60);
COLORREF kSeparator = RGB(70, 70, 70);
COLORREF kText = RGB(243, 243, 243);
COLORREF kGroupBorder = RGB(80, 80, 80);

bool g_effectiveDark = true;

HBRUSH g_darkBrush = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_menuFont = nullptr;

void SetProcessDarkMode(bool dark) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux == nullptr) {
        return;
    }
    enum AppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
    typedef AppMode(WINAPI * SetPreferredAppModeFn)(AppMode);
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    if (setMode != nullptr) {
        setMode(dark ? ForceDark : ForceLight);
    }
}

void AllowWindowDarkMode(HWND hwnd, bool dark) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux == nullptr) {
        return;
    }
    typedef bool(WINAPI * AllowDarkModeForWindowFn)(HWND, bool);
    auto allowForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    if (allowForWindow != nullptr) {
        allowForWindow(hwnd, dark);
    }
}

bool SystemUsesLightTheme() {
    HKEY key;
    DWORD v = 1, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &sz);
        RegCloseKey(key);
    }
    return v != 0;
}

// Internal: a blank 32bpp top-down DIB section to render a glyph into.
static HBITMAP MakeArgbBitmap(int px, void** bitsOut) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bitsOut, nullptr, 0);
    ReleaseDC(nullptr, dc);
    return hbmp;
}

HBITMAP MakeGlyphBitmap(const wchar_t* glyph, int px, COLORREF color) {
    void* bits = nullptr;
    HBITMAP hbmp = MakeArgbBitmap(px, &bits);
    if (hbmp == nullptr) {
        return nullptr;
    }
    {
        Gdiplus::Bitmap bmp(px, px, px * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        Gdiplus::FontFamily ff(L"Segoe Fluent Icons");
        Gdiplus::Font font(&ff, px * 0.6f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rect(0.0f, 0.0f, static_cast<Gdiplus::REAL>(px), static_cast<Gdiplus::REAL>(px));
        g.DrawString(glyph, -1, &font, rect, &fmt, &brush);
    }
    return hbmp;
}

}  // namespace theme
