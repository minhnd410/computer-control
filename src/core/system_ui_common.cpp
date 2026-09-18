// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cctype>

#include "cc/system_ui.hpp"

namespace cc {

const char* to_string(SystemAction a) noexcept {
    switch (a) {
        case SystemAction::Search: return "search";
        case SystemAction::Launcher: return "launcher";
        case SystemAction::AppSwitcher: return "app_switcher";
        case SystemAction::Overview: return "overview";
        case SystemAction::ShowDesktop: return "show_desktop";
        case SystemAction::NextDesktop: return "next_desktop";
        case SystemAction::PreviousDesktop: return "previous_desktop";
        case SystemAction::Notifications: return "notifications";
        case SystemAction::ScreenshotUi: return "screenshot_ui";
        case SystemAction::Emoji: return "emoji";
        case SystemAction::RunDialog: return "run_dialog";
        case SystemAction::Settings: return "settings";
        case SystemAction::FileManager: return "file_manager";
        case SystemAction::LockScreen: return "lock_screen";
    }
    return "unknown";
}

Result<SystemAction> system_action_from_string(std::string_view name) {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(key.begin(), key.end(), '-', '_');

    // Aliases matter here: people reach for the name their own OS uses, and
    // being pedantic about it just makes the tool annoying.
    if (key == "search" || key == "spotlight" || key == "find") return SystemAction::Search;
    if (key == "launcher" || key == "launchpad" || key == "apps" || key == "start" ||
        key == "start_menu" || key == "all_apps") {
        return SystemAction::Launcher;
    }
    if (key == "app_switcher" || key == "switch_apps" || key == "alt_tab" ||
        key == "switch_windows" || key == "cmd_tab") {
        return SystemAction::AppSwitcher;
    }
    if (key == "overview" || key == "mission_control" || key == "task_view" || key == "expose" ||
        key == "activities" || key == "window_overview") {
        return SystemAction::Overview;
    }
    if (key == "show_desktop" || key == "desktop" || key == "minimize_all") {
        return SystemAction::ShowDesktop;
    }
    if (key == "next_desktop" || key == "next_space" || key == "next_workspace") {
        return SystemAction::NextDesktop;
    }
    if (key == "previous_desktop" || key == "prev_desktop" || key == "previous_space" ||
        key == "prev_space" || key == "previous_workspace") {
        return SystemAction::PreviousDesktop;
    }
    if (key == "notifications" || key == "notification_center" || key == "action_center") {
        return SystemAction::Notifications;
    }
    if (key == "screenshot_ui" || key == "snip" || key == "screenshot_tool" ||
        key == "capture_ui") {
        return SystemAction::ScreenshotUi;
    }
    if (key == "emoji" || key == "emoji_picker" || key == "character_picker") {
        return SystemAction::Emoji;
    }
    if (key == "run_dialog" || key == "run") return SystemAction::RunDialog;
    if (key == "settings" || key == "preferences" || key == "control_panel") {
        return SystemAction::Settings;
    }
    if (key == "file_manager" || key == "files" || key == "finder" || key == "explorer") {
        return SystemAction::FileManager;
    }
    if (key == "lock_screen" || key == "lock") return SystemAction::LockScreen;

    return err(ErrorCode::InvalidArgument, "unknown system action '" + std::string(name) + "'",
               "Known: search, launcher, app_switcher, overview, show_desktop, next_desktop, "
               "previous_desktop, notifications, screenshot_ui, emoji, run_dialog, settings, "
               "file_manager, lock_screen. Common aliases (spotlight, start, task_view, "
               "mission_control, explorer) are accepted too.");
}

std::vector<SystemActionInfo> system_actions() {
    std::vector<SystemActionInfo> out;
    for (int i = 0; i <= static_cast<int>(SystemAction::LockScreen); ++i) {
        out.push_back(describe_system_action(static_cast<SystemAction>(i)));
    }
    return out;
}

}  // namespace cc
