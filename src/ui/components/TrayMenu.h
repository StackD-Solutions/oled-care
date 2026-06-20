// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

namespace ui {

// One entry in the custom tray popup. `icon` is a POINTER to an HBITMAP because the app declares its
// menu array before the icon bitmaps are created; pass null for separators or icon-less entries.
struct MenuItem {
    UINT id;
    const wchar_t* text;
    HBITMAP* icon;
    bool isSeparator;
};

// Show the owner-drawn popup at `pt`, growing up-left from the cursor (tray style), clamped to screen.
// When an item is chosen the menu posts WM_COMMAND to `owner` with LOWORD(wParam) = the item's id, then
// closes - so the owner decides what each command does and the menu stays decoupled from those actions.
// `items` must outlive the menu (use the app's static array). Colors/fonts come from ui/Theme.h.
void ShowCustomMenu(HWND owner, POINT pt, const MenuItem* items, int count);

// Close the popup if it's currently open.
void CloseCustomMenu();

}  // namespace ui
