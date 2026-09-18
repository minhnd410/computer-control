// SPDX-License-Identifier: MIT
//
// Windows has no permission dialogs for any of this. What it has instead is
// UIPI, an integrity-level boundary that silently discards synthetic input
// aimed at a window running higher than this process - which looks exactly
// like a coordinate bug. Reporting the elevation state up front is the useful
// equivalent of a permission check.

#include <windows.h>

#include <string>
#include <vector>

#include "cc/permissions.hpp"

namespace cc {
namespace {

bool process_is_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL okay = ::GetTokenInformation(token, TokenElevation, &elevation,
                                            static_cast<DWORD>(sizeof(elevation)), &returned);
    ::CloseHandle(token);
    // TokenIsElevated is a DWORD, so compare rather than relying on an
    // implicit narrowing conversion that /W4 warns about.
    return okay != FALSE && elevation.TokenIsElevated != 0;
}

bool touch_injection_available() {
    // Initializing twice in one process fails, so this is only a probe of
    // whether the API exists at all on this Windows version.
    static const bool available = [] {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 && ::GetProcAddress(user32, "InitializeTouchInjection") != nullptr;
    }();
    return available;
}

std::string current_executable() {
    // MAX_PATH is not the real limit on modern Windows; grow until the call
    // stops truncating, which it signals by returning the buffer size.
    std::vector<wchar_t> path(MAX_PATH);
    DWORD n = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) break;
        path.resize(path.size() * 2);
        n = 0;
    }
    if (n == 0) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(n), nullptr, 0,
                                           nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(n), out.data(), size, nullptr,
                          nullptr);
    return out;
}

}  // namespace

const char* to_string(Permission p) noexcept {
    switch (p) {
        case Permission::Accessibility: return "accessibility";
        case Permission::ScreenRecording: return "screen_recording";
        case Permission::InputMonitoring: return "input_monitoring";
        case Permission::TouchInjection: return "touch_injection";
    }
    return "unknown";
}

const char* to_string(PermissionState s) noexcept {
    switch (s) {
        case PermissionState::Granted: return "granted";
        case PermissionState::Degraded: return "degraded";
        case PermissionState::Denied: return "denied";
        case PermissionState::NotDetermined: return "not_determined";
        case PermissionState::NotRequired: return "not_required";
    }
    return "unknown";
}

std::string executable_path() {
    return current_executable();
}
std::string bundle_path() {
    return {};
}
bool has_own_tcc_identity() {
    return true;
}  // no TCC on Windows
std::string permission_owner() {
    return {};
}  // no responsible-process concept

PermissionStatus check_permission(Permission p) {
    PermissionStatus out;
    out.permission = p;

    switch (p) {
        case Permission::Accessibility:
            out.affects = {"snapshot", "elements", "input into elevated windows"};
            if (process_is_elevated()) {
                out.state = PermissionState::Granted;
                out.detail = "running elevated: UI Automation and input reach every window";
                return out;
            }
            // Not elevated is the normal, correct way to run. It is only a
            // problem for elevated targets, so this is Granted with a caveat
            // rather than Degraded - most automation never touches one.
            out.state = PermissionState::Granted;
            out.detail =
                "running unelevated, which is fine for ordinary applications. Input and UI "
                "Automation aimed at a window running at a higher integrity level (an "
                "installer, Task Manager, an app launched as administrator) will be silently "
                "discarded by UIPI.";
            out.remedy =
                "If a click into an elevated app appears to do nothing, restart "
                "computer-control elevated.";
            return out;

        case Permission::ScreenRecording:
            out.affects = {"screenshot", "snapshot", "zoom"};
            out.state = PermissionState::Granted;
            out.detail =
                "Windows requires no permission to capture the screen. Capture still fails on "
                "the secure desktop (a UAC prompt or the lock screen) and returns black for "
                "windows using SetWindowDisplayAffinity.";
            return out;

        case Permission::InputMonitoring:
            out.state = PermissionState::NotRequired;
            out.detail = "no equivalent on Windows";
            return out;

        case Permission::TouchInjection:
            out.affects = {"gesture"};
            if (touch_injection_available()) {
                out.state = PermissionState::Granted;
                out.detail = "InjectTouchInput is available: gestures use real touch contacts";
            } else {
                out.state = PermissionState::Denied;
                out.detail = "InjectTouchInput is unavailable on this Windows version";
                out.remedy =
                    "Touch injection needs Windows 8 or later and an interactive session. "
                    "Gestures fall back to scroll and click emulation; "
                    "`computer-control-mcp --doctor` "
                    "reports which.";
            }
            return out;
    }
    return out;
}

std::vector<PermissionStatus> check_permissions() {
    return {
        check_permission(Permission::Accessibility),
        check_permission(Permission::ScreenRecording),
        check_permission(Permission::InputMonitoring),
        check_permission(Permission::TouchInjection),
    };
}

Status open_permission_settings(Permission) {
    return err(ErrorCode::Unsupported, "Windows has no permission page for desktop automation",
               "The only relevant control is whether this process runs elevated.");
}

PermissionStatus request_permission(Permission p) {
    // Nothing to prompt for; report the state so callers can treat every
    // platform the same way.
    return check_permission(p);
}

}  // namespace cc
