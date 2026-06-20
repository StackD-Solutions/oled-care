// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

namespace ui {

// Draw the StackD Solutions logo (rendered from its SVG path data), fit by height into the box
// (lx,ly,lw,lh) and horizontally centered. Installer-only brand mark.
void DrawLogo(HDC dc, int lx, int ly, int lw, int lh);

}  // namespace ui
