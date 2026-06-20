// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

// OledCareSetup - custom dark install wizard.
//
// This is the UI shell: a header, a left step-sidebar, a content area, and Cancel/Back/Next
// buttons. The actual install/uninstall plumbing is wired in later. Self-contained and dark-
// themed to match the OledCare app.

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "ui/components/AppIcon.h"
#include "AppContract.h"
#include "setup/ui/Theme.h"
#include "setup/ui/Wizard.h"
#include "setup/ui/components/DarkButton.h"
#include "setup/ui/components/InstallerLogo.h"
#include "setup/ui/components/FolderBrowser.h"
#include "setup/ui/components/WizardChrome.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace theme;   // installer palette, fonts, DPI scale (setup/ui/Theme.h)
using namespace wizard;  // step/option/installDir state, layout, geometry (setup/ui/Wizard.h)

namespace {

const wchar_t* kClass = L"OledCareSetupWnd";

// Palette + fonts + the S() DPI-scale helper now live in setup/ui/Theme.h (theme:: namespace, used above).


enum CtrlId : UINT { IDC_CANCEL = 101, IDC_BACK = 102, IDC_NEXT = 103, IDC_PATH = 104, IDC_BROWSE = 105 };

HINSTANCE g_hInst = nullptr;
HWND g_wnd = nullptr;
ULONG_PTR g_gdiToken = 0;

// Forward declarations (definitions appear later in this file).
void LayoutOptionControls();
void UpdateOptionControls();
void SyncInstallDirFromEdit();
void RestartExplorer();
void WaitForProcessExit(const wchar_t* exeName, DWORD timeoutMs);


HFONT MakeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}


// ---- button helpers -------------------------------------------------------


const wchar_t* NextLabel() {
    if (g_step == STEP_FINISH) {
        return L"Finish";
    }
    if (g_step == STEP_INSTALL) {
        return L"Install";
    }
    return L"Next";
}

void UpdateButtons() {
    HWND back = GetDlgItem(g_wnd, IDC_BACK);
    HWND next = GetDlgItem(g_wnd, IDC_NEXT);
    HWND cancel = GetDlgItem(g_wnd, IDC_CANCEL);
    EnableWindow(back, g_step != STEP_WELCOME && g_step != STEP_FINISH);
    EnableWindow(cancel, g_step != STEP_FINISH);
    SetWindowTextW(next, NextLabel());
    InvalidateRect(back, nullptr, TRUE);
    InvalidateRect(next, nullptr, TRUE);
    InvalidateRect(cancel, nullptr, TRUE);
}

void GoTo(int step) {
    if (step < 0 || step >= STEP_COUNT) {
        return;
    }
    g_step = step;
    UpdateButtons();
    UpdateOptionControls();
    InvalidateRect(g_wnd, nullptr, TRUE);
}

// ---- install / uninstall --------------------------------------------------

const wchar_t* kVendor = contract::kVendorName;
const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OledCare";
const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Embedded payload resource IDs (mirrors setup.rc).
const int IDR_APP_EXE = 100;
const int IDR_HOOK_DLL = 101;

std::wstring SelfPath() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}
std::wstring ParentOf(const std::wstring& p) {
    size_t i = p.find_last_of(L'\\');
    return (i == std::wstring::npos) ? p : p.substr(0, i);
}
// Write an embedded RCDATA resource out to a file (drops the bundled app + hook on install).
bool ExtractResourceTo(int resId, const std::wstring& outPath) {
    HRSRC res = FindResourceW(g_hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (res == nullptr) {
        return false;
    }
    HGLOBAL h = LoadResource(g_hInst, res);
    if (h == nullptr) {
        return false;
    }
    DWORD size = SizeofResource(g_hInst, res);
    const void* data = LockResource(h);
    if (data == nullptr || size == 0) {
        return false;
    }
    HANDLE f = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok != FALSE && written == size;
}
// InstallDir() (Program Files\StackD Solutions\OledCare) now lives in setup/ui/Wizard.cpp (wizard::).

// True if a file exists but is currently locked (e.g. a DLL still mapped into a running process),
// so it can't be opened for writing/replacing.
bool IsFileLocked(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;  // not present -> not locked
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_SHARING_VIOLATION;
    }
    CloseHandle(f);
    return false;
}

// A previous uninstall (with the hook DLL still loaded in Explorer) may have scheduled this
// install's files/folders for deletion on the next reboot. Remove those pending deletions so they
// can't wipe the files we're about to (re)install once the user next reboots. Only entries whose
// source path is under `folder` are dropped; everything else (Windows Update, etc.) is preserved.
void CancelPendingRebootDeletions(const std::wstring& folder) {
    const wchar_t* sm = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sm, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        return;
    }
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(k, L"PendingFileRenameOperations", nullptr, &type, nullptr, &cb) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && cb >= sizeof(wchar_t)) {
        std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, L'\0');
        DWORD cb2 = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
        if (RegQueryValueExW(k, L"PendingFileRenameOperations", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf.data()), &cb2) == ERROR_SUCCESS) {
            std::wstring needle = folder;
            if (!needle.empty()) {
                CharLowerBuffW(&needle[0], static_cast<DWORD>(needle.size()));
            }
            const size_t n = cb2 / sizeof(wchar_t);
            std::vector<wchar_t> out;
            bool changed = false;
            size_t i = 0;
            // Entries come in (source, target) pairs; an empty target means "delete source".
            while (i < n && buf[i] != L'\0') {
                std::wstring src(&buf[i]);
                i += src.size() + 1;
                std::wstring dst;
                if (i < n) {
                    dst = std::wstring(&buf[i]);
                    i += dst.size() + 1;
                }
                std::wstring srcLower = src;
                if (!srcLower.empty()) {
                    CharLowerBuffW(&srcLower[0], static_cast<DWORD>(srcLower.size()));
                }
                if (!needle.empty() && srcLower.find(needle) != std::wstring::npos) {
                    changed = true;  // drop this pending op (it targets our install)
                    continue;
                }
                out.insert(out.end(), src.begin(), src.end());
                out.push_back(L'\0');
                out.insert(out.end(), dst.begin(), dst.end());
                out.push_back(L'\0');
            }
            if (changed) {
                if (out.empty()) {
                    RegDeleteValueW(k, L"PendingFileRenameOperations");
                } else {
                    out.push_back(L'\0');  // multi-sz terminator
                    RegSetValueExW(k, L"PendingFileRenameOperations", 0, REG_MULTI_SZ,
                                   reinterpret_cast<const BYTE*>(out.data()),
                                   static_cast<DWORD>(out.size() * sizeof(wchar_t)));
                }
            }
        }
    }
    RegCloseKey(k);
}

// Run a child process (e.g. the installed app's --install-service mode) and wait for it.
void RunChildWait(const std::wstring& exe, const wchar_t* args) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Launch a process at the logged-in user's (non-elevated) integrity using explorer's token,
// so the tray app does not run elevated even though setup does.
bool LaunchUnelevated(const std::wstring& exe) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return false;
    }
    HANDLE tok = nullptr, dup = nullptr;
    bool ok = false;
    if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY |
                                   TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, &tok)) {
        if (DuplicateTokenEx(tok, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &dup)) {
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            std::wstring cmd = L"\"" + exe + L"\"";
            if (CreateProcessWithTokenW(dup, 0, exe.c_str(), &cmd[0], 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ok = true;
            }
            CloseHandle(dup);
        }
        CloseHandle(tok);
    }
    CloseHandle(proc);
    return ok;
}

void RegisterArp(const std::wstring& dir, const std::wstring& uninst) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return;
    }
    auto setS = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    auto setD = [&](const wchar_t* name, DWORD v) {
        RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    };
    setS(L"DisplayName", contract::kProductName);
    setS(L"DisplayVersion", OLEDCARE_VERSION_W);
    setS(L"Publisher", contract::kVendorName);
    setS(L"DisplayIcon", dir + L"\\OledCare.exe");
    setS(L"InstallLocation", dir);
    setS(L"UninstallString", L"\"" + uninst + L"\" --uninstall");
    setD(L"NoModify", 1);
    setD(L"NoRepair", 1);
    RegCloseKey(k);
}

// The Start Menu "Programs\OledCare" folder for the current user (COM/CSIDL resolves to the elevated
// user's profile, which matches where the HKCU Run autostart value is written).
std::wstring StartMenuFolder() {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, buf) != S_OK) {
        return L"";
    }
    return std::wstring(buf) + L"\\OledCare";
}

// Write a .lnk shortcut. COM is already initialized (WinMain). Returns false on any failure.
bool CreateShortcut(const std::wstring& lnkPath, const std::wstring& target, const std::wstring& args,
                    const std::wstring& desc, const std::wstring& workDir, const std::wstring& iconPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&link))) ||
        link == nullptr) {
        return false;
    }
    link->SetPath(target.c_str());
    if (!args.empty()) {
        link->SetArguments(args.c_str());
    }
    if (!desc.empty()) {
        link->SetDescription(desc.c_str());
    }
    if (!workDir.empty()) {
        link->SetWorkingDirectory(workDir.c_str());
    }
    if (!iconPath.empty()) {
        link->SetIconLocation(iconPath.c_str(), 0);
    }
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) &&
        file != nullptr) {
        ok = SUCCEEDED(file->Save(lnkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// Create the Start Menu folder with a launch shortcut + an uninstall shortcut. Both use OledCare.exe's
// embedded icon. Best-effort: a missing Start Menu doesn't fail the install.
void CreateStartMenuShortcuts(const std::wstring& dir, const std::wstring& appExe, const std::wstring& uninst) {
    std::wstring folder = StartMenuFolder();
    if (folder.empty()) {
        return;
    }
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    CreateShortcut(folder + L"\\OledCare.lnk", appExe, L"", L"OledCare", dir, appExe);
    CreateShortcut(folder + L"\\Uninstall OledCare.lnk", uninst, L"--uninstall", L"Uninstall OledCare", dir, appExe);
}

// Remove the Start Menu shortcuts + their (now-empty) folder.
void RemoveStartMenuShortcuts() {
    std::wstring folder = StartMenuFolder();
    if (folder.empty()) {
        return;
    }
    DeleteFileW((folder + L"\\OledCare.lnk").c_str());
    DeleteFileW((folder + L"\\Uninstall OledCare.lnk").c_str());
    RemoveDirectoryW(folder.c_str());
}

// Copy the binaries into Program Files, register Add/Remove Programs, set up autostart, and
// start the app. option: 0 = standard (Run key), 1 = high priority (Windows service).
bool RunInstall(int option) {
    std::wstring dir = g_installDir.empty() ? InstallDir() : g_installDir;
    // Create the full directory chain (the chosen location may be on any drive/parent).
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    // A prior uninstall (with the hook DLL still loaded in Explorer) may have scheduled this
    // folder for deletion on the next reboot. Cancel that first so it can't wipe what we install.
    CancelPendingRebootDeletions(ParentOf(dir));

    std::wstring appExe = dir + L"\\OledCare.exe";

    // Tear down any previous instance FIRST (this mirrors the uninstall teardown). Without it the
    // old tray app keeps the single-instance mutex, so the freshly-launched app exits immediately
    // (kExitAlreadyRunning) and the new hook never gets injected - i.e. "installed but nothing
    // works". Stopping the service also terminates the app it launched; then we close any straggler
    // and wait for OledCare.exe to fully exit so the .exe unlocks for the overwrite below.
    if (GetFileAttributesW(appExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RunChildWait(appExe, L"--uninstall-service");
    }
    HWND oldWnd = FindWindowW(L"OledCareHiddenWnd", nullptr);
    if (oldWnd != nullptr) {
        SendMessageW(oldWnd, WM_CLOSE, 0, 0);
    }
    WaitForProcessExit(L"OledCare.exe", 1500);

    // If a previous OledCareHook.dll is still loaded in Explorer (re-install without a reboot) it's
    // locked and can't be overwritten. Ask the old hook to self-unload (no Explorer restart); only
    // if that doesn't free it within ~3s (e.g. a very old build that can't self-unload) fall back to
    // refreshing Explorer. Fresh installs skip all of this (nothing is locked) and stay seamless.
    if (IsFileLocked(dir + L"\\OledCareHook.dll")) {
        HANDLE ue = OpenEventW(EVENT_MODIFY_STATE, FALSE, contract::kEventUnload);
        if (ue != nullptr) {
            SetEvent(ue);
            CloseHandle(ue);
        }
        bool freed = false;
        for (int i = 0; i < 60; ++i) {
            if (!IsFileLocked(dir + L"\\OledCareHook.dll")) {
                freed = true;
                break;
            }
            Sleep(50);
        }
        if (!freed) {
            RestartExplorer();  // fallback: old hook couldn't self-unload
            for (int i = 0; i < 60 && FindWindowW(L"Shell_TrayWnd", nullptr) == nullptr; ++i) {
                Sleep(100);
            }
        }
    }

    // Extract the bundled binaries from our own resources (self-contained installer).
    if (!ExtractResourceTo(IDR_APP_EXE, dir + L"\\OledCare.exe")) {
        return false;
    }
    if (!ExtractResourceTo(IDR_HOOK_DLL, dir + L"\\OledCareHook.dll")) {
        return false;
    }

    // The uninstaller is this setup exe itself (it supports --uninstall).
    std::wstring uninst = dir + L"\\OledCareUninstall.exe";
    CopyFileW(SelfPath().c_str(), uninst.c_str(), FALSE);

    RegisterArp(dir, uninst);
    CreateStartMenuShortcuts(dir, appExe, uninst);

    if (option == 1) {
        RunChildWait(appExe, L"--install-service");  // installs + starts the service (it launches the app)
    } else {
        HKEY rk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &rk) == ERROR_SUCCESS) {
            std::wstring q = L"\"" + appExe + L"\"";
            RegSetValueExW(rk, L"OledCare", 0, REG_SZ, reinterpret_cast<const BYTE*>(q.c_str()),
                           static_cast<DWORD>((q.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(rk);
        }
        LaunchUnelevated(appExe);
    }
    // No Explorer restart: the app injects the hook into the running shell live (it targets the
    // process that owns Shell_TrayWnd, and the hook's diagnostics connection attaches to the
    // already-built taskbar). Restarting Explorer here would make the taskbar vanish for several
    // seconds and look like a crash, for no benefit.
    return true;
}

// Read the install directory recorded at install time from Add/Remove Programs.
std::wstring ReadInstallLocation() {
    std::wstring result;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {};
        DWORD cb = sizeof(buf), type = 0;
        if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS && type == REG_SZ) {
            result = buf;
        }
        RegCloseKey(k);
    }
    return result;
}

// Wait for every running process named exeName to exit (up to timeoutMs each); force-kill any
// that don't exit gracefully, so a lingering app can't re-inject the hook when Explorer restarts.
void WaitForProcessExit(const wchar_t* exeName, DWORD timeoutMs) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h != nullptr) {
                    if (WaitForSingleObject(h, timeoutMs) != WAIT_OBJECT_0) {
                        TerminateProcess(h, 0);  // graceful close didn't take - force it
                    }
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Terminate Explorer so the injected hook DLL is unloaded; Windows auto-restarts the shell
// (AutoRestartShell), so the tray comes back clean and the DLL is no longer locked.
void RestartExplorer() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W pe = { sizeof(pe) };
    std::vector<HANDLE> procs;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h != nullptr) {
                    TerminateProcess(h, 0);
                    procs.push_back(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    // Wait for ALL explorer processes to finish exiting AT ONCE (so the hook DLL unmaps and the
    // file frees up), capped at ~2s total. After a forced TerminateProcess they die almost
    // immediately - this is normally a fraction of a second, not a long freeze. Windows
    // (AutoRestartShell) relaunches the shell in parallel, so the taskbar is back within ~1-2s.
    if (!procs.empty()) {
        WaitForMultipleObjects(static_cast<DWORD>(procs.size()), procs.data(), TRUE, 2000);
        for (HANDLE h : procs) {
            CloseHandle(h);
        }
    }
}

// Delete this still-running uninstaller and the (now-empty) install + vendor folders via a
// detached cmd that first waits for us to exit. A process can't delete its own exe directly.
void SelfDeleteAndRemoveDirs(const std::wstring& dir) {
    std::wstring cmd = L"cmd.exe /C ping 127.0.0.1 -n 3 >nul & del /F /Q \"" + SelfPath() +
                       L"\" & rmdir /Q \"" + dir + L"\" & rmdir /Q \"" + ParentOf(dir) + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int RunUninstall() {
    if (MessageBoxW(nullptr, L"Remove OledCare from your computer?", L"Uninstall OledCare",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 0;
    }
    std::wstring dir = ReadInstallLocation();
    if (dir.empty()) {
        dir = InstallDir();
    }
    std::wstring appExe = dir + L"\\OledCare.exe";

    // Remove the service (stopping it also terminates the app it launched).
    if (GetFileAttributesW(appExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RunChildWait(appExe, L"--uninstall-service");
    }
    // Close the running tray instance, then wait for it to exit so it can't re-inject the hook.
    HWND w = FindWindowW(L"OledCareHiddenWnd", nullptr);
    if (w != nullptr) {
        SendMessageW(w, WM_CLOSE, 0, 0);
    }
    WaitForProcessExit(L"OledCare.exe", 1500);

    // Tell the injected hook to cleanly unload ITSELF from Explorer: it restores the tray + taskbar,
    // detaches from the visual tree, and FreeLibrary's its own module - so the DLL file frees with
    // NO Explorer restart (no taskbar blink, no shell respawn wait). The file unlocks in ~200ms.
    HANDLE unloadEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, contract::kEventUnload);
    if (unloadEvent != nullptr) {
        SetEvent(unloadEvent);
        CloseHandle(unloadEvent);
    }

    // Delete everything. Poll the hook DLL until it unlocks (event-driven: exits the instant the
    // hook has unloaded it, ~200ms), then delete. Reboot-delete is an absolute last resort only if
    // the hook wasn't loaded to receive the signal.
    DeleteFileW(appExe.c_str());
    std::wstring hook = dir + L"\\OledCareHook.dll";
    bool hookRemoved = false;
    for (int i = 0; i < 60; ++i) {
        if (DeleteFileW(hook.c_str()) || GetFileAttributesW(hook.c_str()) == INVALID_FILE_ATTRIBUTES) {
            hookRemoved = true;
            break;
        }
        Sleep(50);
    }
    if (!hookRemoved) {
        MoveFileExW(hook.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    // Registry cleanup: Add/Remove entry, autostart value, and settings.
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, KEY_WOW64_64KEY, 0);
    HKEY rk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &rk) == ERROR_SUCCESS) {
        RegDeleteValueW(rk, L"OledCare");
        RegCloseKey(rk);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\OledCare");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\OledCare");
    RemoveStartMenuShortcuts();

    MessageBoxW(nullptr, L"OledCare has been uninstalled.", L"Uninstall OledCare",
                MB_OK | MB_ICONINFORMATION);

    // Last: delete this running uninstaller and the now-empty install + vendor folders. Runs
    // after we exit (a process can't delete its own exe), so it must be the final step.
    SelfDeleteAndRemoveDirs(dir);
    return 0;
}


// ---- window proc ----------------------------------------------------------

HWND MakeButton(HWND parent, UINT id) {
    return CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), g_hInst, nullptr);
}

void LayoutButtons() {
    RECT cr;
    GetClientRect(g_wnd, &cr);
    int bw = S(110), bh = S(34), gap = S(10);
    int y = cr.bottom - S(kFooterH) + (S(kFooterH) - bh) / 2;
    int rx = cr.right - S(24) - bw;
    SetWindowPos(GetDlgItem(g_wnd, IDC_NEXT), nullptr, rx, y, bw, bh, SWP_NOZORDER);
    rx -= bw + gap;
    SetWindowPos(GetDlgItem(g_wnd, IDC_BACK), nullptr, rx, y, bw, bh, SWP_NOZORDER);
    rx -= bw + gap;
    SetWindowPos(GetDlgItem(g_wnd, IDC_CANCEL), nullptr, rx, y, bw, bh, SWP_NOZORDER);
}

// Position the path edit (inside its bordered box) and the Browse button.
void LayoutOptionControls() {
    RECT box = LocationBoxRect();
    RECT btn = BrowseBtnRect();
    int inset = S(8), editH = S(18);
    SetWindowPos(GetDlgItem(g_wnd, IDC_PATH), nullptr,
                 box.left + inset, box.top + ((box.bottom - box.top) - editH) / 2,
                 (box.right - box.left) - inset * 2, editH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_wnd, IDC_BROWSE), nullptr,
                 btn.left, btn.top, btn.right - btn.left, btn.bottom - btn.top, SWP_NOZORDER);
}

// The path field + Browse button are only shown on the Options step.
void UpdateOptionControls() {
    int show = (g_step == STEP_OPTIONS) ? SW_SHOW : SW_HIDE;
    ShowWindow(GetDlgItem(g_wnd, IDC_PATH), show);
    ShowWindow(GetDlgItem(g_wnd, IDC_BROWSE), show);
}

// Pull the (possibly hand-edited) path back into g_installDir, trimming trailing slashes.
void SyncInstallDirFromEdit() {
    HWND e = GetDlgItem(g_wnd, IDC_PATH);
    if (e == nullptr) {
        return;
    }
    wchar_t buf[MAX_PATH] = {};
    GetWindowTextW(e, buf, MAX_PATH);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\\')) {
        s.pop_back();
    }
    if (!s.empty()) {
        g_installDir = s;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        MakeButton(hwnd, IDC_CANCEL);
        MakeButton(hwnd, IDC_BACK);
        MakeButton(hwnd, IDC_NEXT);
        SetWindowTextW(GetDlgItem(hwnd, IDC_CANCEL), L"Cancel");
        SetWindowTextW(GetDlgItem(hwnd, IDC_BACK), L"Back");
        SetWindowTextW(GetDlgItem(hwnd, IDC_NEXT), L"Next");
        HWND ed = CreateWindowExW(0, L"EDIT", g_installDir.c_str(),
                                  WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                                  0, 0, 0, 0, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PATH)), g_hInst, nullptr);
        SendMessageW(ed, WM_SETFONT, reinterpret_cast<WPARAM>(g_fBody), TRUE);
        MakeButton(hwnd, IDC_BROWSE);
        SetWindowTextW(GetDlgItem(hwnd, IDC_BROWSE), L"Browse...");
        return 0;
    }
    case WM_SIZE:
        LayoutButtons();
        LayoutOptionControls();
        UpdateButtons();
        UpdateOptionControls();
        return 0;
    case WM_LBUTTONDOWN:
        if (g_step == STEP_OPTIONS) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            for (int i = 0; i < 2; ++i) {
                RECT r = OptionRect(i);
                if (PtInRect(&r, pt)) {
                    g_option = i;
                    InvalidateRect(hwnd, nullptr, TRUE);
                    break;
                }
            }
        }
        return 0;
    case WM_DRAWITEM: {
        auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        wchar_t text[32] = {};
        GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
        ui::DrawButton(dis, text, dis->CtlID == IDC_NEXT);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case IDC_CANCEL:
                DestroyWindow(hwnd);
                break;
            case IDC_BACK:
                if (g_step == STEP_OPTIONS) {
                    SyncInstallDirFromEdit();
                }
                GoTo(g_step - 1);
                break;
            case IDC_BROWSE: {
                std::wstring picked;
                if (ui::BrowseForFolder(g_wnd, g_installDir, picked)) {
                    // Guarantee a clean product folder: append \OledCare unless already chosen.
                    size_t slash = picked.find_last_of(L'\\');
                    std::wstring tail = (slash == std::wstring::npos) ? picked : picked.substr(slash + 1);
                    if (_wcsicmp(tail.c_str(), L"OledCare") != 0) {
                        if (!picked.empty() && picked.back() != L'\\') {
                            picked += L"\\";
                        }
                        picked += L"OledCare";
                    }
                    g_installDir = picked;
                    SetWindowTextW(GetDlgItem(hwnd, IDC_PATH), g_installDir.c_str());
                }
                break;
            }
            case IDC_NEXT:
                if (g_step == STEP_FINISH) {
                    DestroyWindow(hwnd);
                } else if (g_step == STEP_INSTALL) {
                    SyncInstallDirFromEdit();
                    if (RunInstall(g_option)) {
                        GoTo(STEP_FINISH);
                    } else {
                        MessageBoxW(hwnd, L"Installation failed. Please run setup again.",
                                    L"OledCare Setup", MB_OK | MB_ICONERROR);
                    }
                } else {
                    if (g_step == STEP_OPTIONS) {
                        SyncInstallDirFromEdit();
                    }
                    GoTo(g_step + 1);
                }
                break;
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;  // painted in WM_PAINT
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        // double buffer to avoid flicker
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(mem, &cr, bg);
        DeleteObject(bg);
        ui::PaintHeader(mem, cr);
        ui::PaintSidebar(mem, cr);
        ui::PaintContent(mem, cr);
        ui::PaintFooter(mem, cr);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH editBrush = nullptr;
        if (editBrush == nullptr) {
            editBrush = CreateSolidBrush(kPanel);
        }
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<LRESULT>(editBrush);
    }
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool uninstall = false;
        if (argv != nullptr) {
            for (int i = 1; i < argc; ++i) {
                if (wcscmp(argv[i], L"--uninstall") == 0) { uninstall = true; }
            }
            LocalFree(argv);
        }
        if (uninstall) {
            return RunUninstall();
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // BIF_NEWDIALOGSTYLE folder picker needs COM

    Gdiplus::GdiplusStartupInput gdiInput;
    Gdiplus::GdiplusStartup(&g_gdiToken, &gdiInput, nullptr);

    g_hInst = hInstance;
    g_dpi = GetDpiForSystem();
    g_installDir = InstallDir();  // default; user can change it on the Options page
    g_fBrand = MakeFont(19, FW_SEMIBOLD);
    g_fTitle = MakeFont(15, FW_SEMIBOLD);
    g_fBody = MakeFont(10, FW_NORMAL);
    g_fOpt = MakeFont(11, FW_SEMIBOLD);

    // Caption/taskbar icon, loaded from our own embedded icon (RT_GROUP_ICON id 1).
    HICON titleIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    HICON titleIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = titleIcon;
    wc.hIconSm = titleIconSm;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, S(kWinW), S(kWinH) };
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, g_dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_wnd = CreateWindowExW(0, kClass, L"OledCare Setup", style, x, y, w, h,
                            nullptr, nullptr, hInstance, nullptr);
    if (g_wnd == nullptr) {
        return 1;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // DWM dark captions don't reliably pick up the class icon; set it per-window too.
    SendMessageW(g_wnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(titleIconSm));
    SendMessageW(g_wnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(titleIcon));
    LayoutButtons();
    LayoutOptionControls();
    UpdateButtons();
    UpdateOptionControls();
    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (g_gdiToken != 0) {
        Gdiplus::GdiplusShutdown(g_gdiToken);
    }
    CoUninitialize();
    return 0;
}
