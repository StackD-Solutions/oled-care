// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "setup/ui/components/InstallerLogo.h"

#include <gdiplus.h>

#include "ui/components/AppIcon.h"  // ui::BuildSvgPath (shared SVG-path builder)

#pragma comment(lib, "gdiplus.lib")

namespace ui {

namespace {

Gdiplus::GraphicsPath* g_logoP1 = nullptr;  // lime (bottom)
Gdiplus::GraphicsPath* g_logoP2 = nullptr;  // purple (middle)
Gdiplus::GraphicsPath* g_logoP3 = nullptr;  // lime (top)
const float kLogoVbW = 1008.08f, kLogoVbH = 1052.03f;

void EnsureLogoPaths() {
    if (g_logoP1 != nullptr) {
        return;
    }
    static const char* kP1 = "M993.18,781.9l-453.03,261.53c-19.85,11.46-52.37,11.46-72.22,0L14.9,781.9c-19.85-11.46-19.85-30.22,0-41.69l144.65-83.53,308.38,178c19.85,11.46,52.37,11.46,72.22,0l308.32-178,144.7,83.53c19.85,11.46,19.85,30.22,0,41.69Z";
    static const char* kP2 = "M993.18,546.9l-453.03,261.53c-19.85,11.46-52.37,11.46-72.22,0L14.9,546.9c-19.85-11.46-19.85-30.22,0-41.69l144.7-83.53,308.32,178c19.85,11.46,52.37,11.46,72.22,0l308.32-178.05,144.7,83.58c19.85,11.46,19.85,30.22,0,41.69Z";
    static const char* kP3 = "M14.89,270.16c-19.86,11.46-19.86,30.22,0,41.69l453.05,261.57c19.86,11.46,52.35,11.46,72.2,0l453.05-261.57c19.86-11.46,19.86-30.22,0-41.69L540.14,8.6c-19.86-11.46-52.35-11.46-72.2,0L14.89,270.16Z";
    g_logoP1 = new Gdiplus::GraphicsPath();
    g_logoP2 = new Gdiplus::GraphicsPath();
    g_logoP3 = new Gdiplus::GraphicsPath();
    BuildSvgPath(g_logoP1, kP1);
    BuildSvgPath(g_logoP2, kP2);
    BuildSvgPath(g_logoP3, kP3);
}

}  // namespace

void DrawLogo(HDC dc, int lx, int ly, int lw, int lh) {
    EnsureLogoPaths();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float s = static_cast<float>(lh) / kLogoVbH;
    float drawnW = kLogoVbW * s;
    Gdiplus::Matrix mtx;
    mtx.Translate(lx + (lw - drawnW) / 2.0f, static_cast<float>(ly));
    mtx.Scale(s, s);
    g.SetTransform(&mtx);
    Gdiplus::SolidBrush lime(Gdiplus::Color(255, 175, 255, 51));
    Gdiplus::SolidBrush purple(Gdiplus::Color(255, 125, 60, 152));
    if (g_logoP1) { g.FillPath(&lime, g_logoP1); }
    if (g_logoP2) { g.FillPath(&purple, g_logoP2); }
    if (g_logoP3) { g.FillPath(&lime, g_logoP3); }
}

}  // namespace ui
