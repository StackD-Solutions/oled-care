// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "setup/ui/Wizard.h"

#include "setup/ui/Theme.h"  // theme::S (DIP -> pixel)

using namespace theme;

namespace wizard {

int g_step = STEP_WELCOME;
int g_option = 0;
std::wstring g_installDir;

RECT OptionRect(int index) {
    int top = kHeaderH + 64 + index * 78;
    RECT r = { S(kContentX), S(top), S(kWinW - 32), S(top + 70) };
    return r;
}

RECT LocationBoxRect() {
    RECT r = { S(kContentX), S(kLocBoxY), S(kWinW - 32) - S(kBrowseW) - S(8), S(kLocBoxY + kLocBoxH) };
    return r;
}

RECT BrowseBtnRect() {
    int right = S(kWinW - 32);
    RECT r = { right - S(kBrowseW), S(kLocBoxY), right, S(kLocBoxY + kLocBoxH) };
    return r;
}

namespace {
// Program Files\StackD Solutions
std::wstring VendorDir() {
    wchar_t pf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    return std::wstring(pf) + L"\\" + kVendorName;
}
}  // namespace

std::wstring InstallDir() {
    return VendorDir() + L"\\OledCare";
}

}  // namespace wizard
