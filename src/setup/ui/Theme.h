// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

// Shared look + metrics for the installer's UI components. Unlike the tray app, the installer is
// always dark, so the palette is plain constants. The fonts are created once at startup; the
// components reach them (and the DPI scale helper S) through this header.
namespace theme {

// Palette (the OledCare dark theme; green = the high-priority accent).
inline const COLORREF kBg = RGB(32, 32, 32);
inline const COLORREF kPanel = RGB(45, 45, 45);        // current-step highlight / secondary buttons
inline const COLORREF kLine = RGB(60, 60, 60);         // separators / borders
inline const COLORREF kText = RGB(243, 243, 243);
inline const COLORREF kTextDim = RGB(160, 160, 160);   // descriptions / pending steps
inline const COLORREF kGreen = RGB(175, 255, 51);      // #AFFF33 brand green
inline const COLORREF kGreenHot = RGB(150, 224, 40);   // slightly darker lime for the pressed state

// Fonts, created at startup (MakeFont) and reused across the wizard.
extern HFONT g_fBrand;  // product name
extern HFONT g_fTitle;  // content / step title
extern HFONT g_fBody;   // body + descriptions
extern HFONT g_fOpt;    // option titles (semibold)

// Current window DPI + the DIP->pixel scale helper used throughout the wizard painting.
extern int g_dpi;
inline int S(int v) { return MulDiv(v, g_dpi, 96); }

}  // namespace theme
