// SPDX-License-Identifier: MIT
//
// macOS window and application management.
//
// Two APIs are needed and neither is sufficient alone:
//   CGWindowList  - enumerates every on-screen window with its id and bounds,
//                   needs no permission, but is strictly read-only.
//   Accessibility - can move, resize, raise and close windows, but has no
//                   notion of a CGWindowID, so windows must be correlated by
//                   (pid, frame) between the two.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "cc/window.hpp"

namespace cc {
namespace {

std::string to_std(NSString* s) {
    if (!s) return {};
    const char* u = s.UTF8String;
    return u ? std::string(u) : std::string();
}

AXUIElementRef copy_attr(AXUIElementRef el, CFStringRef attr) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(el, attr, &value) != kAXErrorSuccess) return nullptr;
    return static_cast<AXUIElementRef>(const_cast<void*>(value));
}

bool ax_frame(AXUIElementRef win, CGPoint* pos, CGSize* size) {
    bool got_pos = false, got_size = false;
    CFTypeRef v = nullptr;
    if (AXUIElementCopyAttributeValue(win, kAXPositionAttribute, &v) == kAXErrorSuccess && v) {
        got_pos = AXValueGetValue(static_cast<AXValueRef>(v), kAXValueTypeCGPoint, pos);
        CFRelease(v);
    }
    v = nullptr;
    if (AXUIElementCopyAttributeValue(win, kAXSizeAttribute, &v) == kAXErrorSuccess && v) {
        got_size = AXValueGetValue(static_cast<AXValueRef>(v), kAXValueTypeCGSize, size);
        CFRelease(v);
    }
    return got_pos && got_size;
}

// Finds the AX window element for a CGWindowID by matching the owning process
// and the frame. Frames are compared with a small tolerance because AX and
// CGWindowList can disagree by a fraction of a point on scaled displays.
AXUIElementRef find_ax_window(std::int64_t pid, const Rect& bounds) {
    AXUIElementRef app = AXUIElementCreateApplication(static_cast<pid_t>(pid));
    if (!app) return nullptr;

    CFTypeRef windows_ref = nullptr;
    if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &windows_ref) != kAXErrorSuccess ||
        !windows_ref) {
        CFRelease(app);
        return nullptr;
    }
    CFArrayRef windows = static_cast<CFArrayRef>(windows_ref);

    AXUIElementRef best = nullptr;
    double best_delta = 1e18;
    const CFIndex n = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < n; ++i) {
        AXUIElementRef w =
            static_cast<AXUIElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(windows, i)));
        CGPoint p{};
        CGSize s{};
        if (!ax_frame(w, &p, &s)) continue;
        const double delta = std::fabs(p.x - bounds.x) + std::fabs(p.y - bounds.y) +
                             std::fabs(s.width - bounds.w) + std::fabs(s.height - bounds.h);
        if (delta < best_delta) {
            best_delta = delta;
            best = w;
        }
    }
    // 8 points of slack: enough for rounding, tight enough not to pick a
    // different window of the same app.
    if (best && best_delta <= 8.0)
        CFRetain(best);
    else
        best = nullptr;

    CFRelease(windows);
    CFRelease(app);
    return best;
}

class MacWindows final : public WindowBackend {
public:
    explicit MacWindows(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    std::string name() const override { return "CGWindowList+AX"; }

    Status initialize() override { return ok(); }

    Result<std::vector<WindowInfo>> list_windows(bool include_offscreen) override {
        @autoreleasepool {
            CGWindowListOption opts = kCGWindowListExcludeDesktopElements;
            if (!include_offscreen) opts |= kCGWindowListOptionOnScreenOnly;

            CFArrayRef list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID);
            if (!list) return err(ErrorCode::BackendFailure, "CGWindowListCopyWindowInfo failed");

            NSArray* windows = (__bridge_transfer NSArray*)list;
            std::vector<WindowInfo> out;
            out.reserve(windows.count);

            const auto frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
            const pid_t front_pid = frontmost ? frontmost.processIdentifier : -1;

            for (NSDictionary* w in windows) {
                WindowInfo info;
                info.id = [w[(__bridge NSString*)kCGWindowNumber] unsignedLongLongValue];
                info.pid = [w[(__bridge NSString*)kCGWindowOwnerPID] longLongValue];
                info.title = to_std(w[(__bridge NSString*)kCGWindowName]);
                info.app_name = to_std(w[(__bridge NSString*)kCGWindowOwnerName]);
                info.layer = [w[(__bridge NSString*)kCGWindowLayer] intValue];

                NSDictionary* b = w[(__bridge NSString*)kCGWindowBounds];
                CGRect rect = CGRectZero;
                if (b) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)b, &rect);
                info.bounds = Rect{rect.origin.x, rect.origin.y, rect.size.width, rect.size.height,
                                   Space::Logical};

                info.on_screen = [w[(__bridge NSString*)kCGWindowIsOnscreen] boolValue];
                info.focused = (info.pid == front_pid && info.layer == 0);

                if (const Display* d = displays_->containing(info.bounds.center())) {
                    info.display_index = d->index;
                }

                NSRunningApplication* app = [NSRunningApplication
                    runningApplicationWithProcessIdentifier:static_cast<pid_t>(info.pid)];
                if (app) info.bundle_id = to_std(app.bundleIdentifier);

                // Layer 0 is the normal window layer. Everything above it is
                // system chrome (menu bar, Dock, status items, screen savers);
                // including it makes "list the windows" useless.
                if (info.layer != 0 && !include_offscreen) continue;
                if (info.bounds.w < 2 || info.bounds.h < 2) continue;

                info.state = info.on_screen ? WindowState::Normal : WindowState::Minimized;
                out.push_back(std::move(info));
            }
            return out;
        }
    }

    Result<WindowInfo> focused_window() override {
        @autoreleasepool {
            NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
            if (!front) return err(ErrorCode::NotFound, "no frontmost application");

            auto all = list_windows(false);
            if (!all) return all.error();
            for (auto& w : all.value()) {
                if (w.pid == front.processIdentifier) {
                    w.focused = true;
                    return w;
                }
            }
            return err(ErrorCode::NotFound, "the frontmost application (" +
                                                to_std(front.localizedName) +
                                                ") has no on-screen window");
        }
    }

    Result<std::vector<AppInfo>> list_apps() override {
        @autoreleasepool {
            std::vector<AppInfo> out;
            auto windows = list_windows(false);
            for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
                // Skip agents and daemons: they have no UI and would swamp the list.
                if (app.activationPolicy != NSApplicationActivationPolicyRegular) continue;
                AppInfo a;
                a.pid = app.processIdentifier;
                a.name = to_std(app.localizedName);
                a.bundle_id = to_std(app.bundleIdentifier);
                a.executable = to_std(app.executableURL.path);
                a.active = app.isActive;
                if (windows) {
                    for (const auto& w : windows.value())
                        if (w.pid == a.pid) ++a.window_count;
                }
                out.push_back(std::move(a));
            }
            return out;
        }
    }

    Status activate(std::uint64_t window_id) override {
        @autoreleasepool {
            auto all = list_windows(true);
            if (!all) return all.error();
            const WindowInfo* target = nullptr;
            for (const auto& w : all.value()) {
                if (w.id == window_id) {
                    target = &w;
                    break;
                }
            }
            if (!target)
                return err(ErrorCode::NotFound, "no window with id " + std::to_string(window_id));

            NSRunningApplication* app = [NSRunningApplication
                runningApplicationWithProcessIdentifier:static_cast<pid_t>(target->pid)];
            if (!app) return err(ErrorCode::NotFound, "owning process is gone");

            // Activating the app brings some window forward; raising the specific
            // AX window afterwards picks the right one when the app has several.
            [app activateWithOptions:NSApplicationActivateAllWindows];

            if (AXUIElementRef w = find_ax_window(target->pid, target->bounds)) {
                AXUIElementPerformAction(w, kAXRaiseAction);
                AXUIElementSetAttributeValue(w, kAXMainAttribute, kCFBooleanTrue);
                CFRelease(w);
            }
            return ok();
        }
    }

    Status activate_app(std::string_view name_or_bundle) override {
        @autoreleasepool {
            NSRunningApplication* best = nil;
            int best_score = 0;
            for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
                if (app.activationPolicy != NSApplicationActivationPolicyRegular) continue;
                const int s = std::max(fuzzy_score(name_or_bundle, to_std(app.localizedName)),
                                       fuzzy_score(name_or_bundle, to_std(app.bundleIdentifier)));
                if (s > best_score) {
                    best_score = s;
                    best = app;
                }
            }
            if (!best || best_score < 60) {
                return err(
                    ErrorCode::NotFound,
                    "no running application matches '" + std::string(name_or_bundle) + "'",
                    "Use App(mode=launch) to start it, or list apps to see what is running.");
            }
            [best activateWithOptions:NSApplicationActivateAllWindows];
            return ok();
        }
    }

    Status set_bounds(std::uint64_t window_id, const Rect& bounds) override {
        @autoreleasepool {
            auto all = list_windows(true);
            if (!all) return all.error();
            const WindowInfo* target = nullptr;
            for (const auto& w : all.value())
                if (w.id == window_id) {
                    target = &w;
                    break;
                }
            if (!target)
                return err(ErrorCode::NotFound, "no window with id " + std::to_string(window_id));

            AXUIElementRef w = find_ax_window(target->pid, target->bounds);
            if (!w) {
                return err(ErrorCode::PermissionDenied,
                           "could not reach the window through the Accessibility API",
                           "Grant Accessibility permission, and note that some apps (and all "
                           "sandboxed system windows) refuse programmatic repositioning.");
            }

            const Rect logical = displays_->convert(bounds, Space::Logical);
            CGPoint pos = CGPointMake(logical.x, logical.y);
            CGSize size = CGSizeMake(logical.w, logical.h);

            AXValueRef pv = AXValueCreate(kAXValueTypeCGPoint, &pos);
            AXValueRef sv = AXValueCreate(kAXValueTypeCGSize, &size);
            // Position first, then size: resizing first can clamp against the
            // current display's bounds and lose part of the requested size.
            const AXError e1 = AXUIElementSetAttributeValue(w, kAXPositionAttribute, pv);
            const AXError e2 = AXUIElementSetAttributeValue(w, kAXSizeAttribute, sv);
            if (pv) CFRelease(pv);
            if (sv) CFRelease(sv);
            CFRelease(w);

            if (e1 != kAXErrorSuccess && e2 != kAXErrorSuccess) {
                return err(ErrorCode::BackendFailure, "the window refused both move and resize");
            }
            return ok();
        }
    }

    Status set_state(std::uint64_t window_id, WindowState state) override {
        @autoreleasepool {
            auto all = list_windows(true);
            if (!all) return all.error();
            const WindowInfo* target = nullptr;
            for (const auto& w : all.value())
                if (w.id == window_id) {
                    target = &w;
                    break;
                }
            if (!target)
                return err(ErrorCode::NotFound, "no window with id " + std::to_string(window_id));

            AXUIElementRef w = find_ax_window(target->pid, target->bounds);
            if (!w)
                return err(ErrorCode::PermissionDenied, "window not reachable via Accessibility");

            Status result = ok();
            switch (state) {
                case WindowState::Minimized:
                    AXUIElementSetAttributeValue(w, kAXMinimizedAttribute, kCFBooleanTrue);
                    break;
                case WindowState::Normal:
                    AXUIElementSetAttributeValue(w, kAXMinimizedAttribute, kCFBooleanFalse);
                    AXUIElementSetAttributeValue(w, CFSTR("AXFullScreen"), kCFBooleanFalse);
                    break;
                case WindowState::Fullscreen:
                    AXUIElementSetAttributeValue(w, CFSTR("AXFullScreen"), kCFBooleanTrue);
                    break;
                case WindowState::Maximized: {
                    // macOS has no "maximize": the green button zooms, which is
                    // app-defined. Filling the display's work area is the
                    // predictable interpretation and what callers actually want.
                    CFRelease(w);
                    const Display* d = displays_->by_index(target->display_index);
                    if (!d) return err(ErrorCode::NotFound, "window is not on a known display");
                    return set_bounds(window_id, d->work_area_logical);
                }
                case WindowState::Hidden: {
                    CFRelease(w);
                    NSRunningApplication* app = [NSRunningApplication
                        runningApplicationWithProcessIdentifier:static_cast<pid_t>(target->pid)];
                    if (app) [app hide];
                    return ok();
                }
            }
            CFRelease(w);
            return result;
        }
    }

    Status close_window(std::uint64_t window_id) override {
        @autoreleasepool {
            auto all = list_windows(true);
            if (!all) return all.error();
            const WindowInfo* target = nullptr;
            for (const auto& w : all.value())
                if (w.id == window_id) {
                    target = &w;
                    break;
                }
            if (!target)
                return err(ErrorCode::NotFound, "no window with id " + std::to_string(window_id));

            AXUIElementRef w = find_ax_window(target->pid, target->bounds);
            if (!w)
                return err(ErrorCode::PermissionDenied, "window not reachable via Accessibility");
            AXUIElementRef button = copy_attr(w, kAXCloseButtonAttribute);
            AXError e = kAXErrorFailure;
            if (button) {
                e = AXUIElementPerformAction(button, kAXPressAction);
                CFRelease(button);
            }
            CFRelease(w);
            if (e != kAXErrorSuccess) {
                return err(ErrorCode::BackendFailure, "the window has no usable close button",
                           "Send the app's own close shortcut (cmd+w) instead.");
            }
            return ok();
        }
    }

    Result<AppInfo> launch(const LaunchRequest& req) override {
        @autoreleasepool {
            NSURL* url = nil;
            if (!req.executable.empty()) {
                url = [NSURL fileURLWithPath:@(req.executable.c_str())];
            } else if (!req.name.empty()) {
                NSWorkspace* ws = NSWorkspace.sharedWorkspace;
                url = [ws URLForApplicationWithBundleIdentifier:@(req.name.c_str())];
                if (!url) {
                    // Not a bundle id: try it as an app name in the usual places.
                    for (NSString* dir in @[
                             @"/Applications", @"/System/Applications",
                             @"/System/Applications/Utilities",
                             [NSHomeDirectory() stringByAppendingPathComponent:@"Applications"]
                         ]) {
                        NSString* candidate =
                            [[dir stringByAppendingPathComponent:@(req.name.c_str())]
                                stringByAppendingPathExtension:@"app"];
                        if ([NSFileManager.defaultManager fileExistsAtPath:candidate]) {
                            url = [NSURL fileURLWithPath:candidate];
                            break;
                        }
                    }
                }
            }
            if (!url) {
                return err(ErrorCode::NotFound,
                           "could not resolve '" + req.name + "' to an application",
                           "Pass a bundle identifier (com.apple.Safari), an app name as it appears "
                           "in /Applications, or an absolute executable path.");
            }

            NSWorkspaceOpenConfiguration* cfg = [NSWorkspaceOpenConfiguration configuration];
            cfg.activates = req.activate;
            if (!req.args.empty()) {
                NSMutableArray* args = [NSMutableArray array];
                for (const auto& a : req.args) [args addObject:@(a.c_str())];
                cfg.arguments = args;
            }

            __block NSRunningApplication* launched = nil;
            __block NSError* error = nil;
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [NSWorkspace.sharedWorkspace
                openApplicationAtURL:url
                       configuration:cfg
                   completionHandler:^(NSRunningApplication* app, NSError* e) {
                     launched = app;
                     error = e;
                     dispatch_semaphore_signal(sem);
                   }];
            const auto wait_ns =
                static_cast<int64_t>(req.timeout.count()) * static_cast<int64_t>(NSEC_PER_MSEC);
            if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, wait_ns)) != 0) {
                return err(ErrorCode::Timeout, "the application did not finish launching in time");
            }
            if (!launched) {
                return err(ErrorCode::BackendFailure,
                           "launch failed: " + (error ? to_std(error.localizedDescription)
                                                      : std::string("unknown")));
            }

            AppInfo info;
            info.pid = launched.processIdentifier;
            info.name = to_std(launched.localizedName);
            info.bundle_id = to_std(launched.bundleIdentifier);
            info.executable = to_std(launched.executableURL.path);
            info.active = launched.isActive;

            if (req.wait_for_window) {
                // Poll rather than sleep a fixed amount: a cold launch of a large
                // app can take seconds, a warm one is instant, and returning
                // before the first window exists makes the caller's next click
                // land on whatever was underneath.
                const auto deadline = std::chrono::steady_clock::now() + req.timeout;
                while (std::chrono::steady_clock::now() < deadline) {
                    auto windows = list_windows(false);
                    if (windows) {
                        for (const auto& w : windows.value()) {
                            if (w.pid == info.pid) {
                                ++info.window_count;
                            }
                        }
                    }
                    if (info.window_count > 0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds{120});
                }
            }
            return info;
        }
    }

    Status quit_app(std::int64_t pid, bool force) override {
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication
                runningApplicationWithProcessIdentifier:static_cast<pid_t>(pid)];
            if (!app)
                return err(ErrorCode::NotFound, "no application with pid " + std::to_string(pid));
            const BOOL okd = force ? [app forceTerminate] : [app terminate];
            if (!okd) {
                return err(ErrorCode::BackendFailure, "the application refused to terminate",
                           force ? "" : "Retry with force=true.");
            }
            return ok();
        }
    }
};

}  // namespace

Result<std::unique_ptr<WindowBackend>> WindowBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<MacWindows>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<WindowBackend>(std::move(backend));
}

}  // namespace cc
