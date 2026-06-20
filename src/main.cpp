// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

// OledCare - tray host + injector.
//
// On launch it injects OledCareHook.dll into explorer (which hides the system tray and
// reveals it on hover), so the behavior is ON by default. The tray menu is a fully custom
// popup window (square, dark, self-drawn - so we control border/sizing exactly) with a
// Settings item that opens a small window with two checkboxes, plus Exit.

#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <wtsapi32.h>

#include "service.h"
#include "injector.h"
#include "ui/components/AppIcon.h"
#include "ui/components/TrayMenu.h"
#include "ui/components/AboutDialog.h"
#include "ui/components/StyledCombo.h"
#include "ui/components/SettingsWindow.h"
#include "ui/components/ScreenDimmer.h"
#include "ui/Theme.h"
#include "ui/AppShared.h"
#include "ui/SettingsModel.h"
#include "AppContract.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "ole32.lib")

using namespace theme;     // tray app palette + shared fonts (ui/Theme.h)
using namespace settings;  // registry-backed settings, theme, hotkey, monitors (ui/SettingsModel.h)

// App-lifecycle globals shared with the settings model + window (declared in ui/AppShared.h). main
// owns their lifetime; settings:: and ui:: reach them through the extern declarations there.
HINSTANCE g_hInst = nullptr;
HWND g_mainWnd = nullptr;
HANDLE g_disabledEvent = nullptr;
HANDLE g_hideAllEvent = nullptr;
HICON g_appIcon = nullptr;
HICON g_appIconSm = nullptr;
HICON g_shieldIcon = nullptr;

namespace {

const wchar_t* kWindowClass = L"OledCareHiddenWnd";
const wchar_t* kDisabledEvent = contract::kEventDisabled;
const wchar_t* kPeekEvent = contract::kEventPeek;
const wchar_t* kHideAllEvent = contract::kEventHideAll;
const wchar_t* kMutexName = contract::kMutexName;

const UINT WM_TRAYICON = WM_APP + 1;
const UINT_PTR kPeekTimerId = 1;    // auto-hide timer for the peek reveal
const UINT_PTR kInjectTimerId = 2;  // startup: keep retrying the hook inject until the shell is up

enum MenuId : UINT { ID_SETTINGS = 1001, ID_QUIT = 1002, ID_ABOUT = 1003 };

NOTIFYICONDATAW g_nid = {};
UINT g_taskbarCreatedMsg = 0;
HANDLE g_peekEvent = nullptr;
ULONG_PTR g_gdiplusToken = 0;

// Tray popup contents - rendered + dispatched by ui::ShowCustomMenu (ui/components/TrayMenu). The
// icon fields point to bitmaps created at startup; on click the menu posts WM_COMMAND(id) back here.
ui::MenuItem g_cmenu[] = {
    { ID_SETTINGS, L"Settings", &g_iconSettings, false },
    { ID_ABOUT, L"About", &g_iconInfo, false },
    { 0, nullptr, nullptr, true },
    { ID_QUIT, L"Exit", &g_iconQuit, false },
};

// ---- tray icon + window ----------------------------------------------------

void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_appIconSm != nullptr ? g_appIconSm : LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, contract::kProductName, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// ---- crash-loop guard ------------------------------------------------------
// The injected hook runs inside Explorer, so a bug there can crash the shell - and Explorer
// auto-restarts, broadcasts TaskbarCreated, and we re-inject... straight back into the same crash.
// If Explorer restarts kCrashLoopMax times within kCrashLoopWindowMs we stop re-injecting and tell
// the user, instead of fueling the loop. Normal use injects once at startup + once per (rare) genuine
// restart, so this never trips in practice.
const int kCrashLoopMax = 4;
const DWORD kCrashLoopWindowMs = 15000;
DWORD g_reinjectTicks[kCrashLoopMax] = {};
int g_reinjectIdx = 0;
bool g_injectSuppressed = false;

// Record an Explorer-restart-triggered re-inject; returns true once the last kCrashLoopMax of them
// all landed within kCrashLoopWindowMs (a crash loop).
bool ReinjectLoopDetected() {
    const DWORD now = GetTickCount();
    g_reinjectTicks[g_reinjectIdx] = now;
    g_reinjectIdx = (g_reinjectIdx + 1) % kCrashLoopMax;
    for (DWORD t : g_reinjectTicks) {
        if (t == 0 || now - t > kCrashLoopWindowMs) {
            return false;
        }
    }
    return true;
}

// Balloon-notify the user that we paused after an Explorer crash loop (relaunch clears it).
void NotifyInjectSuppressed() {
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(g_nid.szInfoTitle, L"OledCare paused", ARRAYSIZE(g_nid.szInfoTitle));
    lstrcpynW(g_nid.szInfo,
              L"Explorer restarted several times in a row, so OledCare stopped re-applying to avoid a "
              L"loop. Your Windows build may have changed - re-enable from Settings once it's sorted.",
              ARRAYSIZE(g_nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;  // restore default flags for later updates
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg != 0) {
        // Explorer (re)created the shell - re-add our tray icon and re-inject the hook, unless the
        // crash-loop guard has tripped (the hook is crashing the shell on this Windows build).
        AddTrayIcon(hwnd);
        if (g_injectSuppressed) {
            return 0;
        }
        if (ReinjectLoopDetected()) {
            g_injectSuppressed = true;
            NotifyInjectSuppressed();
            return 0;
        }
        if (IsEnabled()) {
            InjectHook();  // re-inject into the freshly (re)created shell
        }
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            ui::ShowCustomMenu(hwnd, pt, g_cmenu, ARRAYSIZE(g_cmenu));
        }
        return 0;
    case WM_COMMAND:
        // The custom tray menu posts the chosen item's id here (ui/components/TrayMenu is decoupled
        // from the actions, so the owner decides what each command does).
        switch (LOWORD(wParam)) {
        case ID_SETTINGS:
            ui::ShowSettings();
            break;
        case ID_ABOUT:
            ui::ShowAbout(g_appIcon, g_appIconSm);
            break;
        case ID_QUIT:
            DestroyWindow(g_mainWnd);
            break;
        }
        return 0;
    case WM_HOTKEY:
        if (wParam == kHotkeyId && g_peekEvent != nullptr) {
            // Reveal the tray (like hovering), then auto-hide after the configured peek
            // duration. Pressing again while peeking dismisses it immediately. Never touches
            // the "Hide system tray" feature itself.
            if (WaitForSingleObject(g_peekEvent, 0) == WAIT_OBJECT_0) {
                ResetEvent(g_peekEvent);
                KillTimer(hwnd, kPeekTimerId);
            } else {
                SetEvent(g_peekEvent);
                SetTimer(hwnd, kPeekTimerId, static_cast<UINT>(GetPeekSeconds()) * 1000, nullptr);
            }
        }
        return 0;
    case WM_TIMER:
        if (wParam == kPeekTimerId) {
            KillTimer(hwnd, kPeekTimerId);
            if (g_peekEvent != nullptr) {
                ResetEvent(g_peekEvent);  // peek elapsed -> resume hover-hide
            }
        } else if (wParam == kInjectTimerId) {
            // At boot the service can launch us before explorer's taskbar exists. Wait here until
            // Shell_TrayWnd appears, then inject once into a ready shell and stop retrying.
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tray != nullptr) {
                KillTimer(hwnd, kInjectTimerId);
                if (IsEnabled()) {
                    InjectHook();
                }
            }
        }
        return 0;
    case WM_POWERBROADCAST:
        if ((wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) &&
            !g_injectSuppressed && IsSafeStartEnabled() && IsEnabled()) {
            InjectHook();  // re-apply hiding after the PC wakes up
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_UNLOCK && !g_injectSuppressed && IsSafeStartEnabled() && IsEnabled()) {
            InjectHook();
        }
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, kHotkeyId);
        if (g_peekEvent != nullptr) {
            ResetEvent(g_peekEvent);  // clear any peek so a relaunch starts hidden
        }
        WTSUnRegisterSessionNotification(hwnd);
        if (g_disabledEvent != nullptr) {
            SetEvent(g_disabledEvent);  // restore the tray on quit
        }
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Service modes (dispatched before the tray app): --service runs as the Windows
    // service; --install-service / --uninstall-service manage it (require admin).
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        int ret = -1;
        if (argv != nullptr && argc > 1) {
            if (wcscmp(argv[1], L"--service") == 0) {
                ret = RunServiceMain();
            } else if (wcscmp(argv[1], L"--install-service") == 0) {
                ret = InstallService() ? 0 : 1;
            } else if (wcscmp(argv[1], L"--uninstall-service") == 0) {
                ret = UninstallService() ? 0 : 1;
            } else if (wcscmp(argv[1], L"--set-protect") == 0) {
                SetProtectAgainstCrashes(argc > 2 && wcscmp(argv[2], L"1") == 0);
                ret = 0;
            } else if (wcscmp(argv[1], L"--export-icon") == 0 && argc > 2) {
                ULONG_PTR tok = 0;
                Gdiplus::GdiplusStartupInput gi;
                Gdiplus::GdiplusStartup(&tok, &gi, nullptr);
                ret = ui::ExportIco(argv[2]) ? 0 : 1;
                Gdiplus::GdiplusShutdown(tok);
            }
        }
        if (argv != nullptr) {
            LocalFree(argv);
        }
        if (ret != -1) {
            return ret;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        return kExitAlreadyRunning;  // distinct from a user Exit so the service stays running
    }

    g_hInst = hInstance;
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_disabledEvent = CreateEventW(nullptr, TRUE, FALSE, kDisabledEvent);
    if (g_disabledEvent != nullptr) {
        ResetEvent(g_disabledEvent);
    }
    g_peekEvent = CreateEventW(nullptr, TRUE, FALSE, kPeekEvent);
    if (g_peekEvent != nullptr) {
        ResetEvent(g_peekEvent);
    }
    g_hideAllEvent = CreateEventW(nullptr, TRUE, FALSE, kHideAllEvent);
    ApplyHideAllEvent();

    Gdiplus::GdiplusStartupInput gdiInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiInput, nullptr);

    int dpi = GetDpiForSystem();
    g_iconPx = MulDiv(18, dpi, 96);
    g_themeMode = GetThemeMode();
    ApplyTheme();  // resolves colors, the background brush, the process theme, and menu icons

    g_appIcon = ui::CreateAppIcon(MulDiv(32, dpi, 96));
    g_appIconSm = ui::CreateAppIcon(MulDiv(16, dpi, 96));

    SHSTOCKICONINFO sii = {};
    sii.cbSize = sizeof(sii);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &sii))) {
        g_shieldIcon = sii.hIcon;
    }

    g_uiFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_menuFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = g_appIcon;
    wc.hIconSm = g_appIconSm;
    RegisterClassExW(&wc);

    // (The tray menu's + About window's classes register themselves inside their ui/components/ files.)

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"OledCare",
                               WS_OVERLAPPED, 0, 0, 0, 0,
                               nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        return 1;
    }
    g_mainWnd = hwnd;

    // Let explorer's "TaskbarCreated" broadcast reach us even when the high-priority service
    // launched us at a higher integrity level than explorer - UIPI would otherwise silently drop
    // it, so the hook would never re-inject after the shell (re)starts. This is the boot case.
    ChangeWindowMessageFilterEx(hwnd, g_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);

    AllowWindowDarkMode(hwnd, g_effectiveDark);
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);  // for WM_WTSSESSION_CHANGE (unlock)
    LoadHotkey();
    RegisterRevealHotkey();
    AddTrayIcon(hwnd);
    // Inject now if the shell is already up; otherwise keep retrying until it is. At boot the
    // service can start us before explorer exists, so a one-shot inject here silently misses.
    {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tray != nullptr) {
            InjectHook();
        } else {
            SetTimer(hwnd, kInjectTimerId, 100, nullptr);  // poll fast so we inject the instant the shell appears
        }
    }
    ui::StartScreenDimmer(hInstance);  // idle-driven OLED dimmer (no-op until Dim amount is set)
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
    }
    return 0;
}
