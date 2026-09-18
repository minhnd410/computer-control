// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cc/types.hpp"

namespace cc {

enum class Permission : std::uint8_t {
    Accessibility = 0,  // read the UI tree and synthesize input
    ScreenRecording,    // capture the screen
    InputMonitoring,    // observe input (not needed to synthesize it)
    TouchInjection,     // synthesize real touch contacts
};

const char* to_string(Permission p) noexcept;

enum class PermissionState : std::uint8_t {
    Granted = 0,
    // The OS says yes but the capability does not actually work. On macOS a
    // process launched from a granted terminal inherits AXIsProcessTrusted()
    // while still being refused real inspection, which reports as Granted and
    // behaves as Denied. Treating that as Granted is how you end up debugging
    // "this app has no UI" for an hour.
    Degraded,
    Denied,
    // Never requested. Only macOS distinguishes this, and only it can be
    // resolved by prompting.
    NotDetermined,
    // Does not exist on this platform, or is not needed for what was asked.
    NotRequired,
};

const char* to_string(PermissionState s) noexcept;

struct PermissionStatus {
    Permission permission = Permission::Accessibility;
    PermissionState state = PermissionState::NotRequired;
    // What was actually observed, in plain language.
    std::string detail;
    // The exact next step: a Settings pane, a package, a udev rule.
    std::string remedy;
    // True when request() can do something useful. False when the only way
    // forward is a manual step by the user.
    bool can_prompt = false;
    // Which features stop working without it.
    std::vector<std::string> affects;
};

// Every permission this platform cares about, probed functionally where a
// functional probe is possible.
std::vector<PermissionStatus> check_permissions();
PermissionStatus check_permission(Permission p);

// Asks the OS to prompt. Returns the state afterwards, which is usually still
// not Granted: the dialog is asynchronous and the user has to act on it, and
// on macOS most grants only take effect after a restart of this process.
PermissionStatus request_permission(Permission p);

// Opens the relevant settings page. Returns Unsupported where there is no such
// thing to open.
Status open_permission_settings(Permission p);

// Absolute path of the running executable, and the app bundle containing it if
// there is one. Both matter on macOS, where the user has to add this exact
// path to a Settings list and a bundle is what makes the grant stick.
std::string executable_path();
std::string bundle_path();

// True when this process is its own TCC identity rather than inheriting one
// from a parent. A CLI started from Terminal is not; an app bundle opened with
// `open`, or a launchd job, is.
//
// This is the single most confusing thing about macOS permissions: an
// inherited identity means the binary never appears in the Settings list, so
// there is nothing to enable, and prompting silently does nothing because the
// OS already considers the request answered.
bool has_own_tcc_identity();

// Name of the process the OS attributes this one's permissions to, when that
// is not this process. Empty when we own our identity, or on a platform with
// no such concept. Naming it is the fastest way to explain why a permission
// that looks granted does not work: the user granted their terminal, not this.
std::string permission_owner();

// One paragraph explaining the situation on this machine, suitable for showing
// a user directly. Empty when everything is granted.
std::string permission_guidance();

}  // namespace cc
