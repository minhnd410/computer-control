// SPDX-License-Identifier: MIT
//
// macOS shell actions.
//
// Version detection is load-bearing here. macOS 26 (Tahoe) removed
// Launchpad.app entirely and folded the all-applications view into Spotlight,
// so `open -a Launchpad` - the instruction in every guide written before 2026
// - fails with "Unable to find application named 'Launchpad'". The launcher is
// now cmd+space followed by cmd+1.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <thread>

#include "cc/system_ui.hpp"

namespace cc {
namespace {

// True when Launchpad.app is still installed. Checking the file rather than
// the version alone keeps this correct if Apple brings it back, or on a
// machine where a user restored it.
bool launchpad_exists() {
    @autoreleasepool {
        static const bool present = [] {
            for (NSString* dir in @[ @"/System/Applications", @"/Applications" ]) {
                NSString* path = [dir stringByAppendingPathComponent:@"Launchpad.app"];
                if ([NSFileManager.defaultManager fileExistsAtPath:path]) return true;
            }
            return false;
        }();
        return present;
    }
}

Status tap(InputBackend& input, const char* chord) {
    auto parsed = parse_chord(chord);
    if (!parsed) return parsed.error();
    return input.tap_chord(parsed.value(), 1);
}

Status open_app(SystemBackend* system, const char* name) {
    if (!system) {
        return err(ErrorCode::Unsupported, "this action needs the system backend");
    }
    ShellRequest req;
    req.command = std::string("open -a ") + "\"" + name + "\"";
    req.timeout = std::chrono::milliseconds{8000};
    auto r = system->run_shell(req);
    if (!r) return r.error();
    if (r.value().exit_code != 0) {
        return err(ErrorCode::NotFound,
                   std::string("could not open ") + name + ": " + r.value().stderr_text);
    }
    return ok();
}

}  // namespace

SystemActionInfo describe_system_action(SystemAction a) {
    SystemActionInfo info;
    info.action = a;
    info.supported = true;

    switch (a) {
        case SystemAction::Search: info.mechanism = "cmd+space"; return info;

        case SystemAction::Launcher:
            if (launchpad_exists()) {
                info.mechanism = "open -a Launchpad";
            } else {
                info.mechanism = "cmd+space then cmd+1";
                info.note =
                    "macOS 26 removed Launchpad and folded the all-applications view into "
                    "Spotlight, so this opens Spotlight and switches to its Applications mode.";
            }
            return info;

        case SystemAction::AppSwitcher:
            info.mechanism = "cmd+tab";
            info.note =
                "The switcher stays up only while cmd is held, so this shows it and moves one "
                "step. Use key_down/key_up to hold it open.";
            return info;

        case SystemAction::Overview:
            info.mechanism = "ctrl+up";
            info.note = "Mission Control.";
            return info;

        case SystemAction::ShowDesktop:
            info.mechanism = "fn+F11";
            info.note = "Requires 'Show Desktop' to be bound in System Settings > Desktop & Dock > "
                        "Shortcuts; it is on by default but users do remap it.";
            return info;

        case SystemAction::NextDesktop: info.mechanism = "ctrl+right"; return info;

        case SystemAction::PreviousDesktop: info.mechanism = "ctrl+left"; return info;

        case SystemAction::Notifications:
            info.mechanism = "open -a 'Notification Centre'";
            info.supported = false;
            info.note =
                "macOS has no shortcut or URL for Notification Centre unless the user assigns "
                "one. Click the clock in the menu bar instead: its coordinates come from a "
                "snapshot.";
            return info;

        case SystemAction::ScreenshotUi: info.mechanism = "cmd+shift+5"; return info;

        case SystemAction::Emoji: info.mechanism = "ctrl+cmd+space"; return info;

        case SystemAction::RunDialog:
            info.mechanism = "cmd+space";
            info.note = "macOS has no Run dialog; Spotlight is the equivalent.";
            return info;

        case SystemAction::Settings: info.mechanism = "open -a 'System Settings'"; return info;

        case SystemAction::FileManager: info.mechanism = "open -a Finder"; return info;

        case SystemAction::LockScreen:
            info.mechanism = "ctrl+cmd+q";
            info.note = "Locks immediately. There is no confirmation.";
            return info;
    }
    info.supported = false;
    return info;
}

Status perform_system_action(SystemAction a, InputBackend& input, SystemBackend* system) {
    switch (a) {
        case SystemAction::Search:
        case SystemAction::RunDialog: return tap(input, "cmd+space");

        case SystemAction::Launcher: {
            if (launchpad_exists()) return open_app(system, "Launchpad");
            // Spotlight, then its Applications mode. The pause is needed
            // because Spotlight's window has to exist before it will accept
            // the mode switch; without it the cmd+1 goes to whatever was
            // frontmost.
            if (auto st = tap(input, "cmd+space"); !st) return st;
            std::this_thread::sleep_for(std::chrono::milliseconds{450});
            return tap(input, "cmd+1");
        }

        case SystemAction::AppSwitcher: return tap(input, "cmd+tab");
        case SystemAction::Overview: return tap(input, "ctrl+up");
        case SystemAction::ShowDesktop: return tap(input, "fn+f11");
        case SystemAction::NextDesktop: return tap(input, "ctrl+right");
        case SystemAction::PreviousDesktop: return tap(input, "ctrl+left");
        case SystemAction::ScreenshotUi: return tap(input, "cmd+shift+5");
        case SystemAction::Emoji: return tap(input, "ctrl+cmd+space");
        case SystemAction::LockScreen: return tap(input, "ctrl+cmd+q");
        case SystemAction::Settings: return open_app(system, "System Settings");
        case SystemAction::FileManager: return open_app(system, "Finder");

        case SystemAction::Notifications:
            return err(ErrorCode::Unsupported, "macOS has no shortcut for Notification Centre",
                       "Take a snapshot and click the clock in the menu bar, or assign a "
                       "shortcut under System Settings > Keyboard > Keyboard Shortcuts.");
    }
    return err(ErrorCode::InvalidArgument, "unknown system action");
}

}  // namespace cc
