<!--suppress HtmlDeprecatedAttribute -->
<h1 align="center">
  <br>
  <a href="https://www.stackd-solutions.io"><img src="https://raw.githubusercontent.com/StackD-Solutions/oled-care/main/docs/logo.svg" alt="StackD Solutions" width="200"></a>
  <br>OledCare
  <br>
</h1>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%2011-0078D6?logo=windows&logoColor=white" alt="Windows 11">
  <img src="https://img.shields.io/badge/version-1.0.0-blue" alt="Version 1.0.0">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/badge/build-CMake-064F8C?logo=cmake&logoColor=white" alt="CMake">
</p>

OledCare is a small Windows 11 utility for OLED display care: its goal is to reduce the risk of **burn-in and image retention**. OLED pixels age as they light up, so the bright, never-moving elements a desktop keeps on screen all day wear unevenly — and over time that shows up as a lingering after-image or, eventually, a permanent ghost. OledCare exists to slow that down and keep your panel aging evenly, so it stays cleaner and lasts longer.

It ships as a single self-contained installer and runs quietly in the background — no account, no telemetry, no background web service.

## Features

- **System tray hiding** — the clock and the always-on status icons sit in one fixed, bright spot indefinitely, which makes them prime burn-in candidates. OledCare keeps them hidden and reveals them on hover (move the cursor to the right edge of the taskbar), so they're only lit when you reach for them.
- **Clear taskbar** — make the taskbar background fully transparent, so the bright bar fixed along the screen edge isn't continuously lit into the panel — without changing any Windows appearance settings.

_More OLED-care features are planned for the near future._

## Requirements

- **Windows 11.** OledCare integrates with the Windows 11 XAML taskbar; Windows 10 and earlier are not supported.
- No runtime dependencies — the binaries are statically linked, so no Visual C++ redistributable is required.

## Installation

Download `OledCareSetup.exe` from the [latest release](https://github.com/StackD-Solutions/oled-care/releases) and run it. The installer:

- installs to `Program Files\StackD Solutions\OledCare`
- adds **OledCare** and **Uninstall OledCare** shortcuts to the Start Menu
- registers a startup entry so OledCare runs at sign-in (or, if you choose **High priority**, installs a Windows service that starts it earlier at boot)
- adds an entry to **Apps & features** for a clean uninstall

OledCare starts hidden in the system tray with tray-hiding **on** by default. Right-click the tray icon for **Settings**, **About**, and **Exit**.

## Usage

| Action | How |
| ------ | --- |
| Reveal the tray | Move the cursor to the **right edge of the taskbar** |
| Peek the tray on demand | Press the **peek hotkey** (default `Ctrl + Alt + F10`) |
| Open settings | Right-click the tray icon → **Settings** |
| Turn hiding off temporarily | Settings → uncheck **Hide system tray** |
| Quit | Right-click the tray icon → **Exit** (restores the tray) |

## Settings

All settings live in the Settings window and persist to `HKCU\Software\OledCare`.

| Setting | Options | Description |
| ------- | ------- | ----------- |
| **Hide system tray** | on / off | Master switch for hover-to-reveal hiding |
| **Target** (tray) | All monitors / Monitor N | Which display the tray hiding applies to |
| **Fade in / Fade out** | Instant / Fast / Normal / Slow | Reveal and hide animation speed |
| **Peek hotkey** | any key combo | Global shortcut that reveals the tray on demand |
| **Peek duration** | 2 / 3 / 5 / 10 s | How long the peek stays open before auto-hiding (clamped 1–60 s) |
| **Color** (taskbar) | Normal / Clear | Leave the taskbar as-is, or make its background transparent |
| **Target** (taskbar) | All monitors / Monitor N | Which display the taskbar color applies to |
| **Start with Windows** | on / off | Launch at sign-in via the startup list |
| **Set high priority** | on / off | Install a service that starts OledCare earlier at boot (requires elevation) |
| **Protect against crashes** | on / off | Let the high-priority service relaunch OledCare automatically |
| **Safe start after hibernation** | on / off | Re-apply tray hiding after the PC resumes from sleep |
| **Theme** | Dark / Light / System | Appearance of OledCare's own windows |

## How it works

OledCare reveals the tray by setting the opacity of the taskbar's XAML elements. It loads a small hook into Explorer's UI thread, registers as a XAML Diagnostics provider to read the live visual tree, and drives the hide/reveal from there. The injector and hook are OledCare's own — there are no third-party runtime dependencies.

It is built from three binaries:

| Binary | Role |
| ------ | ---- |
| `OledCare.exe` | The tray app + injector — owns the UI, settings, hotkey, and lifecycle |
| `OledCareHook.dll` | The payload injected into `explorer.exe` that reads the live XAML tree and drives the hide/reveal |
| `OledCareSetup.exe` | The self-contained dark-themed installer (embeds the app + hook) |

The hook runs **inside Explorer**, so the project treats it as the blast-radius it is: the connection is gated on the taskbar's XAML host being present and settled (cold-boot safe), and a crash-loop watchdog stops re-injecting if Explorer restarts repeatedly right after injection.

## Building from source

Requires **CMake 3.20+** and the **Visual Studio 2022 Build Tools** (MSVC, C++ workload).

```bash
cmake -S . -B build
cmake --build build --config Release
```

Outputs to `build/Release/`:

- `OledCare.exe` — the tray app
- `OledCareHook.dll` — the injected hook
- `OledCareSetup.exe` — the installer (rebuilt to embed the fresh app + hook)

## Testing

Unit tests cover the pure settings logic (clamps, hotkey formatting) and run with no desktop, so they fit CI:

```bash
ctest --test-dir build -C Release --output-on-failure
```

An integration smoke test exercises the real inject → hide → settings path. It restarts Explorer and needs an interactive desktop, so it's a local pre-ship check rather than a CI step:

```powershell
powershell -File scripts/smoke-test.ps1
```

## Uninstalling

Uninstall from **Settings → Apps → Installed apps → OledCare** or the Start Menu **Uninstall OledCare** shortcut. Uninstalling restores the taskbar, removes the startup entry / high-priority service, and detaches the hook from Explorer without a shell restart.
