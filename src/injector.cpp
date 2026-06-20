// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

// injector.cpp - loads OledCareHook.dll into explorer.exe the clean way: a thread-scoped
// WH_CALLWNDPROC hook on the taskbar's UI thread. Windows maps the DLL into explorer the next time
// that thread dispatches a window message - which cannot happen until that thread's message loop
// (and its XAML island) is alive. That timing is what lets the hook's XAML-diagnostics connection
// succeed on a cold boot; the older CreateRemoteThread injection fired on a fresh thread that could
// run before the taskbar's XAML existed, so InitializeXamlDiagnosticsEx returned ERROR_NOT_FOUND and
// never recovered.

#include "injector.h"

#include <windows.h>
#include <string>

namespace {

// Full path to OledCareHook.dll, which sits next to OledCare.exe.
std::wstring HookDllPath() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p(exe);
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return L"OledCareHook.dll";
    }
    return p.substr(0, slash + 1) + L"OledCareHook.dll";
}

}  // namespace

void InjectHook() {
    // Target the taskbar's owning UI thread, not the process generically - the hook must ride that
    // specific thread's message pump so the DLL loads while its XAML island is alive.
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) {
        return;  // taskbar not up yet - the caller (startup poll / TaskbarCreated) retries
    }
    DWORD tid = GetWindowThreadProcessId(tray, nullptr);
    if (tid == 0) {
        return;
    }

    // Load the hook DLL into OUR process only to resolve its exported hook proc. Its DllMain detects
    // it isn't running inside explorer and does nothing.
    HMODULE hookMod = LoadLibraryW(HookDllPath().c_str());
    if (hookMod == nullptr) {
        return;
    }

    HOOKPROC proc = reinterpret_cast<HOOKPROC>(GetProcAddress(hookMod, "OledCareCallWndProc"));
    if (proc != nullptr) {
        // Install the thread-scoped hook, then nudge the taskbar thread with a benign WM_NULL so
        // Windows maps our DLL into explorer right away (its DllMain starts the XAML connection on
        // its own thread). Once mapped the hook has served its only purpose, so we remove it - the
        // DLL stays loaded in explorer independently.
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, proc, hookMod, tid);
        if (hook != nullptr) {
            SendMessageTimeoutW(tray, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, nullptr);
            UnhookWindowsHookEx(hook);
        }
    }

    FreeLibrary(hookMod);  // drop our copy; explorer's copy stays mapped
}
