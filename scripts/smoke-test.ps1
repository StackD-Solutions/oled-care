# smoke-test.ps1 - the integration tripwire for the load-bearing inject -> hide -> settings path.
#
# Builds Release, restarts Explorer, launches OledCare, and asserts the hook actually connects into
# Explorer, drives the tray, the app<->hook event channel exists, and the settings window opens.
# This is the inject-and-eyeball loop we used to run by hand, codified so a regression fails loudly.
#
# NOTE: this RESTARTS Explorer (briefly blanks the taskbar) and needs a real interactive desktop, so
# it is a LOCAL pre-ship check, not a CI step (CI runs the unit tests, which need no desktop).
#
# Usage:  pwsh -File scripts/smoke-test.ps1            (or: powershell -File ...)
# Exit code 0 = PASS, 1 = FAIL.

[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [int]$ConnectTimeoutSec = 60
)
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$exe   = Join-Path $build "$Config\OledCare.exe"
$log   = Join-Path $env:TEMP 'OledCareHook.log'

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

$script:fails = 0
function Assert([string]$name, [bool]$ok) {
    if ($ok) { Write-Host "  PASS  $name" -ForegroundColor Green }
    else     { Write-Host "  FAIL  $name" -ForegroundColor Red; $script:fails++ }
}

# P/Invoke helpers: find a process's top-level window by class, and post a message to it.
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Win {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr FindByClass(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h,p)=>{ uint wp; GetWindowThreadProcessId(h, out wp); if (wp==pid) { var sb=new StringBuilder(128); GetClassNameW(h,sb,128); if (sb.ToString()==cls) { found=h; return false; } } return true; }, IntPtr.Zero);
    return found;
  }
}
'@

Write-Host "== build ($Config) =="
& $cmake --build $build --config $Config | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit 1 }
Assert "Release build" ((Test-Path $exe))

Write-Host "== restart Explorer + launch OledCare =="
Get-Process OledCare -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 8
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer; Start-Sleep -Seconds 5 }

$logStart = (Get-Content $log -ErrorAction SilentlyContinue).Count
Start-Process $exe
$deadline = (Get-Date).AddSeconds($ConnectTimeoutSec)
while ((Get-Date) -lt $deadline) {
    $c = Get-Content $log -ErrorAction SilentlyContinue
    if (($c.Count -gt $logStart) -and ($c[$logStart..($c.Count-1)] -match 'AdviseVisualTreeChange hr=0x00000000')) { break }
    Start-Sleep -Seconds 2
}
$c = Get-Content $log -ErrorAction SilentlyContinue
$sess = if ($c.Count -gt $logStart) { $c[$logStart..($c.Count-1)] } else { @() }

Write-Host "== assertions =="
$proc = Get-Process OledCare -ErrorAction SilentlyContinue
Assert "OledCare launched (no startup crash)" ([bool]$proc)
$hookLoaded = $false
if ($proc) { $hookLoaded = [bool]((Get-Process explorer -ErrorAction SilentlyContinue).Modules | Where-Object { $_.ModuleName -eq 'OledCareHook.dll' }) }
Assert "hook injected into Explorer" $hookLoaded
Assert "XAML diagnostics connected" ([bool]($sess -match 'InitializeXamlDiagnosticsEx OK'))
Assert "advise succeeded (driving live tree)" ([bool]($sess -match 'AdviseVisualTreeChange hr=0x00000000'))
Assert "SystemTrayFrame found + hidden" ([bool]($sess -match 'SystemTrayFrame #1'))

$eventsOk = $true
foreach ($n in 'OledCareDisabled','OledCarePeek','OledCareHideAll','OledCareConnected') {
    try { $e=[System.Threading.EventWaitHandle]::OpenExisting($n); $e.Close() } catch { $eventsOk = $false }
}
Assert "app<->hook event channel exists" $eventsOk

$settingsOk = $false
if ($proc) {
    $main = [Win]::FindByClass([uint32]$proc.Id, "OledCareHiddenWnd")
    if ($main -ne [IntPtr]::Zero) {
        [void][Win]::PostMessageW($main, 0x0111, [IntPtr]1001, [IntPtr]0)   # WM_COMMAND, ID_SETTINGS
        Start-Sleep -Seconds 3
        $settingsOk = (([Win]::FindByClass([uint32]$proc.Id, "OledCareSettingsWnd")) -ne [IntPtr]::Zero) -and [bool](Get-Process OledCare -ErrorAction SilentlyContinue)
    }
}
Assert "settings window opens (no crash)" $settingsOk

Get-Process OledCare -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host ""
if ($script:fails -eq 0) { Write-Host "SMOKE: PASS" -ForegroundColor Green; exit 0 }
else { Write-Host "SMOKE: FAIL ($($script:fails) assertion(s))" -ForegroundColor Red; exit 1 }
