// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

namespace ui {

// Create a dark-styled dropdown: a custom-painted closed field (rounded, centered text, solid arrow)
// over a native themed list. Placed at (xDip, rowCenter-14), sized 150 x dropHeight (DIPs, scaled by
// the parent's DPI). Colors/font come from ui/Theme.h. Callers populate it afterwards.
HWND CreateStyledCombo(HWND parent, UINT id, int xDip, int rowCenter, int dropHeight);

// The selected item's CB_GETITEMDATA value, or CB_ERR when nothing is selected.
LRESULT SelectedComboData(HWND combo);

}  // namespace ui
