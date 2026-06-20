// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

#include <string>

#include "AppContract.h"

// The installer wizard's shared model: step enumeration + names, the dark-window layout metrics, the
// option rows, the live navigation state, and the geometry helpers. Both the wizard window
// (setup/main.cpp) and the chrome painting (WizardChrome) draw from this one place.
namespace wizard {

enum Step { STEP_WELCOME = 0, STEP_OPTIONS, STEP_INSTALL, STEP_FINISH, STEP_COUNT };
inline const wchar_t* kStepNames[STEP_COUNT] = { L"Welcome", L"Options", L"Install", L"Finish" };

// Product strings (header / brand mark).
inline const wchar_t* kProduct = contract::kProductName;
inline const wchar_t* kVendorName = contract::kVendorName;
inline const wchar_t* kVersion = L"Version " OLEDCARE_VERSION_W;

// Logical layout metrics (DIPs); scaled by the system DPI (theme::S) at use.
inline const int kWinW = 784, kWinH = 524;
inline const int kHeaderH = 96;
inline const int kSidebarW = 184;
inline const int kFooterH = 72;
inline const int kContentX = kSidebarW + 16;

// Install-location row metrics (Options step).
inline const int kLocLabelY = kHeaderH + 226;  // "Install location" caption
inline const int kLocBoxY = kHeaderH + 250;    // path box / Browse button row
inline const int kLocBoxH = 32;
inline const int kBrowseW = 92;

// The two install options shown on the Options step.
struct Option {
    const wchar_t* title;
    const wchar_t* desc;
};
inline const Option kOptions[2] = {
    { L"Standard (Recommended)",
      L"Starts OledCare automatically when you sign in." },
    { L"High priority (service)",
      L"Also installs a Windows service so OledCare starts earlier at boot, before the normal sign-in startup apps." },
};

// Live navigation state.
extern int g_step;          // current Step
extern int g_option;        // selected option (0 = Standard, 1 = High priority)
extern std::wstring g_installDir;  // chosen install directory (defaults to InstallDir())

// Geometry helpers (client coords), used for both paint + hit-testing.
RECT OptionRect(int index);   // bounding rect of an option row
RECT LocationBoxRect();       // the bordered box behind the path edit
RECT BrowseBtnRect();         // the "Browse..." button, right-aligned on the same row

// Default install directory (Program Files\StackD Solutions\OledCare). Defined in main.cpp.
std::wstring InstallDir();

}  // namespace wizard
