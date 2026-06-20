// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "setup/ui/Theme.h"

// Definitions for the installer's shared fonts + DPI. Created at startup by main.cpp (MakeFont sets
// the fonts, the window's DPI sets g_dpi); kept in their own translation unit so every UI component
// links one shared copy.
namespace theme {

HFONT g_fBrand = nullptr;
HFONT g_fTitle = nullptr;
HFONT g_fBody = nullptr;
HFONT g_fOpt = nullptr;

int g_dpi = 96;

}  // namespace theme
