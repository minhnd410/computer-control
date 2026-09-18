// SPDX-License-Identifier: MIT
//
// Linux has no permission dialogs either. What it has is file permissions on
// /dev/uinput, whether an X display is reachable, and whether the session is
// Wayland - where XTest reaches only XWayland clients. Those are the three
// things that decide whether this library works, so they are reported in the
// same shape as macOS's TCC grants.

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cc/permissions.hpp"

namespace cc {
namespace {

bool running_wayland() {
    if (const char* t = std::getenv("XDG_SESSION_TYPE")) {
        if (std::strcmp(t, "wayland") == 0) return true;
    }
    return std::getenv("WAYLAND_DISPLAY") != nullptr;
}

bool uinput_writable() {
    return ::access("/dev/uinput", W_OK) == 0;
}
bool uinput_exists() {
    return ::access("/dev/uinput", F_OK) == 0;
}
bool have_display() {
    return std::getenv("DISPLAY") != nullptr;
}

const char* kUinputRemedy =
    "Grant write access to /dev/uinput:\n"
    "    sudo modprobe uinput\n"
    "    sudo groupadd -f uinput\n"
    "    sudo usermod -aG uinput \"$USER\"\n"
    "    echo 'KERNEL==\"uinput\", GROUP=\"uinput\", MODE=\"0660\", "
    "OPTIONS+=\"static_node=uinput\"' | sudo tee /etc/udev/rules.d/99-uinput.rules\n"
    "    sudo udevadm control --reload-rules && sudo udevadm trigger\n"
    "Then log out and back in. Without it, gestures fall back to scroll and click "
    "emulation.";

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
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return buf;
}

std::string bundle_path() {
    return {};
}
bool has_own_tcc_identity() {
    return true;
}  // no TCC on Linux
std::string permission_owner() {
    return {};
}  // no responsible-process concept

PermissionStatus check_permission(Permission p) {
    PermissionStatus out;
    out.permission = p;

    switch (p) {
        case Permission::Accessibility:
            out.affects = {"snapshot", "elements", "wait_for"};
            // The AT-SPI backend loads libatspi at runtime, so the honest
            // answer here is about the session, not a grant.
            if (!have_display() && !running_wayland()) {
                out.state = PermissionState::Denied;
                out.detail = "no display: DISPLAY is unset and this is not a Wayland session";
                out.remedy =
                    "Set DISPLAY (for example :0), or start a virtual display with "
                    "`xvfb-run -a`.";
                return out;
            }
            out.state = PermissionState::Granted;
            out.detail =
                "no OS permission is required; the accessibility tree needs AT-SPI2 to be "
                "installed and running, which `computer-control-mcp --doctor` reports "
                "separately";
            return out;

        case Permission::ScreenRecording:
            out.affects = {"screenshot", "snapshot", "zoom"};
            if (running_wayland()) {
                out.state = PermissionState::Degraded;
                out.detail =
                    "Wayland session: X11 capture sees only XWayland clients, not native "
                    "Wayland windows";
                out.remedy =
                    "Run the session under X11, or capture through xdg-desktop-portal's "
                    "ScreenCast interface, which prompts the user per session.";
                return out;
            }
            if (!have_display()) {
                out.state = PermissionState::Denied;
                out.detail = "DISPLAY is unset, so there is no screen to capture";
                out.remedy = "Set DISPLAY, or run under `xvfb-run -a`.";
                return out;
            }
            out.state = PermissionState::Granted;
            out.detail = "X11 capture is available";
            return out;

        case Permission::InputMonitoring:
            out.state = PermissionState::NotRequired;
            out.detail = "no equivalent on Linux";
            return out;

        case Permission::TouchInjection:
            out.affects = {"gesture (pinch, rotate, multi-finger swipe and pan)"};
            if (uinput_writable()) {
                out.state = PermissionState::Granted;
                out.detail =
                    "/dev/uinput is writable: gestures use a real virtual multitouch device, "
                    "which works on Wayland as well as X11";
                return out;
            }
            out.state = PermissionState::Denied;
            out.detail = uinput_exists()
                             ? "/dev/uinput exists but is not writable by this user"
                             : "/dev/uinput does not exist (the uinput module is not loaded)";
            out.remedy = kUinputRemedy;
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
    return err(ErrorCode::Unsupported, "Linux has no settings page for these",
               "They are file permissions and session type; see the remedy on each status.");
}

PermissionStatus request_permission(Permission p) {
    // Nothing to prompt for: every one of these needs a shell command or a
    // different session, not a dialog.
    return check_permission(p);
}

}  // namespace cc
