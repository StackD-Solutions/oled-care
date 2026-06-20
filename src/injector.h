// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

// Inject OledCareHook.dll into the shell process that owns the taskbar (Shell_TrayWnd). Safe to
// call repeatedly: the hook DLL's own single-load guard makes re-injection a no-op.
void InjectHook();
