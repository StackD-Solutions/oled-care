// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/components/ScreenDimmer.h"

#include <windows.h>
#include <shellapi.h>

#include "ui/SettingsModel.h"

#pragma comment(lib, "shell32.lib")

namespace ui {

namespace {

const wchar_t* kDimClass = L"OledCareDimWnd";
const UINT_PTR kDimTimerId = 1;
const int kTickMs = 50;        // fade/poll cadence (~20 fps - smooth, negligible CPU)
const int kStepIn = 4;         // alpha step per tick while dimming  (gentle ~3s fade-in)
const int kStepOut = 18;       // alpha step per tick while restoring (snappy ~0.7s fade-out)
const int kRefreshTicks = 20;  // re-read settings ~once per second (20 * 50ms)

HWND g_dimWnd = nullptr;
int g_curAlpha = 0;            // current overlay alpha (0 = clear, 255 = fully black)
int g_amountPct = 0;          // cached Dim amount (0 = Off .. 90)
int g_afterSec = 300;         // cached idle threshold (seconds)
bool g_busy = false;          // cached "fullscreen / presentation / busy" state
int g_tick = 0;

// True while the foreground window covers a whole monitor (a fullscreen game or video player), so we
// don't black out over it even though there's no mouse/keyboard input.
bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr || fg == g_dimWnd) {
        return false;
    }
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0) {
        return false;  // the desktop / shell is not "fullscreen content"
    }
    RECT wr;
    if (!GetWindowRect(fg, &wr)) {
        return false;
    }
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) {
        return false;
    }
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// True while a fullscreen game / video / presentation is up (so we never dim over it).
bool DisplayBusy() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE)) {
        return true;
    }
    return ForegroundIsFullscreen();
}

// Cover the whole virtual desktop (all monitors) and sit on top.
void SizeToVirtualScreen() {
    if (g_dimWnd != nullptr) {
        SetWindowPos(g_dimWnd, HWND_TOPMOST,
                     GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     SWP_NOACTIVATE);
    }
}

LRESULT CALLBACK DimProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER: {
        if (wParam != kDimTimerId) {
            break;
        }
        // Re-read the (registry-backed) settings about once a second so Settings-window changes apply.
        if (g_tick++ % kRefreshTicks == 0) {
            g_amountPct = settings::GetDimAmount();
            g_afterSec = settings::GetDimAfterSeconds();
            g_busy = DisplayBusy();
        }
        LASTINPUTINFO lii = { sizeof(lii), 0 };
        GetLastInputInfo(&lii);
        const DWORD idleMs = GetTickCount() - lii.dwTime;
        const bool shouldDim = g_amountPct > 0 && !g_busy &&
                               idleMs >= static_cast<DWORD>(g_afterSec) * 1000u;
        const int target = shouldDim ? (g_amountPct * 255 / 100) : 0;
        if (g_curAlpha == target) {
            break;
        }
        if (g_curAlpha < target) {
            g_curAlpha = (g_curAlpha + kStepIn < target) ? g_curAlpha + kStepIn : target;
        } else {
            g_curAlpha = (g_curAlpha - kStepOut > target) ? g_curAlpha - kStepOut : target;
        }
        if (g_curAlpha > 0 && !IsWindowVisible(hwnd)) {
            SizeToVirtualScreen();  // (re)cover the desktop + go topmost before the first fade-in frame
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(g_curAlpha), LWA_ALPHA);
        if (g_curAlpha == 0) {
            ShowWindow(hwnd, SW_HIDE);  // fully restored - get the overlay out of the way entirely
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
        SizeToVirtualScreen();  // monitor added/removed or resolution changed
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void StartScreenDimmer(HINSTANCE hInst) {
    if (g_dimWnd != nullptr) {
        return;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DimProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kDimClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));  // a solid black sheet
    RegisterClassExW(&wc);

    // Topmost, click-through (WS_EX_TRANSPARENT), no taskbar/alt-tab entry (WS_EX_TOOLWINDOW), never
    // takes focus (WS_EX_NOACTIVATE), uniform alpha via WS_EX_LAYERED. Created hidden; the timer shows
    // it only while actually dimming.
    g_dimWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kDimClass, L"", WS_POPUP,
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
        nullptr, nullptr, hInst, nullptr);
    if (g_dimWnd == nullptr) {
        return;
    }
    SetLayeredWindowAttributes(g_dimWnd, 0, 0, LWA_ALPHA);  // start fully transparent (hidden)
    SetTimer(g_dimWnd, kDimTimerId, kTickMs, nullptr);
}

}  // namespace ui
