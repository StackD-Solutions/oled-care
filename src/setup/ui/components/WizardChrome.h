// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

namespace ui {

// Paint the installer's dark wizard chrome. Each draws from the wizard:: model (current step, option,
// install dir, layout) and the installer theme; the window proc just invalidates and calls these.
void PaintHeader(HDC dc, const RECT& cr);   // icon + product/version + logo + separator
void PaintSidebar(HDC dc, const RECT& cr);  // the step list with progress indicators
void PaintContent(HDC dc, const RECT& cr);  // the current step's title + body / option rows
void PaintFooter(HDC dc, const RECT& cr);   // the footer separator line

}  // namespace ui
