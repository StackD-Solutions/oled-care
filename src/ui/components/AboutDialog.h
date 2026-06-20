// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

namespace ui {

// Show the "About OledCare" window (centered, single-instance). The icons are applied to the title
// bar / alt-tab; pass the app's large + small HICONs.
void ShowAbout(HICON iconBig, HICON iconSmall);

// If the About window is open, re-apply the immersive dark-mode attribute and repaint it. Call this
// when the effective theme changes so the open About window tracks it.
void RefreshAboutTheme();

}  // namespace ui
