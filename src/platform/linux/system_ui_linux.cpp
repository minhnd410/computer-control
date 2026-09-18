// SPDX-License-Identifier: MIT
//
// Linux shell actions.
//
// Unlike macOS and Windows there is no single answer: GNOME, KDE, XFCE and the
// tiling window managers each bind these differently, and users remap them
// freely. The desktop environment is detected from XDG_CURRENT_DESKTOP and the
// binding reported alongside the action, so a caller can see what was actually
// sent rather than wondering why nothing happened.

#include <cstdlib>
#include <cstring>
#include <string>

#include "cc/system_ui.hpp"

namespace cc {
namespace {

enum class Desktop { Gnome, Kde, Xfce, Cinnamon, Mate, Unknown };

Desktop detect_desktop() {
    static const Desktop cached = [] {
        const char* raw = std::getenv("XDG_CURRENT_DESKTOP");
        if (!raw) raw = std::getenv("DESKTOP_SESSION");
        if (!raw) return Desktop::Unknown;
        std::string name(raw);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name.find("gnome") != std::string::npos) return Desktop::Gnome;
        if (name.find("kde") != std::string::npos || name.find("plasma") != std::string::npos) {
            return Desktop::Kde;
        }
        if (name.find("xfce") != std::string::npos) return Desktop::Xfce;
        if (name.find("cinnamon") != std::string::npos) return Desktop::Cinnamon;
        if (name.find("mate") != std::string::npos) return Desktop::Mate;
        return Desktop::Unknown;
    }();
    return cached;
}

const char* desktop_name() {
    switch (detect_desktop()) {
        case Desktop::Gnome: return "GNOME";
        case Desktop::Kde: return "KDE Plasma";
        case Desktop::Xfce: return "XFCE";
        case Desktop::Cinnamon: return "Cinnamon";
        case Desktop::Mate: return "MATE";
        default: return "an unrecognised desktop";
    }
}

// Returns the chord for an action, or nullptr when the desktop has no default.
const char* binding_for(SystemAction a) {
    const Desktop d = detect_desktop();
    switch (a) {
        case SystemAction::Search:
        case SystemAction::Launcher:
        case SystemAction::Overview:
            // GNOME's Activities covers search, launcher and overview with one
            // key; KDE splits them.
            if (d == Desktop::Gnome) return "super";
            if (d == Desktop::Kde) return (a == SystemAction::Overview) ? "ctrl+f8" : "super";
            return "super";
        case SystemAction::AppSwitcher: return "alt+tab";
        case SystemAction::ShowDesktop:
            if (d == Desktop::Kde) return "ctrl+f12";
            return "super+d";
        case SystemAction::NextDesktop:
            if (d == Desktop::Gnome) return "super+ctrl+down";
            return "ctrl+alt+right";
        case SystemAction::PreviousDesktop:
            if (d == Desktop::Gnome) return "super+ctrl+up";
            return "ctrl+alt+left";
        case SystemAction::Notifications:
            if (d == Desktop::Gnome) return "super+v";
            return nullptr;
        case SystemAction::ScreenshotUi:
            if (d == Desktop::Gnome) return "shift+printscreen";
            return "printscreen";
        case SystemAction::Emoji: return nullptr;
        case SystemAction::RunDialog:
            if (d == Desktop::Kde) return "alt+space";
            return "alt+f2";
        case SystemAction::Settings: return nullptr;
        case SystemAction::FileManager: return nullptr;
        case SystemAction::LockScreen:
            if (d == Desktop::Gnome) return "super+l";
            return "ctrl+alt+l";
    }
    return nullptr;
}

// Actions better served by launching a program than by a chord.
const char* program_for(SystemAction a) {
    switch (a) {
        case SystemAction::Settings:
            switch (detect_desktop()) {
                case Desktop::Gnome: return "gnome-control-center";
                case Desktop::Kde: return "systemsettings";
                case Desktop::Xfce: return "xfce4-settings-manager";
                default: return nullptr;
            }
        case SystemAction::FileManager: return "xdg-open .";
        default: return nullptr;
    }
}

}  // namespace

SystemActionInfo describe_system_action(SystemAction a) {
    SystemActionInfo info;
    info.action = a;

    if (const char* program = program_for(a)) {
        info.supported = true;
        info.mechanism = program;
        return info;
    }
    if (const char* chord = binding_for(a)) {
        info.supported = true;
        info.mechanism = chord;
        info.note = std::string("default binding on ") + desktop_name() +
                    "; users remap these freely, so verify with a screenshot if it matters";
        return info;
    }

    info.supported = false;
    info.note = std::string(desktop_name()) + " has no default binding for this";
    return info;
}

Status perform_system_action(SystemAction a, InputBackend& input, SystemBackend* system) {
    if (const char* program = program_for(a)) {
        if (!system) return err(ErrorCode::Unsupported, "this action needs the system backend");
        ShellRequest req;
        req.command = program;
        req.timeout = std::chrono::milliseconds{8000};
        auto r = system->run_shell(req);
        if (!r) return r.error();
        if (r.value().exit_code != 0) {
            return err(ErrorCode::NotFound,
                       std::string("could not run ") + program + ": " + r.value().stderr_text);
        }
        return ok();
    }

    const char* chord = binding_for(a);
    if (!chord) {
        return err(
            ErrorCode::Unsupported,
            std::string("no default binding for '") + to_string(a) + "' on " + desktop_name(),
            "Bind it in your desktop's keyboard settings and send that chord with the "
            "`key` tool instead.");
    }
    auto parsed = parse_chord(chord);
    if (!parsed) return parsed.error();
    return input.tap_chord(parsed.value(), 1);
}

}  // namespace cc
