// SPDX-License-Identifier: MIT
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

#include "cc/window.hpp"
#include "devices/device_internal.hpp"

// The subprocess helper below is the one genuinely platform-split piece of the
// device layer: CreateProcess and posix_spawn have nothing in common beyond
// the shape of the problem.
#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cc {

const char* to_string(DevicePlatform p) noexcept {
    switch (p) {
        case DevicePlatform::IOS: return "ios";
        case DevicePlatform::IPadOS: return "ipados";
        case DevicePlatform::TvOS: return "tvos";
        case DevicePlatform::WatchOS: return "watchos";
        case DevicePlatform::Android: return "android";
        default: return "unknown";
    }
}

const char* to_string(DeviceKind k) noexcept {
    switch (k) {
        case DeviceKind::Simulator: return "simulator";
        case DeviceKind::Emulator: return "emulator";
        case DeviceKind::Physical: return "physical";
        case DeviceKind::Mirrored: return "mirrored";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// DeviceViewport
// ---------------------------------------------------------------------------

DeviceViewport::DeviceViewport(Rect host_rect, Size device_points, Orientation)
    : host_(host_rect), device_(device_points) {
    if (host_.w > 0 && host_.h > 0 && device_.w > 0 && device_.h > 0) {
        sx_ = host_.w / device_.w;
        sy_ = host_.h / device_.h;
        valid_ = true;
    }
}

Point DeviceViewport::to_host(const Point& p) const {
    if (!valid_) return p;
    return Point{host_.x + p.x * sx_, host_.y + p.y * sy_, Space::Logical};
}

Point DeviceViewport::to_device(const Point& p) const {
    if (!valid_) return p;
    return Point{(p.x - host_.x) / sx_, (p.y - host_.y) / sy_, Space::Logical};
}

Rect DeviceViewport::to_host(const Rect& r) const {
    if (!valid_) return r;
    return Rect{host_.x + r.x * sx_, host_.y + r.y * sy_, r.w * sx_, r.h * sy_, Space::Logical};
}

namespace devices {

// ---------------------------------------------------------------------------
// exec
// ---------------------------------------------------------------------------

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

#if defined(_WIN32)

ExecResult exec(const std::string& program, const std::vector<std::string>& args,
                std::chrono::milliseconds timeout, const std::string& stdin_data) {
    ExecResult r;

    // CreateProcess takes one command line, so arguments must be quoted using
    // the exact rules CommandLineToArgvW reverses. Getting this wrong is how
    // paths with spaces and trailing backslashes break.
    auto quote = [](const std::string& a) -> std::string {
        if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
        std::string out = "\"";
        for (std::size_t i = 0;; ++i) {
            std::size_t backslashes = 0;
            while (i < a.size() && a[i] == '\\') {
                ++i;
                ++backslashes;
            }
            if (i == a.size()) {
                out.append(backslashes * 2, '\\');
                break;
            }
            if (a[i] == '"') {
                out.append(backslashes * 2 + 1, '\\');
                out.push_back('"');
            } else {
                out.append(backslashes, '\\');
                out.push_back(a[i]);
            }
        }
        out.push_back('"');
        return out;
    };

    std::string cmdline = quote(program);
    for (const auto& a : args) {
        cmdline.push_back(' ');
        cmdline += quote(a);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    HANDLE in_r = nullptr, in_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0) ||
        !CreatePipe(&in_r, &in_w, &sa, 0)) {
        r.spawn_failed = true;
        r.err = "CreatePipe failed";
        return r;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;  // never flash a console window
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    si.hStdInput = in_r;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back('\0');
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        r.spawn_failed = true;
        r.err = "CreateProcess failed for " + program;
        CloseHandle(out_r);
        CloseHandle(out_w);
        CloseHandle(err_r);
        CloseHandle(err_w);
        CloseHandle(in_r);
        CloseHandle(in_w);
        return r;
    }
    CloseHandle(out_w);
    CloseHandle(err_w);
    CloseHandle(in_r);

    if (!stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(in_w, stdin_data.data(), static_cast<DWORD>(stdin_data.size()), &written,
                  nullptr);
    }
    CloseHandle(in_w);

    auto drain = [](HANDLE h, std::string& sink) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) sink.append(buf, n);
    };
    drain(out_r, r.out);
    drain(err_r, r.err);
    CloseHandle(out_r);
    CloseHandle(err_r);

    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count())) == WAIT_TIMEOUT) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

#else

ExecResult exec(const std::string& program, const std::vector<std::string>& args,
                std::chrono::milliseconds timeout, const std::string& stdin_data) {
    ExecResult r;

    int out_pipe[2], err_pipe[2], in_pipe[2];
    if (pipe(out_pipe) != 0) {
        r.spawn_failed = true;
        r.err = "pipe failed";
        return r;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        r.spawn_failed = true;
        return r;
    }
    if (pipe(in_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        r.spawn_failed = true;
        return r;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, err_pipe[0]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawn_rc = posix_spawnp(&pid, program.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (spawn_rc != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        r.spawn_failed = true;
        r.err = program + ": " + std::strerror(spawn_rc);
        return r;
    }

    if (!stdin_data.empty()) {
        // Ignore SIGPIPE via the return value rather than a signal handler:
        // installing one would be a process-wide side effect of a library call.
        (void)!write(in_pipe[1], stdin_data.data(), stdin_data.size());
    }
    close(in_pipe[1]);

    // Poll both pipes so a child that fills stderr while we read stdout cannot
    // deadlock, and so the timeout is actually enforced during I/O rather than
    // only at wait() time.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    int open_count = 2;
    char buf[8192];
    while (open_count > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            r.timed_out = true;
            break;
        }
        const int wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const int n = poll(fds, 2, std::min(wait_ms, 200));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0) continue;
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                const ssize_t got = read(fds[i].fd, buf, sizeof(buf));
                if (got > 0) {
                    (i == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(got));
                } else {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    --open_count;
                }
            }
        }
    }
    for (int i = 0; i < 2; ++i)
        if (fds[i].fd >= 0) close(fds[i].fd);

    if (r.timed_out) {
        kill(pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

#endif

std::string which(const std::string& tool) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(tool);
        if (it != cache.end()) return it->second;
    }
#if defined(_WIN32)
    ExecResult r = exec("where", {tool}, std::chrono::milliseconds{4000});
#else
    ExecResult r = exec("/usr/bin/which", {tool}, std::chrono::milliseconds{4000});
#endif
    std::string path;
    if (r.exit_code == 0) {
        auto lines = split_lines(r.out);
        if (!lines.empty()) path = trim(lines.front());
    }
    std::lock_guard<std::mutex> lk(mu);
    cache[tool] = path;
    return path;
}

bool have(const std::string& tool) {
    return !which(tool).empty();
}

Rect fit_aspect(const Rect& container, double aspect) {
    if (container.w <= 0 || container.h <= 0 || aspect <= 0) return container;
    const double container_aspect = container.w / container.h;
    if (container_aspect > aspect) {
        // Container is wider than the device: bars on the left and right.
        const double w = container.h * aspect;
        return Rect{container.x + (container.w - w) / 2.0, container.y, w, container.h,
                    container.space};
    }
    const double h = container.w / aspect;
    return Rect{container.x, container.y + (container.h - h) / 2.0, container.w, h,
                container.space};
}

ChromeInsets chrome_for(const DeviceInfo& info, const WindowInfo& window) {
    ChromeInsets c;
    if (info.platform == DevicePlatform::Android) {
        // The Android emulator's window content is the device screen; the
        // side toolbar is a separate window, so only the title bar counts.
        c.top = 28;
        return c;
    }
    // iOS Simulator: a standard titled window whose content area holds the
    // device, letterboxed. Xcode 15+ removed the bezel, so no side insets.
    c.top = 28;
    return c;
}

// ---------------------------------------------------------------------------
// OnscreenDriver
// ---------------------------------------------------------------------------

OnscreenDriver::OnscreenDriver(std::shared_ptr<DisplayGraph> displays, InputBackend* input,
                               ScreenBackend* screen, WindowBackend* windows)
    : displays_(std::move(displays)), input_(input), screen_(screen), windows_(windows) {}

Result<DeviceViewport> OnscreenDriver::locate(const DeviceInfo& info) {
    if (!windows_) {
        return err(ErrorCode::Unsupported, "no window backend, cannot locate the device on screen");
    }
    auto all = windows_->list_windows(true);
    if (!all) return all.error();

    // Match on the device name appearing in the window title, which is how
    // both simulators label their windows ("iPhone 15 Pro - 18.0",
    // "Android Emulator - Pixel_8:5554").
    const WindowInfo* best = nullptr;
    int best_score = 0;
    for (const auto& w : all.value()) {
        if (info.host_window_id.has_value() && w.id == *info.host_window_id) {
            best = &w;
            break;
        }
        int s = fuzzy_score(info.name, w.title);
        if (info.platform == DevicePlatform::Android) {
            s = std::max(s, fuzzy_score("Android Emulator", w.app_name));
        } else {
            s = std::max(s, fuzzy_score("Simulator", w.app_name) / 2);
        }
        if (s > best_score) {
            best_score = s;
            best = &w;
        }
    }
    if (!best || best_score < 50) {
        return err(ErrorCode::NotFound,
                   "could not find an on-screen window for device '" + info.name + "'",
                   "Make sure the simulator window is open and not minimised. Use transport="
                   "\"bridge\" to drive it without a visible window.");
    }

    if (!best->on_screen) {
        return err(ErrorCode::NotFound,
                   "the window for '" + info.name + "' exists but is not currently visible",
                   "On macOS it is probably on another Space, or minimised. Bring it forward "
                   "first (Window > Activate, or windows(mode=\"activate\")) - the onscreen "
                   "transport drives real pixels, so the window has to be on screen.");
    }

    const ChromeInsets ci = chrome_for(info, *best);
    const Rect content{best->bounds.x + ci.left, best->bounds.y + ci.top,
                       best->bounds.w - ci.left - ci.right, best->bounds.h - ci.top - ci.bottom,
                       Space::Logical};

    Size dev = info.screen_points;
    if (dev.w <= 0 || dev.h <= 0) {
        // Unknown device metrics: assume the content area *is* the device
        // screen at 1:1. Coordinates then behave like host points, which is
        // at least predictable.
        dev = Size{content.w, content.h, Space::Logical};
    }
    if (info.orientation == Orientation::Landscape ||
        info.orientation == Orientation::LandscapeFlipped) {
        std::swap(dev.w, dev.h);
    }

    const Rect screen_rect = fit_aspect(content, dev.w / dev.h);
    return DeviceViewport(screen_rect, dev, info.orientation);
}

Status OnscreenDriver::tap(const DeviceViewport& vp, const Point& device_point,
                           const DeviceTapOptions& opts) {
    if (!input_) return err(ErrorCode::Unsupported, "no input backend");
    const Point host = vp.to_host(device_point);
    if (opts.hold.count() > 0) {
        GestureRequest g;
        g.kind = GestureKind::LongPress;
        g.center = host;
        g.hold = opts.hold;
        return input_->gesture(g);
    }
    ClickOptions c;
    c.count = std::clamp(opts.count, 1, 3);
    return input_->click(host, c);
}

Status OnscreenDriver::swipe(const DeviceViewport& vp, const Point& from, const Point& to,
                             const DeviceSwipeOptions& opts) {
    if (!input_) return err(ErrorCode::Unsupported, "no input backend");
    StrokeOptions so;
    so.motion.duration = opts.duration;
    so.motion.profile = MotionProfile::EaseInOut;
    // A long press before the travel is what makes iOS and Android treat the
    // gesture as a drag rather than a flick.
    so.settle_before_release = std::chrono::milliseconds{40};
    std::vector<PathPoint> path;
    PathPoint a{vp.to_host(from)};
    a.dwell = opts.press_delay;
    path.push_back(a);
    path.push_back(PathPoint{vp.to_host(to)});
    return input_->stroke(path, so);
}

Status OnscreenDriver::stroke(const DeviceViewport& vp, const std::vector<PathPoint>& path,
                              const StrokeOptions& opts) {
    if (!input_) return err(ErrorCode::Unsupported, "no input backend");
    std::vector<PathPoint> host_path;
    host_path.reserve(path.size());
    for (const auto& p : path) {
        PathPoint q = p;
        q.at = vp.to_host(p.at);
        host_path.push_back(q);
    }
    return input_->stroke(host_path, opts);
}

Status OnscreenDriver::gesture(const DeviceViewport& vp, const GestureRequest& req) {
    if (!input_) return err(ErrorCode::Unsupported, "no input backend");
    GestureRequest host = req;
    host.center = vp.to_host(req.center);
    // Distances and spreads are in device points and must be scaled too, or a
    // 200pt swipe on a simulator shown at 60% travels 200 host points, which
    // is a third of the screen too far.
    host.distance = req.distance * vp.effective_scale();
    host.spread = req.spread * vp.effective_scale();
    host.path.clear();
    for (const auto& p : req.path) host.path.push_back(vp.to_host(p));
    return input_->gesture(host);
}

Status OnscreenDriver::type_text(std::string_view utf8) {
    if (!input_) return err(ErrorCode::Unsupported, "no input backend");
    TypeOptions o;
    // Simulators forward host keystrokes to the device only when hardware
    // keyboard support is on, and they drop characters if typed too fast.
    o.cps = 25;
    o.allow_clipboard_fast_path = false;
    return input_->type_text(utf8, o);
}

Result<Frame> OnscreenDriver::screenshot(const DeviceViewport& vp) {
    if (!screen_) return err(ErrorCode::Unsupported, "no screen backend");
    CaptureOptions o;
    o.region = vp.host_rect();
    o.include_cursor = false;
    return screen_->capture(o);
}

}  // namespace devices

// ---------------------------------------------------------------------------
// DeviceManager
// ---------------------------------------------------------------------------

namespace {

class DeviceManagerImpl final : public DeviceManager {
public:
    DeviceManagerImpl(std::shared_ptr<DisplayGraph> displays, InputBackend* input,
                      ScreenBackend* screen, WindowBackend* windows)
        : driver_(std::make_shared<devices::OnscreenDriver>(std::move(displays), input, screen,
                                                            windows)) {}

    Result<std::vector<DeviceInfo>> list(bool booted_only) override {
        std::vector<DeviceInfo> out;
        // A missing toolchain is normal, not an error: a Linux box has adb but
        // no simctl, and a Mac without Xcode has neither. Partial results with
        // available_tooling() to explain the gap beat a hard failure.
        if (auto ios = devices::list_ios_devices(booted_only); ios) {
            for (auto& d : ios.value()) out.push_back(std::move(d));
        }
        if (auto droid = devices::list_android_devices(booted_only); droid) {
            for (auto& d : droid.value()) out.push_back(std::move(d));
        }

        // Anything visible on screen that the bridges did not already report.
        // Matching is by name so a simulator found both ways appears once,
        // with the bridge entry winning because it can do more.
        if (auto onscreen = devices::list_onscreen_devices(driver_->windows()); onscreen) {
            for (auto& d : onscreen.value()) {
                const bool duplicate =
                    std::any_of(out.begin(), out.end(), [&](const DeviceInfo& existing) {
                        return existing.booted && fuzzy_score(d.name, existing.name) >= 90;
                    });
                if (duplicate) continue;
                out.push_back(std::move(d));
            }
        }
        return out;
    }

    Result<std::shared_ptr<DeviceSession>> open(std::string_view id_or_name,
                                                DeviceTransport transport) override {
        auto all = list(false);
        if (!all) return all.error();

        const DeviceInfo* best = nullptr;
        int best_score = 0;
        for (const auto& d : all.value()) {
            if (d.id == id_or_name) {
                best = &d;
                best_score = 100;
                break;
            }
            const int s = fuzzy_score(id_or_name, d.name);
            if (s > best_score) {
                best_score = s;
                best = &d;
            }
        }
        if (!best || best_score < 60) {
            std::string known;
            for (const auto& d : all.value()) {
                known += "\n  " + d.name + "  [" + d.id + "]" + (d.booted ? " (booted)" : "");
            }
            if (known.empty()) {
                known = "\n  (none found; tooling available: ";
                for (const auto& t : available_tooling()) known += t + " ";
                known += ")";
            }
            return err(ErrorCode::NotFound, "no device matches '" + std::string(id_or_name) +
                                                "'. Known devices:" + known);
        }

        // An onscreen-only device has no bridge to fall back on, so it always
        // uses the window-driving session regardless of the requested
        // transport.
        if (best->id.rfind("onscreen:", 0) == 0 || transport == DeviceTransport::Onscreen) {
            if (best->id.rfind("onscreen:", 0) == 0) {
                return devices::open_onscreen(*best, driver_);
            }
        }
        if (best->platform == DevicePlatform::Android) {
            return devices::open_android(*best, transport, driver_);
        }
        return devices::open_ios(*best, transport, driver_);
    }

    Status boot(std::string_view id) override { return devices::boot_ios(id); }
    Status shutdown(std::string_view id) override { return devices::shutdown_ios(id); }

    std::vector<std::string> available_tooling() const override {
        std::vector<std::string> out;
        for (const char* tool : {"xcrun", "simctl", "idb", "adb", "scrcpy", "emulator"}) {
            if (devices::have(tool)) out.push_back(tool);
        }
        // simctl is not a standalone binary; it is a subcommand of xcrun.
        if (devices::have("xcrun")) {
            auto r = devices::exec("xcrun", {"simctl", "help"}, std::chrono::milliseconds{6000});
            if (r.exit_code == 0) out.push_back("xcrun simctl");
        }
        return out;
    }

private:
    std::shared_ptr<devices::OnscreenDriver> driver_;
};

}  // namespace

Result<std::shared_ptr<DeviceManager>> DeviceManager::create(std::shared_ptr<DisplayGraph> displays,
                                                             InputBackend* input,
                                                             ScreenBackend* screen,
                                                             WindowBackend* windows) {
    return std::shared_ptr<DeviceManager>(
        new DeviceManagerImpl(std::move(displays), input, screen, windows));
}

}  // namespace cc
