cmake_minimum_required(VERSION 3.20)
project(OledCare VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Statically link the MSVC runtime so the installer and app run on a clean PC with no
# VC++ redistributable installed. Requires policy CMP0091 (NEW by default on CMake 3.20+).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Single source of truth for product metadata + the cross-binary contract (version, event names,
# registry paths, the TAP CLSID). Generated into build/generated/AppContract.h and placed on every
# target's include path so the app, the injected hook, and the installer can never diverge.
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/AppContract.h.in" "${GENERATED_DIR}/AppContract.h" @ONLY)
# RC-safe version metadata header, from the same project(VERSION). The .rc files #include it for their
# VERSIONINFO blocks, so PE file metadata stays in lockstep with the product version.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/VersionInfo.h.in" "${GENERATED_DIR}/VersionInfo.h" @ONLY)

# Host app (OledCare.exe): tray icon, menu, and the injector.
# app.rc embeds the manifest (modern common-controls theming + DPI awareness).
add_executable(OledCare WIN32 src/main.cpp src/injector.cpp src/service.cpp src/ui/Theme.cpp src/ui/SettingsModel.cpp src/ui/components/AppIcon.cpp src/ui/components/TrayMenu.cpp src/ui/components/AboutDialog.cpp src/ui/components/StyledCombo.cpp src/ui/components/SettingsWindow.cpp src/ui/components/ScreenDimmer.cpp src/app.rc)
target_compile_definitions(OledCare PRIVATE UNICODE _UNICODE)
target_include_directories(OledCare PRIVATE src "${GENERATED_DIR}")
target_link_libraries(OledCare PRIVATE user32 shell32 gdi32 gdiplus dwmapi uxtheme msimg32)
# We supply our own manifest via app.rc, so stop the linker emitting a second one.
target_link_options(OledCare PRIVATE /MANIFEST:NO)

# Injected payload (OledCareHook.dll): runs inside explorer.exe; reads the
# XAML tree via XAML Diagnostics.
add_library(OledCareHook SHARED src/hook/dllmain.cpp src/hook/hook.rc)
target_compile_definitions(OledCareHook PRIVATE UNICODE _UNICODE)
target_include_directories(OledCareHook PRIVATE "${GENERATED_DIR}")
target_link_libraries(OledCareHook PRIVATE user32 ole32 oleaut32)

# Install wizard (OledCareSetup.exe): custom dark-themed installer UI.
# setup.rc embeds the manifest (common controls v6 + PerMonitorV2 DPI), the app icon,
# and the payload (OledCare.exe + OledCareHook.dll) as RCDATA so the installer is a
# single self-contained file.
add_executable(OledCareSetup WIN32 src/setup/main.cpp src/setup/ui/Theme.cpp src/setup/ui/Wizard.cpp src/setup/ui/components/DarkButton.cpp src/setup/ui/components/InstallerLogo.cpp src/setup/ui/components/FolderBrowser.cpp src/setup/ui/components/WizardChrome.cpp src/ui/components/AppIcon.cpp src/setup/setup.rc)
target_compile_definitions(OledCareSetup PRIVATE UNICODE _UNICODE)
target_include_directories(OledCareSetup PRIVATE src "${GENERATED_DIR}")
target_link_libraries(OledCareSetup PRIVATE user32 gdi32 dwmapi comctl32 ole32 shell32 advapi32)
target_link_options(OledCareSetup PRIVATE /MANIFEST:NO)

# Stage the freshly built app + hook into a payload dir that setup.rc pulls in via its
# resource-compiler include path. The PRE_BUILD step runs before the resource compile,
# and add_dependencies guarantees the binaries exist first.
set(SETUP_PAYLOAD_DIR "${CMAKE_BINARY_DIR}/setup_payload")
add_dependencies(OledCareSetup OledCare OledCareHook)
add_custom_command(TARGET OledCareSetup PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SETUP_PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:OledCare>" "${SETUP_PAYLOAD_DIR}/OledCare.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:OledCareHook>" "${SETUP_PAYLOAD_DIR}/OledCareHook.dll"
    COMMENT "Staging installer payload (OledCare.exe + OledCareHook.dll)")
target_include_directories(OledCareSetup PRIVATE "${SETUP_PAYLOAD_DIR}")

# Unit-test tripwire (pure settings logic). A console exe registered with CTest; CI runs `ctest`.
# Needs no desktop - unlike the integration smoke test (scripts/smoke-test.ps1), which injects into
# Explorer and is a local pre-ship check.
enable_testing()
add_executable(OledCareTests __tests__/tests.cpp)
target_compile_definitions(OledCareTests PRIVATE UNICODE _UNICODE)
target_include_directories(OledCareTests PRIVATE src __tests__)
target_link_libraries(OledCareTests PRIVATE user32)
add_test(NAME unit COMMAND OledCareTests)
