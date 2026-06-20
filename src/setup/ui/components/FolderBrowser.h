// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#pragma once

#include <windows.h>

#include <string>

namespace ui {

// Show a folder-browse dialog (owned by `owner`, with `seed` pre-selected in the tree). Returns true
// and the chosen path in `out` if the user picked one; false if cancelled.
bool BrowseForFolder(HWND owner, const std::wstring& seed, std::wstring& out);

}  // namespace ui
