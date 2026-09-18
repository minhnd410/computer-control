// SPDX-License-Identifier: MIT
//
// macOS permissions.
//
// The hard-won lesson encoded here: AXIsProcessTrusted() is not a reliable
// answer to "can I read the UI tree". A process launched from a granted
// terminal inherits that terminal's TCC identity, so the call returns true
// while every real accessibility query comes back as a placeholder element
// whose role is AXApplication and whose AXChildren is
// kAXErrorAttributeUnsupported. The permission looks granted and behaves as
// denied.
//
// Worse, prompting cannot fix it: AXIsProcessTrustedWithOptions only shows the
// dialog when the process is *not* already trusted, so an inheriting CLI never
// prompts and never appears in the Settings list, leaving the user with
// nothing to enable.
//
// So the accessibility check is functional - actually try to read a window -
// and the guidance distinguishes the inherited case, which needs the binary
// added by hand or shipped as an app bundle, from a plain first-run denial,
// which a prompt resolves.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "cc/permissions.hpp"

namespace cc {
namespace {

constexpr const char* kAccessibilityPane =
    "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility";
constexpr const char* kScreenRecordingPane =
    "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
constexpr const char* kInputMonitoringPane =
    "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent";

// Tries to actually read a window from another process. Returns true only if
// a real element came back, which is what separates a working grant from an
// inherited one.
bool accessibility_actually_works() {
    @autoreleasepool {
        int probed = 0;
        for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
            if (app.activationPolicy != NSApplicationActivationPolicyRegular) continue;
            if (app.processIdentifier == getpid()) continue;

            AXUIElementRef ax = AXUIElementCreateApplication(app.processIdentifier);
            if (!ax) continue;
            AXUIElementSetMessagingTimeout(ax, 0.6f);

            CFTypeRef windows = nullptr;
            const AXError e = AXUIElementCopyAttributeValue(ax, kAXWindowsAttribute, &windows);
            if (e == kAXErrorSuccess && windows && CFGetTypeID(windows) == CFArrayGetTypeID() &&
                CFArrayGetCount(static_cast<CFArrayRef>(windows)) > 0) {
                AXUIElementRef window = static_cast<AXUIElementRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(static_cast<CFArrayRef>(windows), 0)));

                CFTypeRef role = nullptr;
                bool real_role = false;
                if (AXUIElementCopyAttributeValue(window, kAXRoleAttribute, &role) ==
                        kAXErrorSuccess &&
                    role && CFGetTypeID(role) == CFStringGetTypeID()) {
                    // A real window says AXWindow/AXSheet/AXDialog. The
                    // placeholder handed back to an unprivileged caller says
                    // AXApplication, which no actual window ever reports.
                    NSString* r = (__bridge NSString*)role;
                    real_role = ![r isEqualToString:@"AXApplication"] && r.length > 0;
                }
                if (role) CFRelease(role);

                CFTypeRef children = nullptr;
                const AXError ce =
                    AXUIElementCopyAttributeValue(window, kAXChildrenAttribute, &children);
                if (children) CFRelease(children);

                CFRelease(windows);
                CFRelease(ax);

                if (real_role && ce == kAXErrorSuccess) return true;
                if (++probed >= 4) return false;  // consistently degraded
                continue;
            }
            if (windows) CFRelease(windows);
            CFRelease(ax);
        }
        return false;
    }
}

// macOS attributes a TCC grant to the "responsible process", which for a
// binary started from a shell is the terminal, not the binary. There is no
// public API for it, but responsibility_get_pid_responsible_for_pid has been
// in libsystem since 10.14 and is what `tccutil` and friends rely on. It is
// resolved at runtime so a future removal degrades to the heuristic below
// rather than failing to launch.
pid_t responsible_pid() {
    using Fn = pid_t (*)(pid_t);
    static const Fn fn =
        reinterpret_cast<Fn>(::dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid"));
    if (!fn) return -1;
    const pid_t result = fn(::getpid());
    return (result > 0) ? result : -1;
}

// Name of whichever process the grant is attributed to. This is the single
// most useful thing to show someone whose permissions "look granted" but do
// not work: it names the application they actually granted.
std::string responsible_process_name() {
    @autoreleasepool {
        const pid_t pid = responsible_pid();
        if (pid <= 0 || pid == ::getpid()) return {};
        NSRunningApplication* app =
            [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        if (app && app.localizedName) {
            return std::string(app.localizedName.UTF8String ? app.localizedName.UTF8String : "");
        }
        return "pid " + std::to_string(pid);
    }
}

std::string settings_hint(const char* pane_name) {
    return std::string("System Settings > Privacy & Security > ") + pane_name;
}

}  // namespace

const char* to_string(Permission p) noexcept {
    switch (p) {
        case Permission::Accessibility: return "accessibility";
        case Permission::ScreenRecording: return "screen_recording";
        case Permission::InputMonitoring: return "input_monitoring";
        case Permission::TouchInjection: return "touch_injection";
    }
    return "unknown";
}

const char* to_string(PermissionState s) noexcept {
    switch (s) {
        case PermissionState::Granted: return "granted";
        case PermissionState::Degraded: return "degraded";
        case PermissionState::Denied: return "denied";
        case PermissionState::NotDetermined: return "not_determined";
        case PermissionState::NotRequired: return "not_required";
    }
    return "unknown";
}

std::string executable_path() {
    char buf[4096];
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    char resolved[4096];
    if (realpath(buf, resolved)) return resolved;
    return buf;
}

std::string bundle_path() {
    @autoreleasepool {
        NSBundle* main = NSBundle.mainBundle;
        if (!main) return {};
        NSString* path = main.bundlePath;
        // A bare executable reports its containing directory as the "bundle",
        // so only count it when it is really a .app.
        if (!path || ![path hasSuffix:@".app"]) return {};
        return std::string(path.UTF8String ? path.UTF8String : "");
    }
}

bool has_own_tcc_identity() {
    const pid_t responsible = responsible_pid();
    if (responsible > 0) return responsible == ::getpid();
    // Fall back to the shape of the process: a bundle opened through
    // LaunchServices is its own responsible process, a bare CLI is not.
    return !bundle_path().empty();
}

std::string permission_owner() {
    return responsible_process_name();
}

PermissionStatus check_permission(Permission p) {
    PermissionStatus out;
    out.permission = p;

    switch (p) {
        case Permission::Accessibility: {
            out.affects = {"snapshot", "elements", "wait_for", "window move/resize/close"};
            const bool trusted = AXIsProcessTrusted();
            const bool works = trusted && accessibility_actually_works();

            if (works) {
                out.state = PermissionState::Granted;
                out.detail = "the accessibility tree is readable";
                return out;
            }
            if (trusted && !works) {
                const std::string owner = responsible_process_name();
                out.state = PermissionState::Degraded;
                out.detail = "AXIsProcessTrusted() reports true, but every window comes back as a "
                             "placeholder with no readable contents";
                if (!owner.empty()) {
                    out.detail += ". The grant is attributed to " + owner +
                                  ", not to this binary, and an inherited grant only covers "
                                  "the trust check - not real inspection";
                }
                // Prompting is a no-op while AXIsProcessTrusted() is true, so
                // do not promise the user a dialog that will never appear.
                out.can_prompt = false;
                out.remedy = "Add this binary under " + settings_hint("Accessibility") +
                             " with the + button:\n    " + executable_path() +
                             "\nThen restart the process. Granting " +
                             (owner.empty() ? std::string("the parent application") : owner) +
                             " is not enough on macOS 14 and later.\n"
                             "More durable: build the app bundle (`cmake --build build --target "
                             "macos_bundle`) and launch it with `open`, which gives it its own "
                             "identity so the grant sticks.\n"
                             "Screenshots, clicking and typing all work without this.";
                return out;
            }
            out.state = PermissionState::Denied;
            out.detail = "this process is not trusted for Accessibility";
            if (const std::string owner = responsible_process_name(); !owner.empty()) {
                out.detail += " (permissions here are attributed to " + owner + ")";
            }
            out.can_prompt = has_own_tcc_identity();
            out.remedy =
                "Run `computer-control-mcp --request-permissions`, or add it by hand under " +
                settings_hint("Accessibility") + ":\n    " + executable_path() +
                "\nThe grant is read at launch, so restart the process afterwards.";
            return out;
        }

        case Permission::ScreenRecording: {
            out.affects = {"screenshot", "snapshot", "zoom", "device screenshot"};
            if (CGPreflightScreenCaptureAccess()) {
                out.state = PermissionState::Granted;
                out.detail = "screen capture is permitted";
                return out;
            }
            out.state = PermissionState::Denied;
            out.detail = "screen capture is not permitted";
            out.can_prompt = true;
            out.remedy = "Run `computer-control-mcp --request-permissions`, or enable it under " +
                         settings_hint("Screen & System Audio Recording") + ":\n    " +
                         executable_path() + "\nRestart the process afterwards.";
            return out;
        }

        case Permission::InputMonitoring:
            // Only needed to *observe* input. This library synthesizes input,
            // which Accessibility covers, so it is never required.
            out.state = PermissionState::NotRequired;
            out.detail =
                "not needed: Input Monitoring governs observing input, and this library only "
                "synthesizes it";
            return out;

        case Permission::TouchInjection:
            out.state = PermissionState::NotRequired;
            out.detail =
                "macOS exposes no public multi-touch synthesis API, so there is no permission "
                "to grant. Pinch and multi-finger swipes are emulated; see "
                "`computer-control-mcp --doctor`.";
            out.affects = {"gesture (pinch, rotate, 3+ finger swipe)"};
            return out;
    }
    return out;
}

std::vector<PermissionStatus> check_permissions() {
    return {
        check_permission(Permission::Accessibility),
        check_permission(Permission::ScreenRecording),
        check_permission(Permission::InputMonitoring),
        check_permission(Permission::TouchInjection),
    };
}

Status open_permission_settings(Permission p) {
    @autoreleasepool {
        const char* url = nullptr;
        switch (p) {
            case Permission::Accessibility: url = kAccessibilityPane; break;
            case Permission::ScreenRecording: url = kScreenRecordingPane; break;
            case Permission::InputMonitoring: url = kInputMonitoringPane; break;
            case Permission::TouchInjection:
                return err(ErrorCode::Unsupported,
                           "there is no touch-injection permission on macOS");
        }
        NSURL* target = [NSURL URLWithString:@(url)];
        if (!target || ![NSWorkspace.sharedWorkspace openURL:target]) {
            return err(ErrorCode::BackendFailure, "could not open System Settings");
        }
        return ok();
    }
}

PermissionStatus request_permission(Permission p) {
    @autoreleasepool {
        if (p == Permission::Accessibility) {
            // Shows the system dialog *and* registers this binary in the
            // Accessibility list so there is something to enable - but only
            // when the process is not already trusted. An inheriting CLI gets
            // nothing from this, which is why the status below re-probes and
            // reports Degraded with manual instructions instead.
            NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES};
            (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
        } else if (p == Permission::ScreenRecording) {
            // Prompts on first call; afterwards it just reports the answer.
            (void)CGRequestScreenCaptureAccess();
        }

        PermissionStatus status = check_permission(p);
        // Opening the pane is the useful fallback in every case the dialog
        // cannot cover, and harmless when it can.
        if (status.state != PermissionState::Granted &&
            status.state != PermissionState::NotRequired) {
            (void)open_permission_settings(p);
        }
        return status;
    }
}

}  // namespace cc
