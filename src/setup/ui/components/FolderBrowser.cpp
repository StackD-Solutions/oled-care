// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "setup/ui/components/FolderBrowser.h"

#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace ui {

namespace {

int CALLBACK BrowseCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data != 0) {
        // Pre-select the seeded path in the tree.
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}

}  // namespace

bool BrowseForFolder(HWND owner, const std::wstring& seed, std::wstring& out) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose the folder to install OledCare into";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    bi.lpfn = BrowseCallback;
    bi.lParam = reinterpret_cast<LPARAM>(seed.c_str());
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    ILFree(pidl);
    if (ok) {
        out = path;
    }
    return ok;
}

}  // namespace ui
