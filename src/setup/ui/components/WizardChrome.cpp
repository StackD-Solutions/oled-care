// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "setup/ui/components/WizardChrome.h"

#include <gdiplus.h>

#include <string>

#include "setup/ui/Theme.h"
#include "setup/ui/Wizard.h"
#include "ui/components/AppIcon.h"
#include "setup/ui/components/InstallerLogo.h"

#pragma comment(lib, "gdiplus.lib")

using namespace theme;
using namespace wizard;

namespace ui {

namespace {

void PaintStepIndicator(HDC dc, int step, int cx, int cy) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::Color green(255, GetRValue(kGreen), GetGValue(kGreen), GetBValue(kGreen));
    if (step < g_step) {
        // done: green check
        Gdiplus::Pen pen(green, static_cast<Gdiplus::REAL>(S(2)));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::Point p[3] = { { cx - S(5), cy }, { cx - S(1), cy + S(4) }, { cx + S(6), cy - S(5) } };
        g.DrawLines(&pen, p, 3);
    } else if (step == g_step) {
        // current: green right-arrow
        Gdiplus::SolidBrush br(green);
        Gdiplus::Point p[3] = { { cx - S(4), cy - S(5) }, { cx + S(5), cy }, { cx - S(4), cy + S(5) } };
        g.FillPolygon(&br, p, 3);
    } else {
        // pending: dim dot
        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(kTextDim), GetGValue(kTextDim), GetBValue(kTextDim)));
        int dr = S(3);
        g.FillEllipse(&br, cx - dr, cy - dr, dr * 2, dr * 2);
    }
}

}  // namespace

void PaintHeader(HDC dc, const RECT& cr) {
    // app icon + product name + version (left)
    SetBkMode(dc, TRANSPARENT);
    ui::DrawAppIcon(dc, S(28), S(24), S(48), S(48));
    int tx = S(86);
    HFONT of = static_cast<HFONT>(SelectObject(dc, g_fBrand));
    SetTextColor(dc, kText);
    TextOutW(dc, tx, S(22), kProduct, lstrlenW(kProduct));
    SelectObject(dc, g_fBody);
    SetTextColor(dc, kTextDim);
    TextOutW(dc, tx, S(56), kVersion, lstrlenW(kVersion));

    // brand mark: the logo + vendor name on the right
    int lx = cr.right - S(212);
    ui::DrawLogo(dc, lx, S(26), S(38), S(40));
    SetTextColor(dc, kText);
    SelectObject(dc, g_fOpt);
    TextOutW(dc, lx + S(48), S(38), kVendorName, lstrlenW(kVendorName));
    SelectObject(dc, of);

    // separator under the header
    RECT ln = { 0, S(kHeaderH), cr.right, S(kHeaderH) + 1 };
    HBRUSH lb = CreateSolidBrush(kLine);
    FillRect(dc, &ln, lb);
    DeleteObject(lb);
}

void PaintSidebar(HDC dc, const RECT& cr) {
    int rowH = 44, top = kHeaderH + 28;
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < STEP_COUNT; ++i) {
        int y = top + i * rowH;
        if (i == g_step) {
            RECT hl = { 0, S(y - 8), S(kSidebarW), S(y + rowH - 16) };
            HBRUSH hb = CreateSolidBrush(kPanel);
            FillRect(dc, &hl, hb);
            DeleteObject(hb);
        }
        PaintStepIndicator(dc, i, S(26), S(y + 8));
        SelectObject(dc, g_fBody);
        SetTextColor(dc, (i == g_step) ? kText : (i < g_step ? kText : kTextDim));
        TextOutW(dc, S(48), S(y), kStepNames[i], lstrlenW(kStepNames[i]));
    }
}

void PaintContent(HDC dc, const RECT& cr) {
    SetBkMode(dc, TRANSPARENT);
    int x = S(kContentX), y = S(kHeaderH + 18);
    const wchar_t* title = L"";
    const wchar_t* body = L"";
    switch (g_step) {
    case STEP_WELCOME:
        title = L"Welcome";
        body = L"This will install OledCare on your computer.\n\n"
               L"OledCare is a suite of features designed to prevent screen burn-in and maintain "
               L"panel longevity on OLED displays.\n\nClick Next to continue.";
        break;
    case STEP_OPTIONS:
        title = L"Installation options";
        break;
    case STEP_INSTALL:
        title = L"Ready to install";
        body = L"OledCare is ready to install. Click Install to copy the files to your computer "
               L"and configure it to run.";
        break;
    case STEP_FINISH:
        title = L"Finished";
        body = L"OledCare has been installed and is now running.";
        break;
    }
    SelectObject(dc, g_fTitle);
    SetTextColor(dc, kText);
    TextOutW(dc, x, y, title, lstrlenW(title));

    if (g_step == STEP_OPTIONS) {
        for (int i = 0; i < 2; ++i) {
            RECT r = OptionRect(i);
            bool sel = (g_option == i);
            // radio circle (anti-aliased via GDI+)
            int rcx = r.left + S(10), rcy = r.top + S(10), rad = S(9);
            COLORREF ringCol = sel ? kGreen : kTextDim;
            {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen ring(Gdiplus::Color(255, GetRValue(ringCol), GetGValue(ringCol), GetBValue(ringCol)),
                                  static_cast<Gdiplus::REAL>(S(2)));
                g.DrawEllipse(&ring, rcx - rad, rcy - rad, rad * 2, rad * 2);
                if (sel) {
                    Gdiplus::SolidBrush dot(Gdiplus::Color(255, GetRValue(kGreen), GetGValue(kGreen), GetBValue(kGreen)));
                    int dr = S(4);
                    g.FillEllipse(&dot, rcx - dr, rcy - dr, dr * 2, dr * 2);
                }
            }
            int tx = r.left + S(34);
            SelectObject(dc, g_fOpt);
            SetTextColor(dc, kText);
            TextOutW(dc, tx, r.top, kOptions[i].title, lstrlenW(kOptions[i].title));
            SelectObject(dc, g_fBody);
            SetTextColor(dc, kTextDim);
            RECT dr = { tx, r.top + S(24), r.right, r.bottom };
            DrawTextW(dc, kOptions[i].desc, -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        // install-location caption + bordered box (the edit control sits on top)
        SelectObject(dc, g_fOpt);
        SetTextColor(dc, kText);
        const wchar_t* locCap = L"Install location";
        TextOutW(dc, S(kContentX), S(kLocLabelY), locCap, lstrlenW(locCap));
        {
            RECT box = LocationBoxRect();
            HBRUSH pf = CreateSolidBrush(kPanel);
            HPEN pp = CreatePen(PS_SOLID, 1, kLine);
            HGDIOBJ ob2 = SelectObject(dc, pf);
            HGDIOBJ op2 = SelectObject(dc, pp);
            RoundRect(dc, box.left, box.top, box.right, box.bottom, S(4), S(4));
            SelectObject(dc, ob2);
            SelectObject(dc, op2);
            DeleteObject(pf);
            DeleteObject(pp);
        }
        // note line
        SelectObject(dc, g_fBody);
        SetTextColor(dc, kTextDim);
        RECT nr = { x, S(kWinH - kFooterH - 34), S(kWinW - 32), S(kWinH - kFooterH - 12) };
        DrawTextW(dc, L"Note: OledCare injects a small helper into Explorer to apply its effects.",
                  -1, &nr, DT_LEFT | DT_SINGLELINE);
    } else {
        SelectObject(dc, g_fBody);
        SetTextColor(dc, kTextDim);
        RECT br = { x, y + S(36), S(kWinW - 32), S(kWinH - kFooterH - 16) };
        DrawTextW(dc, body, -1, &br, DT_LEFT | DT_TOP | DT_WORDBREAK);
        if (g_step == STEP_INSTALL) {
            std::wstring loc = L"Location:   " + (g_installDir.empty() ? InstallDir() : g_installDir);
            SetTextColor(dc, kText);
            RECT lr = { x, y + S(96), S(kWinW - 32), y + S(140) };
            DrawTextW(dc, loc.c_str(), -1, &lr, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
}

void PaintFooter(HDC dc, const RECT& cr) {
    RECT ln = { 0, cr.bottom - S(kFooterH), cr.right, cr.bottom - S(kFooterH) + 1 };
    HBRUSH lb = CreateSolidBrush(kLine);
    FillRect(dc, &ln, lb);
    DeleteObject(lb);
}

}  // namespace ui
