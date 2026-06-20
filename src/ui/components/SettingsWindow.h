// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

// The settings window: a custom dark-themed panel (System Tray / Taskbar / Automatic startup /
// Appearance sections) drawn and laid out by hand. It reads and writes every setting through the
// settings:: model and re-themes itself live. The tray host only needs to open it - everything else
// (the window proc, layout, control state, live theming) is internal to the component.
namespace ui {

void ShowSettings();  // open the settings window, or focus it if already open

}  // namespace ui
