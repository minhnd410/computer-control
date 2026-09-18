// SPDX-License-Identifier: MIT
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <libproc.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cc/system.hpp"

namespace cc {
namespace {

std::string to_std(NSString* s) {
    if (!s) return {};
    const char* u = s.UTF8String;
    return u ? std::string(u) : std::string();
}

class MacSystem final : public SystemBackend {
public:
    std::string name() const override { return "AppKit+POSIX"; }
    Status initialize() override { return ok(); }

    Result<ClipboardContent> clipboard_get() override {
        @autoreleasepool {
            ClipboardContent c;
            NSPasteboard* pb = NSPasteboard.generalPasteboard;

            if (NSString* s = [pb stringForType:NSPasteboardTypeString]) {
                c.text = to_std(s);
                c.has_text = true;
            }
            if (NSData* png = [pb dataForType:NSPasteboardTypePNG]) {
                const auto* bytes = static_cast<const std::uint8_t*>(png.bytes);
                c.image_png.assign(bytes, bytes + png.length);
                c.has_image = true;
            } else if (NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF]) {
                // Convert rather than hand back TIFF: callers expect PNG, and
                // NSBitmapImageRep makes the conversion cheap.
                NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:tiff];
                NSData* png2 = [rep representationUsingType:NSBitmapImageFileTypePNG
                                                 properties:@{}];
                if (png2) {
                    const auto* bytes = static_cast<const std::uint8_t*>(png2.bytes);
                    c.image_png.assign(bytes, bytes + png2.length);
                    c.has_image = true;
                }
            }
            NSArray<NSURL*>* urls =
                [pb readObjectsForClasses:@[ NSURL.class ]
                                  options:@{
                                      NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                  }];
            for (NSURL* u in urls) {
                c.file_paths.push_back(to_std(u.path));
                c.has_files = true;
            }
            return c;
        }
    }

    Status clipboard_set(const ClipboardContent& c) override {
        @autoreleasepool {
            NSPasteboard* pb = NSPasteboard.generalPasteboard;
            [pb clearContents];
            BOOL wrote = NO;
            if (c.has_text || !c.text.empty()) {
                wrote = [pb setString:@(c.text.c_str()) forType:NSPasteboardTypeString] || wrote;
            }
            if (!c.image_png.empty()) {
                NSData* d = [NSData dataWithBytes:c.image_png.data() length:c.image_png.size()];
                wrote = [pb setData:d forType:NSPasteboardTypePNG] || wrote;
            }
            if (!c.file_paths.empty()) {
                NSMutableArray<NSURL*>* urls = [NSMutableArray array];
                for (const auto& p : c.file_paths) {
                    [urls addObject:[NSURL fileURLWithPath:@(p.c_str())]];
                }
                wrote = [pb writeObjects:urls] || wrote;
            }
            if (!wrote)
                return err(ErrorCode::BackendFailure, "the pasteboard rejected the content");
            return ok();
        }
    }

    Result<std::vector<ProcessInfo>> list_processes() override {
        // sysctl KERN_PROC_ALL is the only way to enumerate processes without
        // spawning `ps`. The size can change between the sizing call and the
        // fetch, so retry with headroom rather than failing.
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
        std::vector<struct kinfo_proc> procs;
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::size_t len = 0;
            if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0) {
                return err(ErrorCode::BackendFailure, "sysctl sizing failed");
            }
            procs.resize(len / sizeof(struct kinfo_proc) + 32);
            len = procs.size() * sizeof(struct kinfo_proc);
            if (sysctl(mib, 4, procs.data(), &len, nullptr, 0) == 0) {
                procs.resize(len / sizeof(struct kinfo_proc));
                break;
            }
            if (attempt == 3) return err(ErrorCode::BackendFailure, "sysctl KERN_PROC_ALL failed");
        }

        std::vector<ProcessInfo> out;
        out.reserve(procs.size());
        for (const auto& p : procs) {
            ProcessInfo info;
            info.pid = p.kp_proc.p_pid;
            info.ppid = p.kp_eproc.e_ppid;
            info.name = p.kp_proc.p_comm;

            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath(static_cast<int>(info.pid), path, sizeof(path)) > 0) {
                info.command = path;
            }
            struct proc_taskinfo ti {};
            if (proc_pidinfo(static_cast<int>(info.pid), PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) ==
                sizeof(ti)) {
                info.memory_bytes = static_cast<std::int64_t>(ti.pti_resident_size);
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    Status kill_process(std::int64_t pid, bool force) override {
        if (pid <= 1) {
            return err(ErrorCode::InvalidArgument, "refusing to signal pid " + std::to_string(pid),
                       "pid 0 and 1 are the kernel and launchd; killing them takes the machine "
                       "down.");
        }
        if (pid == getpid()) {
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
        @autoreleasepool {
            const auto start = std::chrono::steady_clock::now();
            NSTask* task = [[NSTask alloc] init];

            const std::string shell = req.shell.empty() ? "/bin/zsh" : req.shell;
            if (shell == "osascript") {
                task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/osascript"];
                task.arguments = @[ @"-e", @(req.command.c_str()) ];
            } else {
                task.executableURL = [NSURL fileURLWithPath:@(shell.c_str())];
                // -l would source the login profile on every call, which is slow
                // and makes behaviour depend on the user's dotfiles.
                task.arguments = @[ @"-c", @(req.command.c_str()) ];
            }

            if (!req.cwd.empty()) {
                task.currentDirectoryURL = [NSURL fileURLWithPath:@(req.cwd.c_str())];
            }
            if (!req.env.empty()) {
                NSMutableDictionary* env = [NSProcessInfo.processInfo.environment mutableCopy];
                for (const auto& [k, v] : req.env) env[@(k.c_str())] = @(v.c_str());
                task.environment = env;
            }

            NSPipe* out_pipe = [NSPipe pipe];
            NSPipe* err_pipe = [NSPipe pipe];
            task.standardOutput = out_pipe;
            task.standardError =
                req.capture_stderr ? err_pipe : NSFileHandle.fileHandleWithNullDevice;
            task.standardInput = NSFileHandle.fileHandleWithNullDevice;

            NSError* error = nil;
            if (![task launchAndReturnError:&error]) {
                return err(ErrorCode::BackendFailure,
                           "failed to start " + shell + ": " + to_std(error.localizedDescription));
            }

            // Drain both pipes on background queues. Reading them after waiting
            // deadlocks as soon as the child writes more than one pipe buffer
            // (64 KB), which a `ls -R /` hits instantly.
            __block NSMutableData* out_data = [NSMutableData data];
            __block NSMutableData* err_data = [NSMutableData data];
            dispatch_group_t group = dispatch_group_create();
            dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);

            dispatch_group_async(group, q, ^{
              NSData* d = [out_pipe.fileHandleForReading readDataToEndOfFile];
              if (d) [out_data appendData:d];
            });
            if (req.capture_stderr) {
                dispatch_group_async(group, q, ^{
                  NSData* d = [err_pipe.fileHandleForReading readDataToEndOfFile];
                  if (d) [err_data appendData:d];
                });
            }

            ShellResult result;
            const auto deadline_ns =
                static_cast<int64_t>(req.timeout.count()) * static_cast<int64_t>(NSEC_PER_MSEC);
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            dispatch_async(q, ^{
              [task waitUntilExit];
              dispatch_semaphore_signal(done);
            });

            if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, deadline_ns)) != 0) {
                result.timed_out = true;
                [task terminate];
                // Give it a moment to die on SIGTERM before escalating.
                dispatch_semaphore_wait(
                    done,
                    dispatch_time(DISPATCH_TIME_NOW, 2LL * static_cast<int64_t>(NSEC_PER_SEC)));
                if (task.isRunning) kill(task.processIdentifier, SIGKILL);
            }
            dispatch_group_wait(
                group, dispatch_time(DISPATCH_TIME_NOW, 2LL * static_cast<int64_t>(NSEC_PER_SEC)));

            result.exit_code = result.timed_out ? -1 : task.terminationStatus;
            result.stdout_text =
                std::string(static_cast<const char*>(out_data.bytes), out_data.length);
            result.stderr_text =
                std::string(static_cast<const char*>(err_data.bytes), err_data.length);
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            return result;
        }
    }

    Status notify(const NotificationRequest& req) override {
        @autoreleasepool {
            // UNUserNotificationCenter requires a signed, bundled app; a CLI
            // binary cannot use it. osascript's display notification works from
            // anywhere and is what every other tool in this space uses.
            auto escape = [](const std::string& s) {
                std::string out;
                for (char c : s) {
                    if (c == '"' || c == '\\') out.push_back('\\');
                    out.push_back(c);
                }
                return out;
            };
            std::string script = "display notification \"" + escape(req.message) + "\"";
            script += " with title \"" + escape(req.title) + "\"";
            if (!req.subtitle.empty()) script += " subtitle \"" + escape(req.subtitle) + "\"";
            if (!req.sound.empty()) script += " sound name \"" + escape(req.sound) + "\"";

            ShellRequest sr;
            sr.command = script;
            sr.shell = "osascript";
            sr.timeout = std::chrono::milliseconds{5000};
            auto r = run_shell(sr);
            if (!r) return r.error();
            if (r.value().exit_code != 0) {
                return err(ErrorCode::BackendFailure, "osascript failed: " + r.value().stderr_text,
                           "Notifications require the host app to be allowed in System Settings > "
                           "Notifications.");
            }
            return ok();
        }
    }
};

}  // namespace

// Registry is Windows-only; the base implementations report that clearly
// rather than each non-Windows backend repeating the same stub.
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
    auto backend = std::make_unique<MacSystem>();
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<SystemBackend>(std::move(backend));
}

}  // namespace cc
