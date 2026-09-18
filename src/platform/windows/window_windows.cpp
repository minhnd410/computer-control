// SPDX-License-Identifier: MIT
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "cc/window.hpp"

namespace cc {
namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr,
                                        0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                          nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string window_title(HWND hwnd) {
    const int len = ::GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring buf(static_cast<std::size_t>(len) + 1, L'\0');
    const int got = ::GetWindowTextW(hwnd, buf.data(), len + 1);
    buf.resize(static_cast<std::size_t>(std::max(0, got)));
    return narrow(buf);
}

std::string process_name(DWORD pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string out;
    if (::QueryFullProcessImageNameW(h, 0, path, &size)) {
        std::wstring full(path, size);
        const auto slash = full.find_last_of(L"\\/");
        out = narrow(slash == std::wstring::npos ? full : full.substr(slash + 1));
        // "chrome.exe" reads worse than "chrome" in a window list.
        if (out.size() > 4 && out.compare(out.size() - 4, 4, ".exe") == 0) {
            out.resize(out.size() - 4);
        }
    }
    ::CloseHandle(h);
    return out;
}

bool is_listable(HWND hwnd, bool include_offscreen) {
    if (!::IsWindow(hwnd)) return false;
    if (!include_offscreen && !::IsWindowVisible(hwnd)) return false;

    // Cloaked windows are the modern trap: a UWP app's window and every
    // background virtual-desktop window is visible by IsWindowVisible but
    // composited away. Listing them fills the output with ghosts.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked && !include_offscreen) {
        return false;
    }

    // Tool windows are palettes and tooltips, not application windows.
    const LONG ex = ::GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;

    // Only top-level, owner-less windows: an owned window is a dialog of
    // something already in the list.
    if (::GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    if (window_title(hwnd).empty()) return false;

    RECT r{};
    if (!::GetWindowRect(hwnd, &r)) return false;
    if ((r.right - r.left) < 2 || (r.bottom - r.top) < 2) return false;
    return true;
}

struct EnumCtx {
    std::vector<HWND>* out;
    bool include_offscreen;
};

BOOL CALLBACK enum_windows(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumCtx*>(param);
    if (is_listable(hwnd, ctx->include_offscreen)) ctx->out->push_back(hwnd);
    return TRUE;
}

class WinWindows final : public WindowBackend {
public:
    explicit WinWindows(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    std::string name() const override { return "Win32"; }
    Status initialize() override { return ok(); }

    Result<std::vector<WindowInfo>> list_windows(bool include_offscreen) override {
        std::vector<HWND> handles;
        EnumCtx ctx{&handles, include_offscreen};
        ::EnumWindows(enum_windows, reinterpret_cast<LPARAM>(&ctx));

        const HWND foreground = ::GetForegroundWindow();
        std::vector<WindowInfo> out;
        out.reserve(handles.size());

        int z = 0;
        for (HWND hwnd : handles) {
            WindowInfo w;
            w.id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
            w.title = window_title(hwnd);
            w.layer = z++;  // EnumWindows walks front to back

            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            w.pid = pid;
            w.app_name = process_name(pid);

            RECT r{};
            if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
                ::GetWindowRect(hwnd, &r);
            }
            const Rect physical{static_cast<double>(r.left), static_cast<double>(r.top),
                                static_cast<double>(r.right - r.left),
                                static_cast<double>(r.bottom - r.top), Space::Physical};
            w.bounds = displays_->convert(physical, Space::Logical);

            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            if (::GetWindowPlacement(hwnd, &wp)) {
                if (wp.showCmd == SW_SHOWMINIMIZED)
                    w.state = WindowState::Minimized;
                else if (wp.showCmd == SW_SHOWMAXIMIZED)
                    w.state = WindowState::Maximized;
            }
            w.focused = (hwnd == foreground);
            w.on_screen = ::IsWindowVisible(hwnd) != FALSE;
            if (const Display* d = displays_->containing(w.bounds.center())) {
                w.display_index = d->index;
            }
            out.push_back(std::move(w));
        }
        return out;
    }

    Result<WindowInfo> focused_window() override {
        HWND hwnd = ::GetForegroundWindow();
        if (!hwnd) return err(ErrorCode::NotFound, "no foreground window");
        auto all = list_windows(true);
        if (!all) return all.error();
        const auto id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
        for (auto& w : all.value()) {
            if (w.id == id) {
                w.focused = true;
                return w;
            }
        }
        return err(ErrorCode::NotFound, "the foreground window is not listable");
    }

    Result<std::vector<AppInfo>> list_apps() override {
        auto windows = list_windows(false);
        if (!windows) return windows.error();

        std::vector<AppInfo> out;
        const HWND foreground = ::GetForegroundWindow();
        DWORD front_pid = 0;
        if (foreground) ::GetWindowThreadProcessId(foreground, &front_pid);

        for (const auto& w : windows.value()) {
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const AppInfo& a) { return a.pid == w.pid; });
            if (it != out.end()) {
                ++it->window_count;
                continue;
            }
            AppInfo a;
            a.pid = w.pid;
            a.name = w.app_name;
            a.window_count = 1;
            a.active = (static_cast<DWORD>(w.pid) == front_pid);
            out.push_back(std::move(a));
        }
        return out;
    }

    Status activate(std::uint64_t window_id) override {
        HWND hwnd = handle(window_id);
        if (!hwnd) return err(ErrorCode::NotFound, "window no longer exists");

        if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);

        // Windows refuses SetForegroundWindow from a process that does not own
        // the current foreground window. Briefly attaching to its input queue
        // is the standard, long-documented way around it.
        const DWORD our_thread = ::GetCurrentThreadId();
        const HWND foreground = ::GetForegroundWindow();
        const DWORD fg_thread = foreground ? ::GetWindowThreadProcessId(foreground, nullptr) : 0;

        bool attached = false;
        if (fg_thread && fg_thread != our_thread) {
            attached = ::AttachThreadInput(our_thread, fg_thread, TRUE) != FALSE;
        }
        ::BringWindowToTop(hwnd);
        const BOOL okay = ::SetForegroundWindow(hwnd);
        if (attached) ::AttachThreadInput(our_thread, fg_thread, FALSE);

        if (!okay && ::GetForegroundWindow() != hwnd) {
            return err(ErrorCode::BackendFailure, "the window refused to come to the foreground",
                       "Windows blocks focus stealing while the user is typing elsewhere, and "
                       "always for a window at a higher integrity level.");
        }
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
                       "no running window matches '" + std::string(name_or_bundle) + "'");
        }
        return activate(best->id);
    }

    Status set_bounds(std::uint64_t window_id, const Rect& bounds) override {
        HWND hwnd = handle(window_id);
        if (!hwnd) return err(ErrorCode::NotFound, "window no longer exists");

        const Rect physical = displays_->convert(bounds, Space::Physical);

        // SetWindowPos positions the *window* rect, but the caller gave us the
        // visible frame (which is what list_windows reports). On Aero the two
        // differ by the invisible resize border, so correct for the delta or
        // every move drifts by a few pixels.
        RECT frame{}, window{};
        double dx = 0, dy = 0, dw = 0, dh = 0;
        if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame,
                                              sizeof(frame))) &&
            ::GetWindowRect(hwnd, &window)) {
            dx = frame.left - window.left;
            dy = frame.top - window.top;
            dw = (frame.right - frame.left) - (window.right - window.left);
            dh = (frame.bottom - frame.top) - (window.bottom - window.top);
        }

        if (!::SetWindowPos(hwnd, nullptr, static_cast<int>(std::lround(physical.x - dx)),
                            static_cast<int>(std::lround(physical.y - dy)),
                            static_cast<int>(std::lround(physical.w - dw)),
                            static_cast<int>(std::lround(physical.h - dh)),
                            SWP_NOZORDER | SWP_NOACTIVATE)) {
            return err(ErrorCode::BackendFailure, "SetWindowPos failed");
        }
        return ok();
    }

    Status set_state(std::uint64_t window_id, WindowState state) override {
        HWND hwnd = handle(window_id);
        if (!hwnd) return err(ErrorCode::NotFound, "window no longer exists");
        switch (state) {
            case WindowState::Minimized: ::ShowWindow(hwnd, SW_MINIMIZE); return ok();
            case WindowState::Maximized: ::ShowWindow(hwnd, SW_MAXIMIZE); return ok();
            case WindowState::Normal: ::ShowWindow(hwnd, SW_RESTORE); return ok();
            case WindowState::Hidden: ::ShowWindow(hwnd, SW_HIDE); return ok();
            case WindowState::Fullscreen: {
                // Windows has no generic fullscreen; borderless on the
                // monitor's full bounds is the closest honest equivalent.
                const Display* d = displays_->containing(
                    displays_->convert(Rect{0, 0, 1, 1, Space::Physical}, Space::Logical).center());
                auto all = list_windows(true);
                if (all) {
                    for (const auto& w : all.value()) {
                        if (w.id == window_id) d = displays_->by_index(w.display_index);
                    }
                }
                if (!d) return err(ErrorCode::NotFound, "window is not on a known display");
                ::SetWindowLongW(hwnd, GWL_STYLE,
                                 ::GetWindowLongW(hwnd, GWL_STYLE) & ~(WS_CAPTION | WS_THICKFRAME));
                return set_bounds(window_id, d->bounds_logical);
            }
        }
        return ok();
    }

    Status close_window(std::uint64_t window_id) override {
        HWND hwnd = handle(window_id);
        if (!hwnd) return err(ErrorCode::NotFound, "window no longer exists");
        // WM_CLOSE, not WM_DESTROY: the app gets to run its save prompt.
        if (!::PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
            return err(ErrorCode::BackendFailure, "PostMessage(WM_CLOSE) failed");
        }
        return ok();
    }

    Result<AppInfo> launch(const LaunchRequest& req) override {
        const std::wstring target = widen(req.executable.empty() ? req.name : req.executable);
        if (target.empty()) {
            return err(ErrorCode::InvalidArgument, "provide name or executable");
        }

        std::wstring params;
        for (const auto& a : req.args) {
            if (!params.empty()) params += L' ';
            // Quote anything containing whitespace; ShellExecute re-parses the
            // parameter string.
            const std::wstring wide = widen(a);
            if (wide.find_first_of(L" \t\"") != std::wstring::npos) {
                params += L'"';
                for (wchar_t c : wide) {
                    if (c == L'"') params += L'\\';
                    params += c;
                }
                params += L'"';
            } else {
                params += wide;
            }
        }
        const std::wstring cwd = widen(req.cwd);

        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = target.c_str();
        info.lpParameters = params.empty() ? nullptr : params.c_str();
        info.lpDirectory = cwd.empty() ? nullptr : cwd.c_str();
        info.nShow = req.activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE;

        if (!::ShellExecuteExW(&info)) {
            return err(
                ErrorCode::NotFound,
                "could not launch '" + (req.executable.empty() ? req.name : req.executable) + "'",
                "Pass an absolute path to the executable, or a name that resolves through "
                "the App Paths registry key (for example \"notepad\", \"msedge\").");
        }

        AppInfo out;
        if (info.hProcess) {
            out.pid = static_cast<std::int64_t>(::GetProcessId(info.hProcess));
            ::CloseHandle(info.hProcess);
        }
        out.name = req.name.empty() ? req.executable : req.name;

        if (req.wait_for_window && out.pid > 0) {
            const auto deadline = std::chrono::steady_clock::now() + req.timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                auto windows = list_windows(false);
                if (windows) {
                    for (const auto& w : windows.value()) {
                        if (w.pid == out.pid) ++out.window_count;
                    }
                }
                if (out.window_count > 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{120});
            }
        }
        return out;
    }

    Status quit_app(std::int64_t pid, bool force) override {
        if (pid <= 4) {
            return err(ErrorCode::InvalidArgument,
                       "refusing to terminate pid " + std::to_string(pid) + " (a system process)");
        }
        if (!force) {
            // Ask every top-level window of the process to close first, so the
            // app can prompt to save.
            auto windows = list_windows(true);
            bool asked = false;
            if (windows) {
                for (const auto& w : windows.value()) {
                    if (w.pid == pid) {
                        ::PostMessageW(handle(w.id), WM_CLOSE, 0, 0);
                        asked = true;
                    }
                }
            }
            if (asked) return ok();
        }
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (!h) {
            return err(ErrorCode::PermissionDenied,
                       "cannot open pid " + std::to_string(pid) + " for termination",
                       "The process may be elevated or protected. Run computer-control "
                       "elevated to reach it.");
        }
        const BOOL okay = ::TerminateProcess(h, 1);
        ::CloseHandle(h);
        if (!okay) return err(ErrorCode::BackendFailure, "TerminateProcess failed");
        return ok();
    }

private:
    static HWND handle(std::uint64_t id) {
        HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
        return ::IsWindow(hwnd) ? hwnd : nullptr;
    }
};

}  // namespace

Result<std::unique_ptr<WindowBackend>> WindowBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<WinWindows>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<WindowBackend>(std::move(backend));
}

}  // namespace cc
