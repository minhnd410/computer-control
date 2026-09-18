// SPDX-License-Identifier: MIT
//
// Windows shell actions.
//
// This is the reliable answer to "make a four-finger trackpad swipe switch
// desktops". It cannot be done by synthesizing the gesture: Windows interprets
// Precision Touchpad gestures from HID reports inside the touchpad driver
// stack, and InjectTouchInput produces *touchscreen* contacts, which are
// routed to whichever window is underneath and never reach the shell. The
// shortcuts below are what the touchpad driver itself ends up invoking, so
// they produce the identical effect - and instantly, with no animation or
// recogniser timing to lose a gesture to.

#include <windows.h>

#include <chrono>
#include <string>

#include "cc/system_ui.hpp"

namespace cc {
namespace {

Status tap(InputBackend& input, const char* chord) {
    auto parsed = parse_chord(chord);
    if (!parsed) return parsed.error();
    return input.tap_chord(parsed.value(), 1);
}

}  // namespace

SystemActionInfo describe_system_action(SystemAction a) {
    SystemActionInfo info;
    info.action = a;
    info.supported = true;

    switch (a) {
        case SystemAction::Search: info.mechanism = "win+s"; return info;
        case SystemAction::Launcher:
            info.mechanism = "win";
            info.note = "Opens the Start menu; press again or Escape to dismiss.";
            return info;
        case SystemAction::AppSwitcher:
            info.mechanism = "alt+tab";
            info.note =
                "The switcher stays up only while alt is held, so this shows it and moves one "
                "step. Use key_down/key_up to hold it open.";
            return info;
        case SystemAction::Overview:
            info.mechanism = "win+tab";
            info.note = "Task View. This is what a three- or four-finger swipe up invokes.";
            return info;
        case SystemAction::ShowDesktop: info.mechanism = "win+d"; return info;
        case SystemAction::NextDesktop:
            info.mechanism = "win+ctrl+right";
            info.note = "Next virtual desktop - what a four-finger swipe left invokes.";
            return info;
        case SystemAction::PreviousDesktop:
            info.mechanism = "win+ctrl+left";
            info.note = "Previous virtual desktop - what a four-finger swipe right invokes.";
            return info;
        case SystemAction::Notifications:
            info.mechanism = "win+n";
            info.note = "Windows 11. On Windows 10 this was win+a (Action Centre).";
            return info;
        case SystemAction::ScreenshotUi:
            info.mechanism = "win+shift+s";
            info.note = "Snipping Tool's region capture overlay.";
            return info;
        case SystemAction::Emoji: info.mechanism = "win+."; return info;
        case SystemAction::RunDialog: info.mechanism = "win+r"; return info;
        case SystemAction::Settings: info.mechanism = "win+i"; return info;
        case SystemAction::FileManager: info.mechanism = "win+e"; return info;
        case SystemAction::LockScreen:
            info.mechanism = "win+l";
            info.note = "Locks immediately. There is no confirmation.";
            return info;
    }
    info.supported = false;
    return info;
}

Status perform_system_action(SystemAction a, InputBackend& input, SystemBackend*) {
    switch (a) {
        case SystemAction::Search: return tap(input, "win+s");
        case SystemAction::Launcher: return tap(input, "win");
        case SystemAction::AppSwitcher: return tap(input, "alt+tab");
        case SystemAction::Overview: return tap(input, "win+tab");
        case SystemAction::ShowDesktop: return tap(input, "win+d");
        case SystemAction::NextDesktop: return tap(input, "win+ctrl+right");
        case SystemAction::PreviousDesktop: return tap(input, "win+ctrl+left");
        case SystemAction::Notifications: return tap(input, "win+n");
        case SystemAction::ScreenshotUi: return tap(input, "win+shift+s");
        case SystemAction::Emoji: return tap(input, "win+.");
        case SystemAction::RunDialog: return tap(input, "win+r");
        case SystemAction::Settings: return tap(input, "win+i");
        case SystemAction::FileManager: return tap(input, "win+e");
        case SystemAction::LockScreen: return tap(input, "win+l");
    }
    return err(ErrorCode::InvalidArgument, "unknown system action");
}

}  // namespace cc
