// SPDX-License-Identifier: MIT
#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cc/system.hpp"
#include "devices/device_internal.hpp"

namespace cc {
namespace {

using devices::exec;
using devices::trim;

// Clipboard on Linux is an X selection owned by a live process, not a kernel
// buffer: whoever sets it must stay running to serve paste requests. A
// short-lived binary therefore cannot own it directly, which is why this
// shells out to xclip/xsel/wl-copy, tools built to fork and persist.
struct ClipboardTool {
    std::string program;
    std::vector<std::string> read_args;
    std::vector<std::string> write_args;
};

const ClipboardTool* clipboard_tool() {
    static const ClipboardTool* chosen = []() -> const ClipboardTool* {
        static ClipboardTool wayland{"wl-copy", {}, {}};
        static ClipboardTool xclip{
            "xclip", {"-selection", "clipboard", "-o"}, {"-selection", "clipboard"}};
        static ClipboardTool xsel{"xsel", {"--clipboard", "--output"}, {"--clipboard", "--input"}};

        const bool wayland_session = std::getenv("WAYLAND_DISPLAY") != nullptr;
        if (wayland_session && devices::have("wl-copy") && devices::have("wl-paste")) {
            return &wayland;
        }
        if (devices::have("xclip")) return &xclip;
        if (devices::have("xsel")) return &xsel;
        if (devices::have("wl-copy")) return &wayland;
        return nullptr;
    }();
    return chosen;
}

class LinuxSystem final : public SystemBackend {
public:
    std::string name() const override { return "POSIX"; }
    Status initialize() override { return ok(); }

    Result<ClipboardContent> clipboard_get() override {
        const ClipboardTool* tool = clipboard_tool();
        if (!tool) return no_clipboard_tool().error();

        ClipboardContent c;
        devices::ExecResult r;
        if (tool->program == "wl-copy") {
            r = exec("wl-paste", {"--no-newline"}, std::chrono::milliseconds{5000});
        } else {
            r = exec(tool->program, tool->read_args, std::chrono::milliseconds{5000});
        }
        // An empty clipboard makes xclip exit non-zero; that is not an error.
        if (r.exit_code == 0 || !r.out.empty()) {
            c.text = r.out;
            c.has_text = !r.out.empty();
        }
        return c;
    }

    Status clipboard_set(const ClipboardContent& c) override {
        const ClipboardTool* tool = clipboard_tool();
        if (!tool) return no_clipboard_tool();

        devices::ExecResult r;
        if (tool->program == "wl-copy") {
            r = exec("wl-copy", {}, std::chrono::milliseconds{5000}, c.text);
        } else {
            r = exec(tool->program, tool->write_args, std::chrono::milliseconds{5000}, c.text);
        }
        if (r.spawn_failed) return no_clipboard_tool();
        return ok();
    }

    Result<std::vector<ProcessInfo>> list_processes() override {
        DIR* proc = ::opendir("/proc");
        if (!proc) return err(ErrorCode::IoError, "cannot read /proc");

        std::vector<ProcessInfo> out;
        const long page_size = ::sysconf(_SC_PAGESIZE);

        while (dirent* entry = ::readdir(proc)) {
            // Numeric directories under /proc are the processes.
            char* end = nullptr;
            const long pid = std::strtol(entry->d_name, &end, 10);
            if (!end || *end != '\0' || pid <= 0) continue;

            ProcessInfo p;
            p.pid = pid;

            const std::string base = "/proc/" + std::string(entry->d_name);
            if (std::ifstream comm(base + "/comm"); comm) {
                std::getline(comm, p.name);
            }
            if (std::ifstream cmdline(base + "/cmdline", std::ios::binary); cmdline) {
                std::string raw((std::istreambuf_iterator<char>(cmdline)),
                                std::istreambuf_iterator<char>());
                // Arguments are NUL-separated; join them for readability.
                std::replace(raw.begin(), raw.end(), '\0', ' ');
                p.command = trim(raw);
            }
            // statm's second field is the resident set in pages.
            if (std::ifstream statm(base + "/statm"); statm) {
                long total = 0, resident = 0;
                statm >> total >> resident;
                p.memory_bytes = static_cast<std::int64_t>(resident) * page_size;
            }
            if (std::ifstream status(base + "/status"); status) {
                std::string line;
                while (std::getline(status, line)) {
                    if (line.rfind("PPid:", 0) == 0) {
                        p.ppid = std::strtol(line.c_str() + 5, nullptr, 10);
                        break;
                    }
                }
            }
            out.push_back(std::move(p));
        }
        ::closedir(proc);
        return out;
    }

    Status kill_process(std::int64_t pid, bool force) override {
        if (pid <= 1) {
            return err(ErrorCode::InvalidArgument, "refusing to signal pid " + std::to_string(pid),
                       "pid 1 is init/systemd; killing it takes the machine down.");
        }
        if (pid == ::getpid()) {
            return err(ErrorCode::InvalidArgument,
                       "refusing to kill the automation process itself");
        }
        if (::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) != 0) {
            return err(ErrorCode::BackendFailure,
                       "kill(" + std::to_string(pid) + ") failed: " + std::strerror(errno));
        }
        return ok();
    }

    Result<ShellResult> run_shell(const ShellRequest& req) override {
        const auto start = std::chrono::steady_clock::now();
        const std::string shell = req.shell.empty() ? "/bin/sh" : req.shell;

        std::vector<std::string> args{"-c", req.command};
        // exec() does not honour cwd, so wrap the command when one is given.
        if (!req.cwd.empty()) {
            args = {"-c", "cd " + shell_quote(req.cwd) + " && { " + req.command + " ; }"};
        }

        auto r = exec(shell, args, req.timeout);
        if (r.spawn_failed) {
            return err(ErrorCode::BackendFailure, "cannot start " + shell + ": " + r.err);
        }

        ShellResult result;
        result.exit_code = r.exit_code;
        result.stdout_text = r.out;
        result.stderr_text = req.capture_stderr ? r.err : std::string{};
        result.timed_out = r.timed_out;
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return result;
    }

    Status notify(const NotificationRequest& req) override {
        if (!devices::have("notify-send")) {
            return err(ErrorCode::Unsupported, "notify-send is not installed",
                       "Install libnotify-bin (Debian/Ubuntu) or libnotify (Fedora/Arch). A "
                       "notification daemon must also be running, which is the case on any "
                       "normal desktop session but not on a bare X server.");
        }
        std::string body = req.message;
        if (!req.subtitle.empty()) body = req.subtitle + "\n" + body;
        auto r = exec("notify-send", {"--app-name=computer-control", req.title, body},
                      std::chrono::milliseconds{5000});
        if (r.exit_code != 0) {
            return err(ErrorCode::BackendFailure, "notify-send failed: " + trim(r.err));
        }
        return ok();
    }

private:
    static std::string shell_quote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'')
                out += "'\\''";
            else
                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    static Status no_clipboard_tool() {
        return err(ErrorCode::Unsupported, "no clipboard helper is installed",
                   "X11 clipboard ownership requires a resident process, so this needs an "
                   "external tool. Install xclip or xsel on X11, or wl-clipboard on Wayland "
                   "(Debian/Ubuntu: `sudo apt install xclip`).");
    }
};

}  // namespace

Result<std::string> SystemBackend::registry_get(std::string_view, std::string_view) {
    return err(ErrorCode::Unsupported, "the registry exists only on Windows");
}
Status SystemBackend::registry_set(std::string_view, std::string_view, std::string_view,
                                   std::string_view) {
    return err(ErrorCode::Unsupported, "the registry exists only on Windows");
}
Status SystemBackend::registry_delete(std::string_view, std::string_view) {
    return err(ErrorCode::Unsupported, "the registry exists only on Windows");
}
Result<std::vector<std::string>> SystemBackend::registry_list(std::string_view) {
    return err(ErrorCode::Unsupported, "the registry exists only on Windows");
}

Result<std::unique_ptr<SystemBackend>> SystemBackend::create() {
    auto backend = std::make_unique<LinuxSystem>();
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<SystemBackend>(std::move(backend));
}

}  // namespace cc
