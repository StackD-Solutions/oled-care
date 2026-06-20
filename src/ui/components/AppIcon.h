// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <vector>

// Shared app-icon rendering, used by BOTH the tray app and the installer. The SVG path data and the
// GDI+ drawing were previously duplicated in each program's main.cpp; this is the single copy. Pure
// rendering - depends on no application state.
namespace ui {

// SVG-path builder: M/L/H/V/C/S/Z, absolute + relative, repeated argument sets, smooth-cubic
// reflection. Fills a GDI+ path in the SVG's own coordinate space. (The installer's logo uses this
// too, which is why it's part of the public surface.)
void BuildSvgPath(Gdiplus::GraphicsPath* path, const char* d);

// Draw the OledCare app icon onto an HDC, fit (preserving aspect) and centered in (lx,ly,lw,lh).
void DrawAppIcon(HDC dc, int lx, int ly, int lw, int lh);

// Create an HICON of the app icon at the given pixel size (transparent background).
HICON CreateAppIcon(int size);

// Export a multi-size .ico (PNG-compressed entries, Vista+) of the app icon to a path.
bool ExportIco(const wchar_t* path);

}  // namespace ui
