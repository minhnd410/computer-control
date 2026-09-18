// SPDX-License-Identifier: MIT
//
// Linux window management, via EWMH (the _NET_* hints every modern window
// manager implements). There is no window-management API in core X11 itself;
// EWMH is the de-facto standard and is what wmctrl and xdotool use.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "cc/window.hpp"
#include "devices/device_internal.hpp"

#if defined(CC_HAVE_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

// Xlib defines `Status` (and a pile of other bare words) as a macro, which
// clobbers cc::Status and turns every Status-returning override into `int`.
// Nothing here uses X11's Status, so it goes away immediately after the
// headers that introduce it.
#ifdef Status
#undef Status
#endif

namespace cc {

#if defined(CC_HAVE_X11)
::Display* x11_display();
#endif

namespace {

#if defined(CC_HAVE_X11)

class LinuxWindows final : public WindowBackend {
public:
    explicit LinuxWindows(std::shared_ptr<DisplayGraph> displays) {
        displays_ = std::move(displays);
    }

    std::string name() const override { return "EWMH/X11"; }

    Status initialize() override {
        dpy_ = x11_display();
        if (!dpy_) return err(ErrorCode::BackendFailure, "cannot open the X display");
        root_ = DefaultRootWindow(dpy_);
        return ok();
    }

    Result<std::vector<WindowInfo>> list_windows(bool include_offscreen) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");

        auto ids = window_list();
        if (ids.empty()) {
            return err(ErrorCode::Unsupported,
                       "the window manager does not publish _NET_CLIENT_LIST",
                       "Window listing needs an EWMH-compliant window manager. Bare X sessions "
                       "and some tiling WMs in minimal configurations do not provide it.");
        }

        const ::Window active = active_window();
        std::vector<WindowInfo> out;
        out.reserve(ids.size());

        for (::Window w : ids) {
            WindowInfo info;
            info.id = static_cast<std::uint64_t>(w);
            info.title = window_title(w);
            info.pid = window_pid(w);
            info.app_name = window_class(w);
            info.focused = (w == active);

            ::XWindowAttributes attrs{};
            if (!::XGetWindowAttributes(dpy_, w, &attrs)) continue;
            info.on_screen = (attrs.map_state == IsViewable);
            if (!include_offscreen && !info.on_screen) continue;

            // Coordinates from XGetWindowAttributes are parent-relative, and
            // most windows are reparented into a WM frame, so they must be
            // translated to root coordinates or every position is wrong by the
            // frame offset.
            int root_x = 0, root_y = 0;
            ::Window child = 0;
            ::XTranslateCoordinates(dpy_, w, root_, 0, 0, &root_x, &root_y, &child);

            const Rect physical{static_cast<double>(root_x), static_cast<double>(root_y),
                                static_cast<double>(attrs.width), static_cast<double>(attrs.height),
                                Space::Physical};
            info.bounds = displays_->convert(physical, Space::Logical);
            if (info.bounds.w < 2 || info.bounds.h < 2) continue;

            if (has_state(w, "_NET_WM_STATE_HIDDEN"))
                info.state = WindowState::Minimized;
            else if (has_state(w, "_NET_WM_STATE_FULLSCREEN"))
                info.state = WindowState::Fullscreen;
            else if (has_state(w, "_NET_WM_STATE_MAXIMIZED_HORZ") &&
                     has_state(w, "_NET_WM_STATE_MAXIMIZED_VERT")) {
                info.state = WindowState::Maximized;
            }

            if (const Display* d = displays_->containing(info.bounds.center())) {
                info.display_index = d->index;
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    Result<WindowInfo> focused_window() override {
        const ::Window active = active_window();
        if (!active) return err(ErrorCode::NotFound, "no active window");
        auto all = list_windows(true);
        if (!all) return all.error();
        for (auto& w : all.value()) {
            if (w.id == static_cast<std::uint64_t>(active)) {
                w.focused = true;
                return w;
            }
        }
        return err(ErrorCode::NotFound, "the active window is not in the client list");
    }

    Result<std::vector<AppInfo>> list_apps() override {
        auto windows = list_windows(false);
        if (!windows) return windows.error();
        std::vector<AppInfo> out;
        for (const auto& w : windows.value()) {
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const AppInfo& a) { return a.pid == w.pid && w.pid != 0; });
            if (it != out.end()) {
                ++it->window_count;
                continue;
            }
            AppInfo a;
            a.pid = w.pid;
            a.name = w.app_name;
            a.window_count = 1;
            a.active = w.focused;
            out.push_back(std::move(a));
        }
        return out;
    }

    Status activate(std::uint64_t window_id) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const auto w = static_cast<::Window>(window_id);

        // _NET_ACTIVE_WINDOW is the cooperative path; XRaiseWindow alone does
        // not move focus and many WMs ignore it entirely.
        if (!send_message(w, "_NET_ACTIVE_WINDOW", {2, CurrentTime, 0, 0, 0})) {
            ::XRaiseWindow(dpy_, w);
            ::XSetInputFocus(dpy_, w, RevertToParent, CurrentTime);
        }
        ::XFlush(dpy_);
        return ok();
    }

    Status activate_app(std::string_view name_or_bundle) override {
        auto all = list_windows(false);
        if (!all) return all.error();
        const WindowInfo* best = nullptr;
        int best_score = 0;
        for (const auto& w : all.value()) {
            const int s = std::max(fuzzy_score(name_or_bundle, w.app_name),
                                   fuzzy_score(name_or_bundle, w.title));
            if (s > best_score) {
                best_score = s;
                best = &w;
            }
        }
        if (!best || best_score < 60) {
            return err(ErrorCode::NotFound,
                       "no window matches '" + std::string(name_or_bundle) + "'");
        }
        return activate(best->id);
    }

    Status set_bounds(std::uint64_t window_id, const Rect& bounds) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const auto w = static_cast<::Window>(window_id);
        const Rect p = displays_->convert(bounds, Space::Physical);

        // A maximized window ignores geometry changes, so clear that first.
        send_message(w, "_NET_WM_STATE",
                     {0, static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_HORZ")),
                      static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_VERT")), 1, 0});

        // Gravity 0 with the source-indication and all four value bits set is
        // what _NET_MOVERESIZE_WINDOW expects; the bit field is easy to get
        // wrong and a wrong one silently moves nothing.
        const long flags = (0L) | (1L << 8) | (1L << 9) | (1L << 10) | (1L << 11) | (2L << 12);
        if (!send_message(
                w, "_NET_MOVERESIZE_WINDOW",
                {flags, static_cast<long>(std::lround(p.x)), static_cast<long>(std::lround(p.y)),
                 static_cast<long>(std::lround(p.w)), static_cast<long>(std::lround(p.h))})) {
            ::XMoveResizeWindow(
                dpy_, w, static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)),
                static_cast<unsigned>(std::lround(p.w)), static_cast<unsigned>(std::lround(p.h)));
        }
        ::XFlush(dpy_);
        return ok();
    }

    Status set_state(std::uint64_t window_id, WindowState state) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const auto w = static_cast<::Window>(window_id);
        constexpr long kRemove = 0, kAdd = 1;

        switch (state) {
            case WindowState::Minimized: ::XIconifyWindow(dpy_, w, DefaultScreen(dpy_)); break;
            case WindowState::Maximized:
                send_message(w, "_NET_WM_STATE",
                             {kAdd, static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_HORZ")),
                              static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_VERT")), 1, 0});
                break;
            case WindowState::Fullscreen:
                send_message(w, "_NET_WM_STATE",
                             {kAdd, static_cast<long>(atom("_NET_WM_STATE_FULLSCREEN")), 0, 1, 0});
                break;
            case WindowState::Normal:
                send_message(w, "_NET_WM_STATE",
                             {kRemove, static_cast<long>(atom("_NET_WM_STATE_FULLSCREEN")),
                              static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_HORZ")), 1, 0});
                send_message(
                    w, "_NET_WM_STATE",
                    {kRemove, static_cast<long>(atom("_NET_WM_STATE_MAXIMIZED_VERT")), 0, 1, 0});
                ::XMapRaised(dpy_, w);
                break;
            case WindowState::Hidden: ::XUnmapWindow(dpy_, w); break;
        }
        ::XFlush(dpy_);
        return ok();
    }

    Status close_window(std::uint64_t window_id) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const auto w = static_cast<::Window>(window_id);
        // _NET_CLOSE_WINDOW lets the app prompt to save; XKillClient does not.
        if (!send_message(w, "_NET_CLOSE_WINDOW", {CurrentTime, 2, 0, 0, 0})) {
            return err(ErrorCode::BackendFailure, "the window manager refused _NET_CLOSE_WINDOW");
        }
        ::XFlush(dpy_);
        return ok();
    }

    Result<AppInfo> launch(const LaunchRequest& req) override {
        const std::string program = req.executable.empty() ? req.name : req.executable;
        if (program.empty()) {
            return err(ErrorCode::InvalidArgument, "provide name or executable");
        }

        // Launch detached: the child must outlive this call, and inheriting
        // our stdio would tie it to the MCP server's pipes.
        std::vector<std::string> args = req.args;
        auto r = devices::exec(
            "setsid",
            [&] {
                std::vector<std::string> a{program};
                for (const auto& x : args) a.push_back(x);
                return a;
            }(),
            std::chrono::milliseconds{1500});

        if (r.spawn_failed) {
            // setsid is not always installed; fall back to a direct spawn.
            r = devices::exec(program, args, std::chrono::milliseconds{1500});
            if (r.spawn_failed) {
                return err(ErrorCode::NotFound, "cannot launch '" + program + "'",
                           "The executable must be on PATH or given as an absolute path. For a "
                           "desktop application try its .desktop Exec name, for example "
                           "\"gnome-terminal\" or \"firefox\".");
            }
        }

        AppInfo info;
        info.name = program;

        if (req.wait_for_window) {
            const auto deadline = std::chrono::steady_clock::now() + req.timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                auto windows = list_windows(false);
                if (windows) {
                    for (const auto& w : windows.value()) {
                        if (fuzzy_score(program, w.app_name) >= 70) {
                            info.pid = w.pid;
                            ++info.window_count;
                        }
                    }
                }
                if (info.window_count > 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{150});
            }
        }
        return info;
    }

    Status quit_app(std::int64_t pid, bool force) override {
        if (pid <= 1) {
            return err(ErrorCode::InvalidArgument, "refusing to signal pid " + std::to_string(pid));
        }
        auto r = devices::exec("kill", {force ? "-9" : "-15", std::to_string(pid)},
                               std::chrono::milliseconds{3000});
        if (r.exit_code != 0) {
            return err(ErrorCode::BackendFailure, "kill failed: " + devices::trim(r.err));
        }
        return ok();
    }

private:
    ::Atom atom(const char* name) { return ::XInternAtom(dpy_, name, False); }

    // Reads a property, returning the raw bytes. Every EWMH query goes through
    // here so the XFree bookkeeping exists once.
    std::vector<unsigned char> property(::Window w, const char* name, ::Atom type,
                                        unsigned long* count_out) {
        ::Atom actual_type = 0;
        int actual_format = 0;
        unsigned long count = 0, bytes_after = 0;
        unsigned char* data = nullptr;
        if (::XGetWindowProperty(dpy_, w, atom(name), 0, (~0L), False, type, &actual_type,
                                 &actual_format, &count, &bytes_after, &data) != Success ||
            !data) {
            if (count_out) *count_out = 0;
            return {};
        }
        const std::size_t bytes = count * static_cast<std::size_t>(actual_format / 8);
        std::vector<unsigned char> out(data, data + bytes);
        if (count_out) *count_out = count;
        ::XFree(data);
        return out;
    }

    std::vector<::Window> window_list() {
        unsigned long count = 0;
        auto raw = property(root_, "_NET_CLIENT_LIST", XA_WINDOW, &count);
        if (raw.empty()) {
            raw = property(root_, "_NET_CLIENT_LIST_STACKING", XA_WINDOW, &count);
        }
        std::vector<::Window> out;
        const auto* ids = reinterpret_cast<const ::Window*>(raw.data());
        for (unsigned long i = 0; i < count && raw.size() >= (i + 1) * sizeof(::Window); ++i) {
            out.push_back(ids[i]);
        }
        return out;
    }

    ::Window active_window() {
        unsigned long count = 0;
        auto raw = property(root_, "_NET_ACTIVE_WINDOW", XA_WINDOW, &count);
        if (raw.size() < sizeof(::Window)) return 0;
        return *reinterpret_cast<const ::Window*>(raw.data());
    }

    std::string window_title(::Window w) {
        // _NET_WM_NAME is UTF-8 and is what modern apps set; WM_NAME is the
        // Latin-1 fallback for old clients.
        unsigned long count = 0;
        auto raw = property(w, "_NET_WM_NAME", atom("UTF8_STRING"), &count);
        if (!raw.empty()) return std::string(raw.begin(), raw.end());
        raw = property(w, "WM_NAME", AnyPropertyType, &count);
        return std::string(raw.begin(), raw.end());
    }

    std::string window_class(::Window w) {
        ::XClassHint hint{};
        if (::XGetClassHint(dpy_, w, &hint)) {
            // res_class is the application identity ("Firefox"); res_name is
            // the instance ("Navigator"), which is less useful here.
            std::string out =
                hint.res_class ? hint.res_class : (hint.res_name ? hint.res_name : "");
            if (hint.res_name) ::XFree(hint.res_name);
            if (hint.res_class) ::XFree(hint.res_class);
            return out;
        }
        return {};
    }

    std::int64_t window_pid(::Window w) {
        unsigned long count = 0;
        auto raw = property(w, "_NET_WM_PID", XA_CARDINAL, &count);
        if (raw.size() < sizeof(unsigned long)) return 0;
        return static_cast<std::int64_t>(*reinterpret_cast<const unsigned long*>(raw.data()));
    }

    bool has_state(::Window w, const char* state) {
        unsigned long count = 0;
        auto raw = property(w, "_NET_WM_STATE", XA_ATOM, &count);
        const auto* atoms = reinterpret_cast<const ::Atom*>(raw.data());
        const ::Atom want = atom(state);
        for (unsigned long i = 0; i < count && raw.size() >= (i + 1) * sizeof(::Atom); ++i) {
            if (atoms[i] == want) return true;
        }
        return false;
    }

    bool send_message(::Window w, const char* type, std::array<long, 5> data) {
        ::XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = atom(type);
        ev.xclient.format = 32;
        for (int i = 0; i < 5; ++i) ev.xclient.data.l[i] = data[static_cast<std::size_t>(i)];
        // The message goes to the root window: the WM is listening there, not
        // on the client window.
        return ::XSendEvent(dpy_, root_, False, SubstructureNotifyMask | SubstructureRedirectMask,
                            &ev) != 0;
    }

    ::Display* dpy_ = nullptr;
    ::Window root_ = 0;
};

#endif  // CC_HAVE_X11

}  // namespace

Result<std::unique_ptr<WindowBackend>> WindowBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
#if defined(CC_HAVE_X11)
    auto backend = std::make_unique<LinuxWindows>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<WindowBackend>(std::move(backend));
#else
    (void)displays;
    return err(ErrorCode::Unsupported, "this build has no X11 support");
#endif
}

}  // namespace cc
