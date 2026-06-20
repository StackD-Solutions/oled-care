// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

// OledCareHook.dll - hides the whole Windows 11 system tray and reveals it on hover.
//
// How it works:
//  1. We're injected into explorer.exe and register as a XAML-diagnostics TAP.
//  2. InitializeXamlDiagnosticsEx (endpoint "VisualDiagConnection<N>", retried on a fresh
//     thread until ready) connects us to explorer's live XAML.
//  3. SetSite gives us IXamlDiagnostics; from a worker thread we AdviseVisualTreeChange on
//     IVisualTreeService3, which replays the whole tree to OnVisualTreeChange.
//  4. We find SystemTray.SystemTrayFrame (the whole tray), set Opacity=0 to hide it, and
//     start a UI-thread timer that reveals it (Opacity=1) while the cursor is over the
//     right edge of the taskbar, hiding it again shortly after the cursor leaves.

#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <objbase.h>
#include <ocidl.h>
#include <xamlom.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AppContract.h"  // shared cross-binary contract: events, reg paths/keys, TAP CLSID

// Pull in the ABI UIElement interface so we can set Opacity. windows.h defines a
// GetCurrentTime macro that collides with a XAML method, so guard the include.
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.media.h>
#pragma pop_macro("GetCurrentTime")

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

static HMODULE g_hModule = nullptr;
static HANDLE g_disabledEvent = nullptr;  // signaled by the app = behavior off (keep tray shown)
static HANDLE g_peekEvent = nullptr;      // signaled by the app = reveal hotkey peeking (show tray)
static HANDLE g_hideAllEvent = nullptr;   // signaled by the app = also hide secondary taskbars


static void LogA(const char* s) {
    wchar_t path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) {
        return;
    }
    lstrcatW(path, L"OledCareHook.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD w = 0;
    WriteFile(f, s, static_cast<DWORD>(strlen(s)), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

// Hover-reveal state. The timer runs on the taskbar UI thread.
class Tap;
// The TAP whose advise is streaming the live taskbar tree; set by BecomeActiveTap() the moment a
// TAP starts tracking the tray/background, read by the hover timer proc. The cold-boot "wrong tree"
// problem is solved upstream by the connect gate (HookThread waits for the taskbar's XAML host to
// settle before its single connect), so there is exactly one streaming TAP and it is the driver.
static Tap* g_activeTap = nullptr;
// Every TAP whose advise succeeded, so the self-unload path can detach (unadvise) ALL of them - a
// stale TAP left advised would fire OnVisualTreeChange into the freed DLL and crash explorer. The
// advise threads register cross-thread; the UI-thread unload iterates. Guarded by g_tapsCs.
static std::vector<Tap*> g_allTaps;
static CRITICAL_SECTION g_tapsCs;
static bool g_tapsCsInit = false;
static double g_curOpacity = 0.0;    // current primary opacity (animated by the fade)
static double g_tgtOpacity = 0.0;    // opacity the fade is moving toward (0 = hidden, 1 = shown)
static int g_tickCounter = 0;        // timer ticks; hover decision runs every 4th
static DWORD g_fadeInMs = 150;       // fade-in / fade-out durations (read from the app's settings)
static DWORD g_fadeOutMs = 300;
static DWORD g_lastInZoneTick = 0;
static bool g_timerSet = false;
static UINT_PTR g_timerId = 0;          // the hover timer id, so KillTimer can stop it on unload
static HANDLE g_unloadEvent = nullptr;  // signaled by the uninstaller = cleanly detach + self-unload
static HANDLE g_connectedEvent = nullptr; // WE signal it once advising = "live"; the app refreshes explorer if it never arrives
static volatile LONG g_canUnload = 0;   // set once torn down, so DllCanUnloadNow lets COM release us
static VOID CALLBACK TrayHoverTimerProc(HWND, UINT, UINT_PTR, DWORD);

// Reset the taskbar window's composition accent to the Windows default (ACCENT_DISABLED), via
// the undocumented SetWindowCompositionAttribute. We only need this to clear any leftover accent
// an older build may have applied, so that Clear (hidden fill) shows the real desktop behind it
// and Normal is exactly the Windows-configured taskbar. Called from inside explorer.
struct OcAccentPolicy { int state; int flags; unsigned int color; int anim; };
struct OcWcaData { int attrib; void* data; size_t size; };
typedef BOOL(WINAPI* OcSwca_t)(HWND, OcWcaData*);

static void ApplyAccentForWindow(HWND hwnd, bool transparent) {
    static OcSwca_t fn = reinterpret_cast<OcSwca_t>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (fn == nullptr || hwnd == nullptr) {
        return;
    }
    OcAccentPolicy p = {};
    if (transparent) {
        p.state = 2;            // ACCENT_ENABLE_TRANSPARENTGRADIENT - makes the window's composition
        p.flags = 2;            // backdrop (the material behind the XAML) fully see-through
        p.color = 0x00000000;   // 0 alpha = transparent
    }  // else state = 0 = ACCENT_DISABLED -> Windows default backdrop (Normal)
    OcWcaData d = { 19 /*WCA_ACCENT_POLICY*/, &p, sizeof(p) };
    fn(hwnd, &d);
}

static void ApplyTaskbarAccent(bool transparent) {
    HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tb != nullptr) {
        ApplyAccentForWindow(tb, transparent);
    }
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        ApplyAccentForWindow(sec, transparent);
    }
}

// The TAP: receives the visual-tree stream, hides the tray, drives hover-reveal.
class Tap : public IObjectWithSite, public IVisualTreeServiceCallback2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IObjectWithSite)) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else if (riid == __uuidof(IVisualTreeServiceCallback)) {
            *ppv = static_cast<IVisualTreeServiceCallback*>(static_cast<IVisualTreeServiceCallback2*>(this));
        } else if (riid == __uuidof(IVisualTreeServiceCallback2)) {
            *ppv = static_cast<IVisualTreeServiceCallback2*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        InterlockedIncrement(&m_ref);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) {
            delete this;
        }
        return r;
    }

    // IObjectWithSite - the framework hands us IXamlDiagnostics here (on the UI thread).
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (site == nullptr) {
            return S_OK;
        }
        site->AddRef();
        InterlockedIncrement(&m_ref);  // keep this Tap alive for the worker thread
        IUnknown* siteRef = site;
        Tap* self = this;
        // Advise from a SEPARATE thread: AdviseVisualTreeChange marshals callbacks to the
        // XAML UI thread, so calling it directly from SetSite (already there) won't replay.
        std::thread([self, siteRef]() {
            siteRef->QueryInterface(__uuidof(IXamlDiagnostics),
                                    reinterpret_cast<void**>(&self->m_xamlDiag));
            IVisualTreeService3* vts3 = nullptr;
            HRESULT hr = siteRef->QueryInterface(__uuidof(IVisualTreeService3),
                                                 reinterpret_cast<void**>(&vts3));
            if (SUCCEEDED(hr) && vts3 != nullptr) {
                LogA("advising visual-tree change (IVisualTreeService3)");
                HRESULT ahr = vts3->AdviseVisualTreeChange(static_cast<IVisualTreeServiceCallback2*>(self));
                char l[96];
                _snprintf_s(l, sizeof(l), _TRUNCATE, "AdviseVisualTreeChange hr=0x%08X",
                            static_cast<unsigned>(ahr));
                LogA(l);
                self->m_vts3 = vts3;
                // Register for the unload sweep: this TAP is now advised, so it must be unadvised
                // before the DLL can unload, even if a newer-generation TAP supersedes it as driver.
                if (g_tapsCsInit) {
                    EnterCriticalSection(&g_tapsCs);
                    g_allTaps.push_back(self);
                    LeaveCriticalSection(&g_tapsCs);
                }
                if (SUCCEEDED(ahr) && g_connectedEvent != nullptr) {
                    SetEvent(g_connectedEvent);  // we're fully live - tell the app NOT to refresh explorer
                }
            } else {
                char l[96];
                _snprintf_s(l, sizeof(l), _TRUNCATE, "ERROR QI IVisualTreeService3 hr=0x%08X",
                            static_cast<unsigned>(hr));
                LogA(l);
            }
            siteRef->Release();
            self->Release();
        }).detach();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID, void** ppv) override {
        *ppv = nullptr;
        return E_NOTIMPL;
    }

    // Walk parent links to decide whether an element lives inside the taskbar itself (vs. a popup
    // such as a right-click menu or window-preview, which has identically named BackgroundFill/
    // Stroke elements that must NOT be hidden - doing so breaks those popups).
    bool IsTaskbarDescendant(InstanceHandle h) {
        if (m_taskbarFrame == 0) {
            return false;
        }
        for (int guard = 0; h != 0 && guard < 64; ++guard) {
            if (h == m_taskbarFrame) {
                return true;
            }
            auto it = m_parents.find(h);
            if (it == m_parents.end()) {
                return false;
            }
            h = it->second;
        }
        return false;
    }

    // True if the element is (or descends from) a hover-flyout background - i.e. the window-preview
    // popup. Its background is a Taskbar.TaskbarBackground too, so it must be excluded explicitly.
    bool IsInFlyout(InstanceHandle h) {
        for (int guard = 0; h != 0 && guard < 64; ++guard) {
            for (InstanceHandle f : m_flyoutRoots) {
                if (h == f) {
                    return true;
                }
            }
            auto it = m_parents.find(h);
            if (it == m_parents.end()) {
                return false;
            }
            h = it->second;
        }
        return false;
    }

    // The advise replays the current tree as Add notifications. We watch for the SystemTrayFrame
    // (to hide the tray) and the taskbar's own background fill/stroke (to hide for Clear).
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                                 VisualMutationType type) override {
        if (element.Type == nullptr) {
            return S_OK;
        }
        // Maintain the child->parent map + the taskbar root so we can distinguish the taskbar's own
        // background from identically named elements inside popups.
        if (type == Add) {
            m_parents[element.Handle] = relation.Parent;
            if (wcscmp(element.Type, L"Taskbar.TaskbarFrame") == 0) {
                m_taskbarFrame = element.Handle;
            }
        } else if (type == Remove) {
            m_parents.erase(element.Handle);
            for (size_t i = 0; i < m_taskbarBgs.size(); ++i) {
                if (m_taskbarBgs[i] == element.Handle) {
                    m_taskbarBgs.erase(m_taskbarBgs.begin() + i);
                    break;
                }
            }
            for (size_t i = 0; i < m_flyoutRoots.size(); ++i) {
                if (m_flyoutRoots[i] == element.Handle) {
                    m_flyoutRoots.erase(m_flyoutRoots.begin() + i);
                    break;
                }
            }
        }
        // Track the taskbar's OWN background fill/stroke (the dark area between icons + the border),
        // but ONLY when they descend from the taskbar frame and are NOT inside a hover-flyout (the
        // window-preview popup, whose background is the same type and would otherwise get hidden).
        if (type == Add) {
            const bool isTaskbarBg = wcscmp(element.Type, L"Taskbar.TaskbarBackground") == 0;
            // The window-preview flyout's background is also a Taskbar.TaskbarBackground, named
            // "HoverFlyoutBackgroundControl". Remember it so we exclude it + its fill/stroke.
            if (isTaskbarBg && element.Name != nullptr &&
                wcscmp(element.Name, L"HoverFlyoutBackgroundControl") == 0) {
                bool known = false;
                for (InstanceHandle h : m_flyoutRoots) {
                    if (h == element.Handle) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    m_flyoutRoots.push_back(element.Handle);
                    LogA("flyout bg noted (excluded from hiding): HoverFlyoutBackgroundControl");
                }
            }
            const bool isNamedBg = element.Name != nullptr &&
                (wcscmp(element.Name, L"BackgroundFill") == 0 || wcscmp(element.Name, L"BackgroundStroke") == 0);
            // The taskbar's own background container is named "BackgroundControl" (not the flyout's).
            const bool isOwnContainer = isTaskbarBg && element.Name != nullptr &&
                wcscmp(element.Name, L"BackgroundControl") == 0;
            if (isOwnContainer ||
                (isNamedBg && IsTaskbarDescendant(element.Handle) && !IsInFlyout(element.Handle))) {
                bool already = false;
                for (InstanceHandle h : m_taskbarBgs) {
                    if (h == element.Handle) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    m_taskbarBgs.push_back(element.Handle);
                    char l[96];
                    _snprintf_s(l, sizeof(l), _TRUNCATE, "bg tracked: %S [%S]",
                                element.Type, element.Name ? element.Name : L"");
                    LogA(l);
                    BecomeActiveTap();
                    if (!g_timerSet) {
                        g_timerId = SetTimer(nullptr, 0, 30, TrayHoverTimerProc);
                        g_timerSet = true;
                        LogA("hover timer started (taskbar bg)");
                    }
                }
            }
        }
        if (wcscmp(element.Type, L"SystemTray.SystemTrayFrame") != 0) {
            return S_OK;
        }
        if (type == Add) {
            for (InstanceHandle h : m_frames) {
                if (h == element.Handle) {
                    return S_OK;  // already tracked (the tree can re-stream)
                }
            }
            bool first = m_frames.empty();
            m_frames.push_back(element.Handle);
            m_lastSecondaryOpacity = -1.0;  // re-evaluate secondaries on the next tick
            char l[64];
            _snprintf_s(l, sizeof(l), _TRUNCATE, "SystemTrayFrame #%d found", static_cast<int>(m_frames.size()));
            LogA(l);
            if (first) {
                // The first frame (primary taskbar) drives hover; hide it now + start polling.
                SetOpacityFor(element.Handle, 0.0);
                g_curOpacity = 0.0;
                g_tgtOpacity = 0.0;
                BecomeActiveTap();
                if (!g_timerSet) {
                    g_timerId = SetTimer(nullptr, 0, 30, TrayHoverTimerProc);  // ~30ms: fades smoothly, polls cursor
                    g_timerSet = true;
                    LogA("hover timer started");
                }
            }
        } else if (type == Remove) {
            for (size_t i = 0; i < m_frames.size(); ++i) {
                if (m_frames[i] == element.Handle) {
                    m_frames.erase(m_frames.begin() + i);
                    m_lastSecondaryOpacity = -1.0;
                    break;
                }
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override {
        return S_OK;
    }

    InstanceHandle PrimaryFrame() const {
        return m_frames.empty() ? 0 : m_frames.front();
    }

    // Set a tray frame's opacity (0 = hidden, 1 = visible). Must run on the XAML UI thread -
    // both callers (OnVisualTreeChange and the UI-thread timer) satisfy that.
    void SetOpacityFor(InstanceHandle handle, double opacity) {
        if (m_xamlDiag == nullptr || handle == 0) {
            return;
        }
        IInspectable* insp = nullptr;
        if (SUCCEEDED(m_xamlDiag->GetIInspectableFromHandle(handle, &insp)) && insp != nullptr) {
            ABI::Windows::UI::Xaml::IUIElement* uie = nullptr;
            if (SUCCEEDED(insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IUIElement),
                                               reinterpret_cast<void**>(&uie))) && uie != nullptr) {
                uie->put_Opacity(opacity);
                uie->Release();
            }
            insp->Release();
        }
    }

    void SetTrayOpacity(double opacity) {
        SetOpacityFor(PrimaryFrame(), opacity);  // primary = the hover-revealed taskbar
    }

    // Secondary taskbars (extra monitors) are hide-only: hidden when "hide on all displays"
    // is on, shown otherwise. They do not get hover-reveal.
    void UpdateSecondaries(bool disabled, bool hideAll) {
        double op = disabled ? 1.0 : (hideAll ? 0.0 : 1.0);
        if (op == m_lastSecondaryOpacity) {
            return;
        }
        InstanceHandle primary = PrimaryFrame();
        for (InstanceHandle h : m_frames) {
            if (h != primary) {
                SetOpacityFor(h, op);
            }
        }
        m_lastSecondaryOpacity = op;
    }

    // Taskbar appearance, applied from inside explorer. For any non-Normal mode we hide the
    // XAML background fill so the window's composition backdrop shows, then set the accent:
    //   Normal -> fill shown, no accent      Opaque -> fill hidden, solid backdrop
    //   Clear  -> fill hidden, no accent      Blur   -> fill hidden, blur backdrop
    //   Acrylic -> fill hidden, acrylic backdrop
    // Both are cached (re-applied only on change, or after a taskbar rebuild resets the cache),
    // so we never spam the composition API - that's what avoids the flicker. The app forces
    // Windows' transparency compositing on for Blur/Acrylic. App not running -> Normal.
    // Runs on the XAML UI thread (the timer).
    void UpdateTaskbarBackground(int taskbarMode, bool appRunning) {
        // Two modes: Normal (Windows default) and Clear (mode 2). App not running -> Normal so
        // quitting restores the taskbar. Clear does TWO things: hide the XAML background fills
        // (opacity re-asserted every tick, see ReassertTaskbarBg) AND apply a TRANSPARENTGRADIENT
        // composition accent to the window (re-asserted here every ~120ms) so the material backdrop
        // behind the XAML is see-through too. Explorer re-asserts its own accent on events
        // (auto-hide, theme), so we keep overriding it every tick.
        m_bgTarget = (appRunning && taskbarMode == 2 /*Clear*/) ? 0.0 : 1.0;
        // The opacity AND the composition accent are (re)applied by ReassertTaskbarBg only when the
        // background drifts from this target - including on a mode change, when it drifts immediately.
    }

    // Read an element's current opacity (-1 if the handle is stale/unreadable).
    double GetOpacityFor(InstanceHandle handle) {
        if (m_xamlDiag == nullptr || handle == 0) {
            return -1.0;
        }
        IInspectable* insp = nullptr;
        double op = -1.0;
        if (SUCCEEDED(m_xamlDiag->GetIInspectableFromHandle(handle, &insp)) && insp != nullptr) {
            ABI::Windows::UI::Xaml::IUIElement* uie = nullptr;
            if (SUCCEEDED(insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IUIElement),
                                               reinterpret_cast<void**>(&uie))) && uie != nullptr) {
                uie->get_Opacity(&op);
                uie->Release();
            }
            insp->Release();
        }
        return op;
    }

    // Keep the taskbar background hidden, but only ACT when explorer has actually changed it.
    // Called every tick (~30ms): we read the fill's current opacity and only re-apply (opacity +
    // composition accent) if it drifted from the target. When the taskbar is stable - which is the
    // normal case, including while a right-click menu or flyout is open - we touch nothing, so we
    // don't race the menu's render pass (that was making its text/icons fail to paint).
    void ReassertTaskbarBg() {
        bool drifted = false;
        for (InstanceHandle h : m_taskbarBgs) {
            double cur = GetOpacityFor(h);
            if (cur >= 0.0 && cur != m_bgTarget) {  // a live handle explorer changed back from our hide
                drifted = true;
            }
        }
        if (!drifted) {
            return;
        }
        for (InstanceHandle h : m_taskbarBgs) {
            SetOpacityFor(h, m_bgTarget);
        }
        // NOTE: we deliberately do NOT apply a transparent composition accent here. It would make
        // the whole taskbar window's backdrop see-through, which bleeds into child popups (the
        // window-preview / menus render with a transparent card). Hiding the fill/stroke alone is
        // enough for Clear on this build.
    }

    // The tray's actual rendered width in DIPs, so the hover zone matches however many
    // icons are currently showing. Returns 0 if not measurable yet.
    double GetTrayWidthDips() {
        InstanceHandle primary = PrimaryFrame();
        if (m_xamlDiag == nullptr || primary == 0) {
            return 0.0;
        }
        IInspectable* insp = nullptr;
        double width = 0.0;
        if (SUCCEEDED(m_xamlDiag->GetIInspectableFromHandle(primary, &insp)) && insp != nullptr) {
            ABI::Windows::UI::Xaml::IFrameworkElement* fe = nullptr;
            if (SUCCEEDED(insp->QueryInterface(__uuidof(ABI::Windows::UI::Xaml::IFrameworkElement),
                                               reinterpret_cast<void**>(&fe))) && fe != nullptr) {
                fe->get_ActualWidth(&width);
                fe->Release();
            }
            insp->Release();
        }
        return width;
    }

    // Cleanly detach: restore the tray + taskbar to normal, stop receiving tree callbacks, and
    // release the diagnostics COM objects. Must run on the XAML UI thread (the timer). After this
    // the Tap references nothing inside explorer, so the DLL can be safely unloaded.
    void Detach() {
        for (InstanceHandle h : m_frames) {
            SetOpacityFor(h, 1.0);  // bring the system tray back
        }
        for (InstanceHandle h : m_taskbarBgs) {
            SetOpacityFor(h, 1.0);  // bring the taskbar background back
        }
        if (m_vts3 != nullptr) {
            m_vts3->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback2*>(this));
            m_vts3->Release();
            m_vts3 = nullptr;
        }
        if (m_xamlDiag != nullptr) {
            m_xamlDiag->Release();
            m_xamlDiag = nullptr;
        }
    }

    // Become the timer's active TAP - the one whose advise is streaming the live taskbar tree.
    // Runs on the UI thread (OnVisualTreeChange), so the plain pointer write is safe.
    void BecomeActiveTap() {
        g_activeTap = this;
    }

private:
    LONG m_ref = 1;
    IVisualTreeService3* m_vts3 = nullptr;
    IXamlDiagnostics* m_xamlDiag = nullptr;
    std::vector<InstanceHandle> m_frames;  // all tray frames; front() = primary (hover)
    double m_lastSecondaryOpacity = -1.0;  // last opacity applied to the secondary frames
    std::vector<InstanceHandle> m_taskbarBgs;  // taskbar background fills (BackgroundFill/Stroke/etc.)
    double m_bgTarget = 1.0;                    // target opacity, re-asserted every tick (0 = Clear)
    std::unordered_map<InstanceHandle, InstanceHandle> m_parents;  // child -> parent, to find ancestry
    InstanceHandle m_taskbarFrame = 0;          // Taskbar.TaskbarFrame handle (the taskbar root)
    std::vector<InstanceHandle> m_flyoutRoots;  // HoverFlyoutBackgroundControl handles (window-preview popups)
};

// True while a system-tray flyout is open (Quick Settings, calendar, or the hidden-icons
// overflow). The shell shows these top-level windows on demand and DWM-cloaks them (rather
// than SW_HIDE) when dismissed, so IsWindowVisible alone always reports them visible - we
// must also reject cloaked windows. We hold the tray revealed while one is open so it never
// hides out from under a panel the user just opened.
static bool IsTrayFlyoutOpen() {
    static const wchar_t* kFlyoutClasses[] = {
        L"TopLevelWindowForOverflowXamlIsland",  // hidden-icons overflow (the "^")
        L"NotifyIconOverflowWindow",             // legacy overflow flyout
        L"ControlCenterWindow",                  // Quick Settings (network / volume / battery)
        L"ClockFlyoutWindow",                    // calendar / clock / notifications
    };
    for (const wchar_t* cls : kFlyoutClasses) {
        HWND w = FindWindowW(cls, nullptr);
        if (w == nullptr || !IsWindowVisible(w)) {
            continue;
        }
        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            continue;  // cloaked = not actually on screen
        }
        return true;
    }
    return false;
}

// True while the OledCare app is running (it holds this single-instance mutex). Lets the hook
// restore the taskbar background when the app quits, even though the registry value persists.
static bool IsAppRunning() {
    // Is OledCare.exe alive? We scan the process list (session-agnostic) rather than opening the
    // app's single-instance mutex: at boot the app is launched by the LocalSystem service into the
    // user session, and the boot explorer the hook lives in couldn't see that session-scoped mutex
    // (OpenMutex failed -> we wrongly decided "app not running" and RESTORED the taskbar). A process
    // scan sees the app regardless of which session created its objects. Throttled to ~1/s.
    static DWORD s_lastCheck = 0;
    static bool s_cached = true;
    const DWORD now = GetTickCount();
    if (s_lastCheck != 0 && now - s_lastCheck < 1000) {
        return s_cached;
    }
    s_lastCheck = now;

    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return s_cached;  // transient failure -> keep the last answer, don't flap the taskbar
    }
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"OledCare.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    s_cached = found;
    return found;
}

// Read a DWORD setting written by the app (HKCU\Software\OledCare) - used for fade timings.
static DWORD ReadRegDword(const wchar_t* name, DWORD def) {
    HKEY key;
    DWORD v = def, sz = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, contract::kRegPath, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &sz);
        RegCloseKey(key);
    }
    return v;
}

// Unload the DLL from explorer. Runs on its OWN thread (NOT explorer's UI thread) so it can
// FreeLibrary the module without yanking code out from under a thread that's still in it. The brief
// sleep lets the UI thread return out of the timer proc (our code) before the module goes away.
//
// The DLL is held by TWO references: our app's LoadLibrary injection AND the XAML diagnostics
// framework's load for the TAP. After Detach() the framework will never call us again, so we drop
// both refs to actually unload + free the file. We first take a "pin" ref (LoadLibrary) so our own
// code stays mapped while we drop the other two; then FreeLibraryAndExitThread drops the pin and
// exits atomically, so no code ever runs in an unloaded module.
static DWORD WINAPI UnloadWorker(LPVOID) {
    Sleep(150);
    LogA("self-unloading hook DLL from explorer");
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    HMODULE pin = LoadLibraryW(path);  // +1 so this code can't be unmapped mid-flight
    FreeLibrary(g_hModule);            // drop the app's injection reference
    FreeLibrary(g_hModule);            // drop the XAML framework's reference
    if (pin != nullptr) {
        FreeLibraryAndExitThread(pin, 0);  // drop the pin + exit; nothing runs after the unload
    }
    return 0;  // not reached
}

// Runs on the taskbar UI thread (thread timer dispatched by explorer's message loop), ~30ms.
// Every ~120ms it decides whether the tray should be shown (cursor over the right edge, a
// flyout open, or force-shown); every tick it fades the opacity toward that target.
static VOID CALLBACK TrayHoverTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (g_activeTap == nullptr) {
        return;
    }
    // The uninstaller (or app) asked us to unload. Restore the tray + taskbar, stop the timer and
    // the tree advise, release COM, then FreeLibrary ourselves from a worker thread - so explorer
    // drops the DLL and the file frees immediately, with NO explorer restart.
    if (g_unloadEvent != nullptr && WaitForSingleObject(g_unloadEvent, 0) == WAIT_OBJECT_0) {
        g_activeTap = nullptr;
        if (g_timerSet) {
            KillTimer(nullptr, g_timerId);
            g_timerSet = false;
        }
        // Detach EVERY advised TAP (normally just one), not only the active one - any left advised
        // would call back into the freed DLL. All run on this UI thread.
        if (g_tapsCsInit) {
            EnterCriticalSection(&g_tapsCs);
            for (Tap* t : g_allTaps) {
                t->Detach();
            }
            g_allTaps.clear();
            LeaveCriticalSection(&g_tapsCs);
        }
        InterlockedExchange(&g_canUnload, 1);
        CreateThread(nullptr, 0, UnloadWorker, nullptr, 0, nullptr);
        return;
    }
    // Run the show/hide decision on the very FIRST tick (so Clear applies ~30ms after the hook
    // connects, not ~120ms later), then every 4th tick thereafter to keep registry reads light.
    ++g_tickCounter;
    if (g_tickCounter == 1 || g_tickCounter % 4 == 0) {
        const bool disabled = (g_disabledEvent != nullptr && WaitForSingleObject(g_disabledEvent, 0) == WAIT_OBJECT_0);
        const bool peek = (g_peekEvent != nullptr && WaitForSingleObject(g_peekEvent, 0) == WAIT_OBJECT_0);
        const bool hideAll = (g_hideAllEvent != nullptr && WaitForSingleObject(g_hideAllEvent, 0) == WAIT_OBJECT_0);
        g_activeTap->UpdateSecondaries(disabled, hideAll);
        g_fadeInMs = ReadRegDword(contract::kRegFadeInMs, 150);
        g_fadeOutMs = ReadRegDword(contract::kRegFadeOutMs, 300);
        const bool appRun = IsAppRunning();
        const int tbMode = static_cast<int>(ReadRegDword(contract::kRegTaskbarMode, 2));
        g_activeTap->UpdateTaskbarBackground(tbMode, appRun);

        bool show;
        if (disabled || peek) {
            show = true;  // app off (or quit), or the reveal hotkey is peeking -> keep shown
        } else {
            bool keepVisible = false;
            HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
            RECT r;
            if (tb != nullptr && GetWindowRect(tb, &r)) {
                POINT pt;
                GetCursorPos(&pt);
                // Zone width = the tray's ACTUAL rendered width (tracks the visible icon count),
                // DIPs -> physical px via the taskbar DPI, plus a margin. Fixed fallback if not measurable.
                const double widthDips = g_activeTap->GetTrayWidthDips();
                UINT dpi = GetDpiForWindow(tb);
                if (dpi == 0) {
                    dpi = 96;
                }
                const LONG zoneWidth = (widthDips > 1.0)
                    ? static_cast<LONG>(widthDips * dpi / 96.0) + 8
                    : 340;
                const bool inZone = (pt.x >= r.right - zoneWidth) && (pt.x <= r.right) &&
                                    (pt.y >= r.top) && (pt.y <= r.bottom);
                // Hold open while a tray flyout is showing (Quick Settings / calendar / overflow).
                keepVisible = inZone || IsTrayFlyoutOpen();
            }
            const DWORD now = GetTickCount();
            if (keepVisible) {
                g_lastInZoneTick = now;
                show = true;
            } else {
                // Linger briefly after the cursor leaves so brief mouse-outs don't flicker.
                const bool wasShowing = (g_tgtOpacity >= 0.5);
                show = wasShowing && (now - g_lastInZoneTick <= 600);
            }
        }
        g_tgtOpacity = show ? 1.0 : 0.0;
    }

    // Animate the primary tray opacity toward the target (the fade in / out).
    if (g_curOpacity != g_tgtOpacity) {
        const DWORD fadeMs = (g_tgtOpacity > g_curOpacity) ? g_fadeInMs : g_fadeOutMs;
        const double step = (fadeMs < 30) ? 1.0 : (30.0 / static_cast<double>(fadeMs));
        if (g_tgtOpacity > g_curOpacity) {
            g_curOpacity = (g_curOpacity + step > g_tgtOpacity) ? g_tgtOpacity : g_curOpacity + step;
        } else {
            g_curOpacity = (g_curOpacity - step < g_tgtOpacity) ? g_tgtOpacity : g_curOpacity - step;
        }
        g_activeTap->SetTrayOpacity(g_curOpacity);
    }

    // Re-assert the taskbar background opacity every tick so explorer can't leave it un-hidden
    // (keeps Clear robust against the intermittent black box around the icons).
    g_activeTap->ReassertTaskbarBg();
}

// Minimal class factory for our TAP CLSID.
class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        Tap* t = new Tap();
        HRESULT hr = t->QueryInterface(riid, ppv);
        t->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (rclsid == contract::kTapClsid) {
        return g_factory.QueryInterface(riid, ppv);
    }
    *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return InterlockedCompareExchange(&g_canUnload, 0, 0) != 0 ? S_OK : S_FALSE;
}

// The taskbar's XAML host window: a Windows.UI.Core.CoreWindow that is a direct child of Shell_TrayWnd.
// We enumerate children + compare class names manually (FindWindowEx by this class name does NOT match
// it reliably). Returns null until the host exists - which, at a cold boot, is a couple seconds after
// the taskbar window itself appears.
static HWND FindTaskbarXamlHost() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) {
        return nullptr;
    }
    for (HWND child = GetWindow(tray, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[80] = {};
        GetClassNameW(child, cls, 80);
        if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
            return child;
        }
    }
    return nullptr;
}

static DWORD WINAPI HookThread(LPVOID) {
    {
        char s[80];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "--- OledCareHook: starting (up=%llums) ---",
                    static_cast<unsigned long long>(GetTickCount64()));
        LogA(s);
    }
    g_disabledEvent = CreateEventW(nullptr, TRUE, FALSE, contract::kEventDisabled);    // shared with the app
    g_peekEvent = CreateEventW(nullptr, TRUE, FALSE, contract::kEventPeek);            // reveal-hotkey peek
    g_hideAllEvent = CreateEventW(nullptr, TRUE, FALSE, contract::kEventHideAll);      // hide secondary taskbars
    g_unloadEvent = CreateEventW(nullptr, TRUE, FALSE, contract::kEventUnload);        // uninstaller asks us to unload
    g_connectedEvent = CreateEventW(nullptr, TRUE, FALSE, contract::kEventConnected);  // WE set it once advising
    // The event is named + manual-reset, so a previous uninstall's "set" persists (explorer leaks
    // the handle on unload). Clear it now so this freshly injected hook doesn't instantly unload
    // itself on the first timer tick - that was breaking install-after-uninstall.
    if (g_unloadEvent != nullptr) {
        ResetEvent(g_unloadEvent);
    }
    if (g_connectedEvent != nullptr) {
        ResetEvent(g_connectedEvent);  // clear any stale "set" from a prior hook so the app's watchdog stays accurate
    }

    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xaml == nullptr) {
        LogA("ERROR: could not load Windows.UI.Xaml.dll");
        return 0;
    }
    typedef HRESULT(WINAPI * PFN_Init)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    PFN_Init ixde = reinterpret_cast<PFN_Init>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    if (ixde == nullptr) {
        LogA("ERROR: InitializeXamlDiagnosticsEx not found");
        return 0;
    }

    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);
    const DWORD pid = GetCurrentProcessId();

    // XAML Diagnostics often isn't ready immediately (returns 0x80070490 ERROR_NOT_FOUND),
    // and it can only be initialized once per thread - so retry on a FRESH thread each
    // attempt. The endpoint name MUST be "VisualDiagConnection<N>" (the well-known name
    // used by VS's Live Visual Tree); any other name -> ERROR_NOT_FOUND.
    //
    // Because the host app now maps us in via a WH_CALLWNDPROC hook on the taskbar's UI thread, by
    // the time this runs that thread's message loop and XAML island are already alive, so diagnostics
    // normally connects on the first attempt even at a cold boot. We still retry up to ~30s in case
    // the XAML island lags its thread slightly.
    //
    // EVENT-DRIVEN CONNECT GATE. At boot Explorer builds the taskbar's XAML tree, then rebuilds it
    // shortly after the taskbar window first appears. Our one connection's advise replays the tree
    // exactly ONCE, and we can neither re-initialize XAML diagnostics (a 2nd InitializeXamlDiagnosticsEx
    // returns 0x80070490) nor re-advise the existing connection (that crashes explorer) - both tried.
    // So connecting too early binds us to the tree Explorer is about to discard; the rebuilt, visible
    // tree never streams to us and the tray/taskbar stay normal (myPid==trayPid yet trayFrameOp=0 on a
    // frame nobody renders). The real signal, from boot child-window traces: the taskbar's XAML host -
    // a Windows.UI.Core.CoreWindow child of Shell_TrayWnd - does NOT exist when we load at a cold boot;
    // it appears ~2s later and then holds the same handle permanently. So instead of a fixed delay we
    // gate the single connect on that window being present AND stable: poll Shell_TrayWnd for a
    // CoreWindow child, and once its handle holds steady for ~2s (skipping any short-lived transient
    // Explorer spins up first), connect. Warm injects (install / app restart / explorer restart) already
    // have a settled host, so the first check passes and we connect immediately. This tracks the shell's
    // actual readiness instead of guessing a duration.
    if (FindTaskbarXamlHost() == nullptr) {
        LogA("waiting for taskbar XAML host (CoreWindow) to appear + settle");
        HWND last = nullptr;
        int stable = 0;
        for (int i = 0; i < 200 && stable < 8; ++i) {  // 250ms poll; 8 identical = ~2s settled; ~50s cap
            Sleep(250);
            HWND h = FindTaskbarXamlHost();
            if (h != nullptr && h == last) {
                ++stable;
            } else {
                last = h;
                stable = (h != nullptr) ? 1 : 0;
            }
        }
        char s[96];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "taskbar XAML host settled=%p (up=%llums)",
                    reinterpret_cast<void*>(last), static_cast<unsigned long long>(GetTickCount64()));
        LogA(s);
    }
    const int kMaxAttempts = 60;   // 60 x 500ms = ~30s
    int endpointN = 0;             // monotonic across the initial connect
    HRESULT hr = E_FAIL;
    bool connected = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        wchar_t conn[64];
        _snwprintf_s(conn, ARRAYSIZE(conn), _TRUNCATE, L"VisualDiagConnection%d", ++endpointN);
        HRESULT attemptHr = E_FAIL;
        std::thread([&]() {
            attemptHr = ixde(conn, pid, nullptr, dllPath, contract::kTapClsid, nullptr);
        }).join();
        hr = attemptHr;
        if (SUCCEEDED(hr)) {
            char line[128];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "InitializeXamlDiagnosticsEx OK on attempt %d (up=%llums)", attempt,
                        static_cast<unsigned long long>(GetTickCount64()));
            LogA(line);
            connected = true;
            break;
        }
        Sleep(500);
    }
    if (!connected) {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "InitializeXamlDiagnosticsEx FAILED after retries, last hr=0x%08X",
                    static_cast<unsigned>(hr));
        LogA(line);
        return 0;
    }

    // Connected. The single advise streamed whatever tree was current at connect time; we hold that
    // one connection for the life of the hook (XAML diagnostics can't be re-initialized, and
    // re-advising from a worker thread crashes explorer - both were tried). The cold-boot "wrong tree"
    // problem is solved earlier, by delaying this connect until the taskbar settles (see above).
    return 0;
}

// Exported so the host app can install us as a WH_CALLWNDPROC hook on the taskbar's UI thread. The
// proc body does nothing on purpose - explorer MAPPING the DLL in (to be able to call it) is the
// whole point: that load happens on the taskbar thread with its message loop and XAML island already
// alive, which is exactly what lets XAML diagnostics connect. CreateRemoteThread loaded us too early
// at boot (before the XAML existed), which is why diagnostics never connected on a cold boot.
extern "C" __declspec(dllexport) LRESULT CALLBACK OledCareCallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// True only when this DLL is running inside explorer.exe (where we do our work). In the host app -
// which loads us merely to resolve OledCareCallWndProc for SetWindowsHookEx - this returns false so
// we don't spin up the XAML connection in the wrong process.
static bool RunningInsideExplorer() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return false;
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    name = (name != nullptr) ? name + 1 : path;
    return _wcsicmp(name, L"explorer.exe") == 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        if (RunningInsideExplorer()) {
            // Guards the TAP registry (g_allTaps), populated by advise threads + drained on unload.
            InitializeCriticalSection(&g_tapsCs);
            g_tapsCsInit = true;
            // Take an extra reference on ourselves. The host app's WH_CALLWNDPROC hook is only a
            // vehicle to get us loaded; the instant it removes the hook, explorer would otherwise
            // unload us - and if that happens before XAML diagnostics has pinned us, our still-live
            // timer / visual-tree callbacks fire into freed code and crash explorer
            // ("OledCareHook.dll_unloaded"). This reference keeps us mapped until the clean self-unload
            // path (which KillTimer/UnadviseVisualTreeChange first) drops it via its matching FreeLibrary.
            HMODULE selfRef = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&DllMain), &selfRef);
            HANDLE t = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
            if (t != nullptr) {
                CloseHandle(t);
            }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (RunningInsideExplorer()) {
            LogA("DLL_PROCESS_DETACH (hook DLL unloading from explorer)");
        }
    }
    return TRUE;
}
