// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

namespace ui {

// Owner-draw an installer push button into its DRAWITEMSTRUCT with the given label. `primary` paints
// the green accent button (Next/Install/Finish); otherwise the dark secondary style (Back/Cancel).
// Colors + font come from the installer theme (setup/ui/Theme.h).
void DrawButton(DRAWITEMSTRUCT* dis, const wchar_t* text, bool primary);

}  // namespace ui
