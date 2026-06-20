// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/components/TrayMenu.h"

#include <windowsx.h>
#include <dwmapi.h>

#include "ui/Theme.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")

using namespace theme;

namespace ui {

namespace {

const wchar_t* kMenuClass = L"OledCareMenuWnd";

const MenuItem* g_items = nullptr;  // the app's static menu array (must outlive the popup)
int g_count = 0;
HWND g_owner = nullptr;             // where the chosen command id is posted
HWND g_menuWnd = nullptr;
int g_menuHover = -1;

int MenuItemH(int i, int dpi) {
    return g_items[i].isSeparator ? MulDiv(9, dpi, 96) : MulDiv(34, dpi, 96);
}

int MenuItemTop(int index, int dpi) {
    int y = MulDiv(4, dpi, 96);  // top padding
    for (int i = 0; i < index; ++i) {
        y += MenuItemH(i, dpi);
    }
    return y;
}

SIZE MenuSize(int dpi) {
    int h = MulDiv(8, dpi, 96);  // top + bottom padding
    for (int i = 0; i < g_count; ++i) {
        h += MenuItemH(i, dpi);
    }
    SIZE s = { MulDiv(190, dpi, 96), h };
    return s;
}

int MenuHitTest(int y, int dpi) {
    for (int i = 0; i < g_count; ++i) {
        if (g_items[i].isSeparator) {
            continue;
        }
        int top = MenuItemTop(i, dpi);
        if (y >= top && y < top + MenuItemH(i, dpi)) {
            return i;
        }
    }
    return -1;
}

LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int dpi = GetDpiForWindow(hwnd);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(kMenuBg);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HBRUSH bd = CreateSolidBrush(kMenuBorder);
        FrameRect(dc, &rc, bd);
        DeleteObject(bd);

        int leftPad = MulDiv(14, dpi, 96);
        int gap = MulDiv(12, dpi, 96);
        HFONT oldf = static_cast<HFONT>(SelectObject(dc, g_menuFont));
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < g_count; ++i) {
            int top = MenuItemTop(i, dpi);
            int h = MenuItemH(i, dpi);
            if (g_items[i].isSeparator) {
                int mid = top + h / 2;
                RECT lr = { rc.left + MulDiv(12, dpi, 96), mid, rc.right - MulDiv(12, dpi, 96), mid + 1 };
                HBRUSH lb = CreateSolidBrush(kSeparator);
                FillRect(dc, &lr, lb);
                DeleteObject(lb);
                continue;
            }
            RECT ir = { rc.left + 1, top, rc.right - 1, top + h };
            if (i == g_menuHover) {
                HBRUSH hb = CreateSolidBrush(kMenuHover);
                FillRect(dc, &ir, hb);
                DeleteObject(hb);
            }
            // Draw the icon at its own pixel size (read from the bitmap) so the menu needs no DPI/icon
            // metric from the app - the bitmaps were already created at the right size.
            HBITMAP icon = (g_items[i].icon != nullptr) ? *g_items[i].icon : nullptr;
            int iconPx = 0;
            if (icon != nullptr) {
                BITMAP bm = {};
                GetObjectW(icon, sizeof(bm), &bm);
                iconPx = bm.bmWidth;
                HDC mdc = CreateCompatibleDC(dc);
                HBITMAP ob = static_cast<HBITMAP>(SelectObject(mdc, icon));
                int iy = top + (h - iconPx) / 2;
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(dc, ir.left + leftPad, iy, iconPx, iconPx, mdc, 0, 0, iconPx, iconPx, bf);
                SelectObject(mdc, ob);
                DeleteDC(mdc);
            }
            SetTextColor(dc, kText);
            RECT tr = ir;
            tr.left += leftPad + iconPx + gap;
            DrawTextW(dc, g_items[i].text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, oldf);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hit = MenuHitTest(GET_Y_LPARAM(lParam), dpi);
        if (hit != g_menuHover) {
            g_menuHover = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_menuHover != -1) {
            g_menuHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        int hit = MenuHitTest(GET_Y_LPARAM(lParam), dpi);
        UINT id = (hit >= 0) ? g_items[hit].id : 0;
        HWND owner = g_owner;
        CloseCustomMenu();
        // Decoupled: the owner decides what the command does (Settings / About / Quit / ...).
        if (id != 0 && owner != nullptr) {
            PostMessageW(owner, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            CloseCustomMenu();
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseCustomMenu();
        }
        return 0;
    case WM_DESTROY:
        g_menuWnd = nullptr;
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
    WNDCLASSEXW mc = {};
    mc.cbSize = sizeof(mc);
    mc.lpfnWndProc = MenuProc;
    mc.hInstance = GetModuleHandleW(nullptr);
    mc.lpszClassName = kMenuClass;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&mc);
    registered = true;
}

}  // namespace

void CloseCustomMenu() {
    if (g_menuWnd != nullptr) {
        DestroyWindow(g_menuWnd);
        g_menuWnd = nullptr;
    }
}

void ShowCustomMenu(HWND owner, POINT pt, const MenuItem* items, int count) {
    CloseCustomMenu();
    EnsureClassRegistered();
    g_items = items;
    g_count = count;
    g_owner = owner;
    g_menuHover = -1;
    int dpi = GetDpiForWindow(owner);
    SIZE sz = MenuSize(dpi);
    // Tray-style placement: grow up-left from the cursor; clamp to the screen.
    int x = pt.x - sz.cx;
    int y = pt.y - sz.cy;
    if (x < 0) {
        x = pt.x;
    }
    if (y < 0) {
        y = pt.y;
    }
    g_menuWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuClass, L"",
                                WS_POPUP, x, y, sz.cx, sz.cy, owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (g_menuWnd == nullptr) {
        return;
    }
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g_menuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    ShowWindow(g_menuWnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_menuWnd);  // so clicking away deactivates and closes it
}

}  // namespace ui
