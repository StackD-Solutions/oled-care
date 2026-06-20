// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

// Screen Dimming: an idle-driven dimmer for OLED burn-in protection. A topmost, click-through
// black overlay fades in once the user has been idle for the configured time and fades out on any
// mouse/keyboard input - so static, bright pixels aren't held at full luminance while you're away.
// Dimming is skipped while the amount is Off, or while a fullscreen game / video / presentation is on
// screen (so it never blacks out a movie).
namespace ui {

// Create the overlay + start the idle/fade timer on the calling thread's message loop. Call once at
// startup. It reads its Dim-amount / Dim-after settings live from the settings model, so changes made
// in the Settings window take effect within ~1s with no restart.
void StartScreenDimmer(HINSTANCE hInst);

}  // namespace ui
