// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/components/StyledCombo.h"

#include <commctrl.h>
#include <uxtheme.h>

#include "ui/Theme.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

using namespace theme;

namespace ui {

namespace {

// Custom-painted closed appearance for the dropdowns: a rounded, button-like fill with centered
// selection text and a solid down-arrow. The native dropdown list (themed dark/light) is left intact.
LRESULT CALLBACK ComboSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        int dpi = GetDpiForWindow(hwnd);
        auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_darkBrush);  // blend the rounded corners into the window background
        COLORREF fill = g_effectiveDark ? RGB(45, 45, 45) : RGB(251, 251, 251);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, kMenuBorder);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, pen);
        RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, S(6), S(6));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
        wchar_t text[80] = {};
        int sel = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
        if (sel >= 0) {
            SendMessageW(hwnd, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text));
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        HFONT of = static_cast<HFONT>(SelectObject(dc, g_uiFont));
        RECT tr = rc;
        tr.left += S(12);
        tr.right -= S(28);
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
        int ax = rc.right - S(16);
        int ay = (rc.top + rc.bottom) / 2;
        int aw = S(4), ah = S(3);
        POINT tri[3] = { { ax - aw, ay - ah }, { ax + aw, ay - ah }, { ax, ay + ah } };
        HBRUSH abr = CreateSolidBrush(kText);
        HPEN apen = CreatePen(PS_SOLID, 1, kText);
        HGDIOBJ oab = SelectObject(dc, abr);
        HGDIOBJ oap = SelectObject(dc, apen);
        Polygon(dc, tri, 3);
        SelectObject(dc, oab);
        SelectObject(dc, oap);
        DeleteObject(abr);
        DeleteObject(apen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ComboSubclass, 0);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

}  // namespace

HWND CreateStyledCombo(HWND parent, UINT id, int xDip, int rowCenter, int dropHeight) {
    const int dpi = GetDpiForWindow(parent);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    HWND combo = CreateWindowW(L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                               S(xDip), S(rowCenter - 14), S(150), S(dropHeight), parent,
                               reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    SetWindowTheme(combo, g_effectiveDark ? L"DarkMode_CFD" : L"CFD", nullptr);
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(22));
    SetWindowSubclass(combo, ComboSubclass, 0, 0);
    return combo;
}

LRESULT SelectedComboData(HWND combo) {
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    return (sel == CB_ERR) ? CB_ERR : SendMessageW(combo, CB_GETITEMDATA, sel, 0);
}

}  // namespace ui
