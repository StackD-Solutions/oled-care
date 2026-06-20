// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

// Unit tests for the pure settings logic (the "tripwire"). These assert the load-bearing value
// transforms - the clamps and the hotkey formatting - without any registry or window I/O, so they run
// instantly in CI. Run via: ctest --test-dir build -C Release  (or run the exe directly).

#include "TestMain.h"

#include "ui/SettingsLogic.h"

using namespace settings;

int main() {
    // clampThemeMode: 0/1/2 pass through; anything else clamps to System (0).
    CHECK(clampThemeMode(0) == 0);
    CHECK(clampThemeMode(1) == 1);
    CHECK(clampThemeMode(2) == 2);
    CHECK(clampThemeMode(3) == 0);
    CHECK(clampThemeMode(255) == 0);

    // clampPeekSeconds: clamp to 1..60.
    CHECK(clampPeekSeconds(0) == 1);
    CHECK(clampPeekSeconds(1) == 1);
    CHECK(clampPeekSeconds(3) == 3);
    CHECK(clampPeekSeconds(60) == 60);
    CHECK(clampPeekSeconds(61) == 60);
    CHECK(clampPeekSeconds(100000) == 60);

    // taskbarModeIsClear: 0 -> Normal (false); anything else -> Clear (true).
    CHECK(!taskbarModeIsClear(0));
    CHECK(taskbarModeIsClear(1));
    CHECK(taskbarModeIsClear(2));

    // clampDimAmount: 0 = Off; valid percents pass; capped at 90 (never fully black).
    CHECK(clampDimAmount(0) == 0);
    CHECK(clampDimAmount(50) == 50);
    CHECK(clampDimAmount(90) == 90);
    CHECK(clampDimAmount(100) == 90);
    CHECK(clampDimAmount(255) == 90);

    // clampDimAfterSeconds: clamp to 10..3600.
    CHECK(clampDimAfterSeconds(0) == 10);
    CHECK(clampDimAfterSeconds(10) == 10);
    CHECK(clampDimAfterSeconds(300) == 300);
    CHECK(clampDimAfterSeconds(3600) == 3600);
    CHECK(clampDimAfterSeconds(99999) == 3600);

    // formatHotkey: the modifier prefix is the pure logic worth pinning (the key name is the OS's).
    CHECK(formatHotkey(MOD_CONTROL | MOD_ALT, VK_F10).rfind(L"Ctrl + Alt + ", 0) == 0);
    CHECK(formatHotkey(MOD_CONTROL, 'A').rfind(L"Ctrl + ", 0) == 0);
    CHECK(formatHotkey(MOD_SHIFT, VK_F1).rfind(L"Shift + ", 0) == 0);
    CHECK(formatHotkey(MOD_WIN, VK_SPACE).rfind(L"Win + ", 0) == 0);
    CHECK(formatHotkey(MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, VK_DELETE)
              .rfind(L"Ctrl + Alt + Shift + Win + ", 0) == 0);
    // No modifiers -> no " + " separator at all (just the key name).
    CHECK(formatHotkey(0, VK_F5).find(L" + ") == std::wstring::npos);
    // A key the OS cannot name (vk 0) falls back to a 0xNN hex token.
    CHECK(formatHotkey(0, 0) == std::wstring(L"0x00"));

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
