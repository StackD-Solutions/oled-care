// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/components/AboutDialog.h"

#include <dwmapi.h>

#include "ui/Theme.h"
#include "ui/components/AppIcon.h"
#include "AppContract.h"

#pragma comment(lib, "dwmapi.lib")

using namespace theme;

namespace ui {

namespace {

const wchar_t* kAboutClass = L"OledCareAboutWnd";
HWND g_aboutWnd = nullptr;
HFONT g_aboutTitleFont = nullptr;  // lazily created on first paint

LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, g_darkBrush);
        return 1;
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

        int iconH = S(80), iconW = MulDiv(iconH, 519, 590);
        DrawAppIcon(dc, (cr.right - iconW) / 2, S(22), iconW, iconH);

        if (g_aboutTitleFont == nullptr) {
            g_aboutTitleFont = CreateFontW(-MulDiv(18, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        HFONT of = static_cast<HFONT>(SelectObject(dc, g_aboutTitleFont));
        SetTextColor(dc, kText);
        RECT tr = { 0, S(118), cr.right, S(146) };
        DrawTextW(dc, contract::kProductName, -1, &tr, DT_CENTER | DT_SINGLELINE);

        SelectObject(dc, g_uiFont);
        SetTextColor(dc, RGB(150, 150, 150));
        RECT vr = { 0, S(150), cr.right, S(170) };
        DrawTextW(dc, L"Version " OLEDCARE_VERSION_W, -1, &vr, DT_CENTER | DT_SINGLELINE);
        RECT cpr = { 0, S(182), cr.right, S(202) };
        DrawTextW(dc, L"\u00A9 2026 StackD Solutions", -1, &cpr, DT_CENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_aboutWnd = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void EnsureClassRegistered() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW ac = {};
    ac.cbSize = sizeof(ac);
    ac.lpfnWndProc = AboutProc;
    ac.hInstance = GetModuleHandleW(nullptr);
    ac.lpszClassName = kAboutClass;
    ac.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&ac);
    registered = true;
}

}  // namespace

void ShowAbout(HICON iconBig, HICON iconSmall) {
    if (g_aboutWnd != nullptr && IsWindow(g_aboutWnd)) {
        SetForegroundWindow(g_aboutWnd);
        return;
    }
    EnsureClassRegistered();
    int dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc = { 0, 0, MulDiv(380, dpi, 96), MulDiv(232, dpi, 96) };
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_aboutWnd = CreateWindowExW(0, kAboutClass, L"About", style, x, y, w, h,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_aboutWnd == nullptr) {
        return;
    }
    BOOL dark = g_effectiveDark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_aboutWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    SendMessageW(g_aboutWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
    SendMessageW(g_aboutWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
    ShowWindow(g_aboutWnd, SW_SHOW);
    SetForegroundWindow(g_aboutWnd);
}

void RefreshAboutTheme() {
    if (g_aboutWnd != nullptr && IsWindow(g_aboutWnd)) {
        BOOL dark = g_effectiveDark ? TRUE : FALSE;
        DwmSetWindowAttribute(g_aboutWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        InvalidateRect(g_aboutWnd, nullptr, TRUE);
    }
}

}  // namespace ui
