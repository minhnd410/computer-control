// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cc/display.hpp"
#include "cc/types.hpp"

namespace cc {

enum class WindowState : std::uint8_t { Normal = 0, Minimized, Maximized, Fullscreen, Hidden };

struct WindowInfo {
    std::uint64_t id = 0;  // CGWindowID / HWND / X11 Window
    std::int64_t pid = 0;
    std::string title;
    std::string app_name;
    std::string bundle_id;  // macOS bundle id / Windows AUMID / Linux WM_CLASS
    Rect bounds;            // logical space
    WindowState state = WindowState::Normal;
    bool focused = false;
    bool on_screen = true;
    std::int32_t display_index = 0;
    std::int32_t layer = 0;  // z-order hint; lower is closer to the front
};

struct AppInfo {
    std::int64_t pid = 0;
    std::string name;
    std::string bundle_id;
    std::string executable;
    bool active = false;
    std::int32_t window_count = 0;
};

struct LaunchRequest {
    std::string name;        // display name or bundle id
    std::string executable;  // explicit path; takes precedence over name
    std::vector<std::string> args;
    std::string cwd;
    bool activate = true;  // bring to the front
    bool wait_for_window = true;
    std::chrono::milliseconds timeout{8000};
};

class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    static Result<std::unique_ptr<WindowBackend>> create(std::shared_ptr<DisplayGraph> displays);

    virtual std::string name() const = 0;
    virtual Status initialize() = 0;

    virtual Result<std::vector<WindowInfo>> list_windows(bool include_offscreen) = 0;
    virtual Result<WindowInfo> focused_window() = 0;
    virtual Result<std::vector<AppInfo>> list_apps() = 0;

    virtual Status activate(std::uint64_t window_id) = 0;
    virtual Status activate_app(std::string_view name_or_bundle) = 0;
    virtual Status set_bounds(std::uint64_t window_id, const Rect& bounds) = 0;
    virtual Status set_state(std::uint64_t window_id, WindowState state) = 0;
    virtual Status close_window(std::uint64_t window_id) = 0;

    virtual Result<AppInfo> launch(const LaunchRequest& req) = 0;
    virtual Status quit_app(std::int64_t pid, bool force) = 0;

protected:
    std::shared_ptr<DisplayGraph> displays_;
};

// Fuzzy title matching shared by every backend and by the `Window` tool, so
// "activate the chrome window" behaves identically everywhere.
// Returns 0..100; 100 is an exact case-insensitive match.
int fuzzy_score(std::string_view needle, std::string_view haystack);

}  // namespace cc
