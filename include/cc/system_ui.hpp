// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cc/input.hpp"
#include "cc/system.hpp"
#include "cc/types.hpp"

namespace cc {

// Shell-level actions that every desktop has but none exposes as an API:
// open the launcher, switch virtual desktops, show the overview.
//
// These exist as a separate concept from `gesture` because synthesizing the
// gesture does not work. On Windows a four-finger trackpad swipe is
// interpreted by the Precision Touchpad driver from HID reports; touch
// injection produces *touchscreen* contacts, which go to whatever application
// is underneath and never reach the shell. macOS has no public multi-touch
// synthesis at all. In both cases the documented keyboard shortcut produces
// exactly the effect the user wanted, instantly and without the gesture
// recogniser's timing sensitivity.
//
// So this is both the reliable path and the fast one: a chord, not a 300ms
// animated contact stream that a recogniser may or may not accept.
enum class SystemAction : std::uint8_t {
    Search = 0,   // Spotlight, Windows Search, GNOME overview search
    Launcher,     // the all-applications view
    AppSwitcher,  // cmd/alt+tab
    Overview,     // Mission Control, Task View, Activities
    ShowDesktop,
    NextDesktop,
    PreviousDesktop,
    Notifications,
    ScreenshotUi,  // the interactive capture tool
    Emoji,
    RunDialog,
    Settings,
    FileManager,
    LockScreen,
};

const char* to_string(SystemAction a) noexcept;
Result<SystemAction> system_action_from_string(std::string_view name);

struct SystemActionInfo {
    SystemAction action = SystemAction::Search;
    bool supported = false;
    // What it actually does: "cmd+space", "Win+Ctrl+Right", "open -a Finder".
    // Surfaced so a caller can see the mechanism rather than guess.
    std::string mechanism;
    std::string note;
};

// What this host can do, with the mechanism for each. Depends on the OS
// version: macOS 26 replaced Launchpad with Spotlight's Applications view, so
// the launcher is a different key sequence there.
std::vector<SystemActionInfo> system_actions();
SystemActionInfo describe_system_action(SystemAction a);

// `system` may be null; only the few actions that shell out need it.
Status perform_system_action(SystemAction a, InputBackend& input, SystemBackend* system);

}  // namespace cc
