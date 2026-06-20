// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

// service.cpp - the "high priority" autostart for OledCare.
//
// Runs as a LocalSystem Windows service (session 0). Services start earlier than the
// throttled HKCU\Run autostart, so this gets OledCare going sooner. A session-0 service
// can't show UI or inject into explorer itself, so its only job is to launch OledCare.exe
// into the active *user* session and relaunch it if it crashes.
//
// Technique (standard Microsoft-documented pattern):
//   WTSGetActiveConsoleSessionId -> WTSQueryUserToken -> DuplicateTokenEx ->
//   CreateEnvironmentBlock -> CreateProcessAsUser

#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <string>
#include "service.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

const wchar_t* kServiceName = L"OledCareService";
const wchar_t* kServiceDisplay = L"OledCare Service";
const wchar_t* kRegPath = L"SOFTWARE\\OledCare";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stopEvent = nullptr;

std::wstring SelfPath() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

void ReportStatus(DWORD state, DWORD wait = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted =
        (state == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    g_status.dwWaitHint = wait;
    SetServiceStatus(g_statusHandle, &g_status);
}

// Launch OledCare.exe (tray mode, no args) in the active interactive session. Returns the
// new process handle (caller waits on it) or null on failure / nobody logged in.
HANDLE LaunchInUserSession() {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        return nullptr;
    }
    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        return nullptr;
    }
    HANDLE primary = nullptr;
    HANDLE process = nullptr;
    if (DuplicateTokenEx(userToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
        void* env = nullptr;
        CreateEnvironmentBlock(&env, primary, FALSE);
        std::wstring path = SelfPath();
        std::wstring cmd = L"\"" + path + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION pi = {};
        if (CreateProcessAsUserW(primary, path.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                                 CREATE_UNICODE_ENVIRONMENT, env, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            process = pi.hProcess;
        }
        if (env != nullptr) {
            DestroyEnvironmentBlock(env);
        }
        CloseHandle(primary);
    }
    CloseHandle(userToken);
    return process;
}

void RunLoop() {
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE proc = LaunchInUserSession();
        if (proc == nullptr) {
            // Nobody at the console yet (boot/login race), or launch failed - wait + retry.
            if (WaitForSingleObject(g_stopEvent, 2000) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        HANDLE waits[2] = { g_stopEvent, proc };
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            TerminateProcess(proc, 0);  // service stopping - take the tray app with us
            CloseHandle(proc);
            break;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(proc, &exitCode);
        CloseHandle(proc);
        if (exitCode == static_cast<DWORD>(kExitAlreadyRunning)) {
            // OledCare is already running in the session (launched by something else). The
            // service's goal is met - stay running and re-check after a delay so we relaunch
            // it if that instance later exits. This is what keeps the service "Running".
            if (WaitForSingleObject(g_stopEvent, 5000) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }
        if (exitCode == 0) {
            break;  // clean exit (user clicked Exit) - don't relaunch
        }
        if (!GetProtectAgainstCrashes()) {
            break;  // crashed, but "protect against crashes" is turned off
        }
        // Crash + protect on -> relaunch after a short delay.
        if (WaitForSingleObject(g_stopEvent, 1500) == WAIT_OBJECT_0) {
            break;
        }
    }
}

DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD, void*, void*) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        ReportStatus(SERVICE_STOP_PENDING, 3000);
        SetEvent(g_stopEvent);
    }
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandler, nullptr);
    if (g_statusHandle == nullptr) {
        return;
    }
    ReportStatus(SERVICE_START_PENDING, 3000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ReportStatus(SERVICE_RUNNING);
    RunLoop();
    ReportStatus(SERVICE_STOPPED);
}

}  // namespace

int RunServiceMain() {
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr },
    };
    StartServiceCtrlDispatcherW(table);
    return 0;
}

bool InstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (scm == nullptr) {
        return false;
    }
    std::wstring bin = L"\"" + SelfPath() + L"\" --service";
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                   bin.c_str(), nullptr, nullptr, nullptr, nullptr /*LocalSystem*/, nullptr);
    bool ok = (svc != nullptr);
    if (svc != nullptr) {
        StartServiceW(svc, 0, nullptr);  // start now; also auto-starts at boot
        CloseServiceHandle(svc);
    } else if (GetLastError() == ERROR_SERVICE_EXISTS) {
        ok = true;
    }
    CloseServiceHandle(scm);
    return ok;
}

bool UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (scm == nullptr) {
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    bool ok = false;
    if (svc != nullptr) {
        SERVICE_STATUS st = {};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        ok = DeleteService(svc) != 0;
        CloseServiceHandle(svc);
    } else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        ok = true;
    }
    CloseServiceHandle(scm);
    return ok;
}

bool IsServiceInstalled() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    bool exists = (svc != nullptr);
    if (svc != nullptr) {
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return exists;
}

bool GetProtectAgainstCrashes() {
    HKEY key;
    DWORD val = 1;  // default: protect on
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD sz = sizeof(val);
        RegQueryValueExW(key, L"ProtectAgainstCrashes", nullptr, nullptr, reinterpret_cast<BYTE*>(&val), &sz);
        RegCloseKey(key);
    }
    return val != 0;
}

void SetProtectAgainstCrashes(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        DWORD val = on ? 1 : 0;
        RegSetValueExW(key, L"ProtectAgainstCrashes", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }
}
