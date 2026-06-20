// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

#include <string>

// Pure, side-effect-free settings logic, factored out of the registry accessors so it can be unit
// tested without touching the registry or any Win32 window. The settings model funnels its raw
// registry DWORDs through these; test/tests.cpp asserts them directly. Keeping the logic here (rather
// than inline in the I/O accessors) is what makes the "tripwire" tests possible.
namespace settings {

// Theme mode: 0 = system, 1 = light, 2 = dark; any out-of-range value clamps to system (0).
inline int clampThemeMode(DWORD v) {
    return static_cast<int>(v > 2 ? 0 : v);
}

// Peek auto-hide duration, clamped to a sane 1..60 seconds.
inline int clampPeekSeconds(DWORD v) {
    return static_cast<int>(v < 1 ? 1 : (v > 60 ? 60 : v));
}

// Taskbar appearance: stored DWORD 0 = Normal, anything else = Clear (OledCare's out-of-the-box look).
inline bool taskbarModeIsClear(DWORD v) {
    return v != 0;
}

// Human-readable hotkey, e.g. "Ctrl + Alt + F10". The modifier prefix is pure logic; the key name
// comes from the OS (GetKeyNameTextW), with a 0xNN hex fallback for keys it cannot name.
inline std::wstring formatHotkey(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) { s += L"Ctrl + "; }
    if (mods & MOD_ALT)     { s += L"Alt + "; }
    if (mods & MOD_SHIFT)   { s += L"Shift + "; }
    if (mods & MOD_WIN)     { s += L"Win + "; }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
    case VK_UP:     case VK_DOWN:
        sc |= 0x100;  // extended keys need the extended bit for a correct name
        break;
    default:
        break;
    }
    wchar_t name[64] = {};
    if (sc != 0 && GetKeyNameTextW(static_cast<LONG>(sc << 16), name, ARRAYSIZE(name)) > 0) {
        s += name;
    } else {
        wchar_t fb[16];
        wsprintfW(fb, L"0x%02X", vk);
        s += fb;
    }
    return s;
}

}  // namespace settings
