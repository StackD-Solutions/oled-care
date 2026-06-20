// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

// Exit code the tray app returns when it bails because another instance is already running
// (single-instance guard). The service uses this to tell "already running" apart from a
// user-requested Exit (code 0) so it stays running instead of stopping.
constexpr int kExitAlreadyRunning = 2;

// "High priority" autostart: a LocalSystem Windows service that launches OledCare.exe
// into the active user session. Invoked from WinMain based on the command line.
bool InstallService();
bool UninstallService();
int RunServiceMain();

bool IsServiceInstalled();
bool GetProtectAgainstCrashes();     // reads HKLM (any user can read)
void SetProtectAgainstCrashes(bool on);  // writes HKLM (needs admin)
