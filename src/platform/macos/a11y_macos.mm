// SPDX-License-Identifier: MIT
//
// macOS accessibility tree extraction.
//
// The performance trap here is that every AXUIElementCopyAttributeValue is a
// synchronous IPC round trip to the target process. A naive full walk of a
// large Electron window issues tens of thousands of them and takes many
// seconds; worse, an unresponsive app blocks each call until it times out.
// Three things keep this usable:
//   1. A hard wall-clock budget checked at every node.
//   2. AXUIElementSetMessagingTimeout, so one hung app cannot stall the walk.
//   3. Fetching attributes in one batched call per element where possible, and
//      skipping subtrees that are entirely off-screen.

#import <Foundation/Foundation.h>

#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>

#include "cc/accessibility.hpp"
#include "cc/window.hpp"

namespace cc {
namespace {

using Clock = std::chrono::steady_clock;

std::string cf_to_std(CFTypeRef ref) {
    if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) return {};
    NSString* s = (__bridge NSString*)ref;
    const char* u = s.UTF8String;
    return u ? std::string(u) : std::string();
}

CFTypeRef copy_attr(AXUIElementRef el, CFStringRef attr) {
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(el, attr, &value) != kAXErrorSuccess) return nullptr;
    return value;
}

std::string string_attr(AXUIElementRef el, CFStringRef attr) {
    CFTypeRef v = copy_attr(el, attr);
    if (!v) return {};
    std::string out;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        out = cf_to_std(v);
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        NSString* num = [(__bridge NSNumber*)v stringValue];
        out = num.UTF8String ? std::string(num.UTF8String) : std::string();
    } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        out = CFBooleanGetValue(static_cast<CFBooleanRef>(v)) ? "true" : "false";
    }
    CFRelease(v);
    return out;
}

std::optional<bool> bool_attr(AXUIElementRef el, CFStringRef attr) {
    CFTypeRef v = copy_attr(el, attr);
    if (!v) return std::nullopt;
    std::optional<bool> out;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        out = CFBooleanGetValue(static_cast<CFBooleanRef>(v));
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &n);
        out = (n != 0);
    }
    CFRelease(v);
    return out;
}

bool frame_attr(AXUIElementRef el, Rect* out) {
    CGPoint p{};
    CGSize s{};
    bool got_p = false, got_s = false;
    if (CFTypeRef v = copy_attr(el, kAXPositionAttribute)) {
        got_p = AXValueGetValue(static_cast<AXValueRef>(v), kAXValueTypeCGPoint, &p);
        CFRelease(v);
    }
    if (CFTypeRef v = copy_attr(el, kAXSizeAttribute)) {
        got_s = AXValueGetValue(static_cast<AXValueRef>(v), kAXValueTypeCGSize, &s);
        CFRelease(v);
    }
    if (!got_p || !got_s) return false;
    *out = Rect{p.x, p.y, s.width, s.height, Space::Logical};
    return true;
}

const std::unordered_map<std::string, Role>& role_map() {
    static const std::unordered_map<std::string, Role> m = {
        {"AXApplication", Role::Application},
        {"AXWindow", Role::Window},
        {"AXSheet", Role::Sheet},
        {"AXDrawer", Role::Group},
        {"AXPopover", Role::Popover},
        {"AXMenu", Role::Menu},
        {"AXMenuItem", Role::MenuItem},
        {"AXMenuBar", Role::MenuBar},
        {"AXMenuBarItem", Role::MenuItem},
        {"AXMenuButton", Role::Button},
        {"AXButton", Role::Button},
        {"AXPopUpButton", Role::ComboBox},
        {"AXRadioButton", Role::RadioButton},
        {"AXCheckBox", Role::CheckBox},
        {"AXLink", Role::Link},
        {"AXRadioGroup", Role::Group},
        {"AXTabGroup", Role::TabList},
        {"AXTextField", Role::TextField},
        {"AXTextArea", Role::TextArea},
        {"AXSearchField", Role::SearchField},
        {"AXSecureTextField", Role::SecureTextField},
        {"AXComboBox", Role::ComboBox},
        {"AXList", Role::List},
        {"AXTable", Role::Table},
        {"AXRow", Role::Row},
        {"AXCell", Role::Cell},
        {"AXColumn", Role::ColumnHeader},
        {"AXOutline", Role::Tree},
        {"AXOutlineRow", Role::TreeItem},
        {"AXSlider", Role::Slider},
        {"AXProgressIndicator", Role::ProgressBar},
        {"AXIncrementor", Role::Stepper},
        {"AXStepper", Role::Stepper},
        {"AXScrollArea", Role::ScrollArea},
        {"AXScrollBar", Role::ScrollBar},
        {"AXGroup", Role::Group},
        {"AXToolbar", Role::Toolbar},
        {"AXImage", Role::Image},
        {"AXStaticText", Role::StaticText},
        {"AXSplitter", Role::Separator},
        {"AXWebArea", Role::WebView},
        {"AXDisclosureTriangle", Role::Disclosure},
        {"AXDateField", Role::DatePicker},
        {"AXColorWell", Role::ColorWell},
        {"AXHeading", Role::StaticText},
        {"AXValueIndicator", Role::Slider},
        {"AXUnknown", Role::Unknown},
    };
    return m;
}

Role map_role(const std::string& raw) {
    auto it = role_map().find(raw);
    return it == role_map().end() ? Role::Unknown : it->second;
}

bool role_is_interactive(Role r) {
    switch (r) {
        case Role::Button:
        case Role::ToggleButton:
        case Role::RadioButton:
        case Role::CheckBox:
        case Role::Link:
        case Role::Tab:
        case Role::MenuItem:
        case Role::TextField:
        case Role::TextArea:
        case Role::SearchField:
        case Role::SecureTextField:
        case Role::ComboBox:
        case Role::ListItem:
        case Role::Row:
        case Role::Cell:
        case Role::TreeItem:
        case Role::Slider:
        case Role::Stepper:
        case Role::Disclosure:
        case Role::DatePicker:
        case Role::ColorWell: return true;
        default: return false;
    }
}

// A window element that reports role AXApplication is the signature of a
// refused inspection: macOS hands back a placeholder rather than an error.
// A real window reports AXWindow, AXSheet, AXDrawer or similar.
bool is_placeholder_window(AXUIElementRef el) {
    const std::string r = string_attr(el, kAXRoleAttribute);
    return r.empty() || r == "AXApplication";
}

class MacA11y final : public AccessibilityBackend {
public:
    explicit MacA11y(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    std::string name() const override { return "AXUIElement"; }

    Status initialize() override { return check_permission(false); }

    Status check_permission(bool prompt) override {
        if (prompt) {
            NSDictionary* opts = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES};
            if (AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts)) return ok();
        } else if (AXIsProcessTrusted()) {
            return ok();
        }
        return err(ErrorCode::PermissionDenied, "this process is not trusted for Accessibility",
                   "System Settings > Privacy & Security > Accessibility, add the binary (or "
                   "the terminal running it), enable it, and restart the process. The trust "
                   "state is read at launch, so toggling it while running does nothing.");
    }

    Result<Tree> snapshot(const TreeOptions& opts) override {
        @autoreleasepool {
            if (auto st = check_permission(false); !st) return st.error();

            const auto start = Clock::now();
            Tree tree;
            deadline_ = start + opts.budget;
            nodes_left_ = opts.max_nodes;

            std::vector<std::int64_t> pids;
            if (opts.pid.has_value()) {
                pids.push_back(*opts.pid);
            } else {
                auto wb = WindowBackend::create(displays_);
                if (!wb) return wb.error();
                auto apps = wb.value()->list_apps();
                if (!apps) return apps.error();
                for (const auto& a : apps.value()) pids.push_back(a.pid);
            }

            // macOS can report AXIsProcessTrusted() == true while still refusing
            // real inspection of other processes, handing back placeholder elements
            // whose role is AXApplication and whose AXChildren is
            // kAXErrorAttributeUnsupported. That happens when the grant belongs to
            // the parent terminal rather than to this binary. Silently returning an
            // empty tree makes it look like the apps have no UI, so count how many
            // window elements were real and diagnose it explicitly below.
            int windows_seen = 0;
            int placeholder_windows = 0;

            for (std::int64_t pid : pids) {
                if (Clock::now() > deadline_ || nodes_left_ <= 0) {
                    tree.truncated = true;
                    tree.truncation_reason =
                        (nodes_left_ <= 0) ? "node budget exhausted" : "time budget exhausted";
                    break;
                }
                AXUIElementRef app = AXUIElementCreateApplication(static_cast<pid_t>(pid));
                if (!app) continue;
                // One second per message: long enough for a busy app to answer,
                // short enough that a hung one cannot eat the whole budget.
                AXUIElementSetMessagingTimeout(app, 1.0f);

                const std::string app_name = string_attr(app, CFSTR("AXTitle"));

                CFTypeRef windows_ref = copy_attr(app, kAXWindowsAttribute);
                if (windows_ref && CFGetTypeID(windows_ref) == CFArrayGetTypeID()) {
                    CFArrayRef windows = static_cast<CFArrayRef>(windows_ref);
                    const CFIndex n = CFArrayGetCount(windows);
                    for (CFIndex i = 0; i < n; ++i) {
                        AXUIElementRef w = static_cast<AXUIElementRef>(
                            const_cast<void*>(CFArrayGetValueAtIndex(windows, i)));
                        ++windows_seen;
                        if (is_placeholder_window(w)) ++placeholder_windows;
                        Node root;
                        if (build(w, root, 0, opts, pid, app_name, tree)) {
                            tree.roots.push_back(std::move(root));
                        }
                    }
                }
                if (windows_ref) CFRelease(windows_ref);
                CFRelease(app);
            }

            assign_labels(tree, opts);

            // Refused inspection and a genuinely empty desktop look identical from
            // the outside, so only call it out when placeholders were returned AND
            // nothing usable came back. Reporting "0 elements" for what is really a
            // permissions problem sends the caller hunting for the wrong bug.
            if (placeholder_windows > 0 && tree.interactive.empty()) {
                return err(ErrorCode::PermissionDenied,
                           "Accessibility inspection is being refused: " +
                               std::to_string(placeholder_windows) + " of " +
                               std::to_string(windows_seen) +
                               " windows came back as placeholders with no readable contents",
                           "AXIsProcessTrusted() reports true here, which usually means the grant "
                           "belongs to the parent terminal rather than to this binary. Add the "
                           "binary itself under System Settings > Privacy & Security > "
                           "Accessibility, enable it, and restart the process. Screenshots, "
                           "clicking and typing all work without this; only the element tree "
                           "needs it.");
            }
            if (placeholder_windows > 0) {
                tree.truncated = true;
                tree.truncation_reason = std::to_string(placeholder_windows) + " of " +
                                         std::to_string(windows_seen) +
                                         " windows were not inspectable (Accessibility grant "
                                         "missing for this binary)";
            }
            tree.elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
            if (nodes_left_ <= 0 && tree.truncation_reason.empty()) {
                tree.truncated = true;
                tree.truncation_reason = "node budget exhausted";
            }
            return tree;
        }
    }

    Result<Node> element_at(const Point& p) override {
        @autoreleasepool {
            if (auto st = check_permission(false); !st) return st.error();
            AXUIElementRef system = AXUIElementCreateSystemWide();
            if (!system)
                return err(ErrorCode::BackendFailure, "AXUIElementCreateSystemWide failed");
            AXUIElementSetMessagingTimeout(system, 1.0f);

            const Point logical = displays_->convert(p, Space::Logical);
            AXUIElementRef el = nullptr;
            const AXError e = AXUIElementCopyElementAtPosition(
                system, static_cast<float>(logical.x), static_cast<float>(logical.y), &el);
            CFRelease(system);
            if (e != kAXErrorSuccess || !el) {
                return err(ErrorCode::NotFound, "no accessible element at " +
                                                    std::to_string(logical.x) + "," +
                                                    std::to_string(logical.y));
            }
            Node n;
            fill(el, n, 0, "");
            CFRelease(el);
            return n;
        }
    }

    Result<Node> focused_element() override {
        @autoreleasepool {
            if (auto st = check_permission(false); !st) return st.error();
            AXUIElementRef system = AXUIElementCreateSystemWide();
            if (!system)
                return err(ErrorCode::BackendFailure, "AXUIElementCreateSystemWide failed");
            AXUIElementSetMessagingTimeout(system, 1.0f);
            CFTypeRef focused = copy_attr(system, kAXFocusedUIElementAttribute);
            CFRelease(system);
            if (!focused) return err(ErrorCode::NotFound, "nothing has keyboard focus");
            Node n;
            fill(static_cast<AXUIElementRef>(const_cast<void*>(focused)), n, 0, "");
            n.focused = true;
            CFRelease(focused);
            return n;
        }
    }

    Status perform_action(const Node& node, std::string_view action) override {
        @autoreleasepool {
            // Nodes are value types with no live AX reference, so re-resolve by
            // position. This is deliberate: holding AXUIElementRefs across calls
            // leaks into other processes and goes stale the moment the UI redraws.
            auto found = element_at(node.bounds.center());
            if (!found) return found.error();

            AXUIElementRef system = AXUIElementCreateSystemWide();
            AXUIElementRef el = nullptr;
            const Point c = node.bounds.center();
            AXUIElementCopyElementAtPosition(system, static_cast<float>(c.x),
                                             static_cast<float>(c.y), &el);
            CFRelease(system);
            if (!el) return err(ErrorCode::NotFound, "element is no longer at that position");

            NSString* name = @(std::string(action).c_str());
            if ([name isEqualToString:@"press"]) name = (__bridge NSString*)kAXPressAction;
            const AXError e = AXUIElementPerformAction(el, (__bridge CFStringRef)name);
            CFRelease(el);
            if (e != kAXErrorSuccess) {
                return err(ErrorCode::BackendFailure,
                           "action '" + std::string(action) + "' failed (AXError " +
                               std::to_string(static_cast<int>(e)) + ")");
            }
            return ok();
        }
    }

    Status set_value(const Node& node, std::string_view value) override {
        @autoreleasepool {
            AXUIElementRef system = AXUIElementCreateSystemWide();
            AXUIElementRef el = nullptr;
            const Point c = node.bounds.center();
            AXUIElementCopyElementAtPosition(system, static_cast<float>(c.x),
                                             static_cast<float>(c.y), &el);
            CFRelease(system);
            if (!el) return err(ErrorCode::NotFound, "element is no longer at that position");
            NSString* s = @(std::string(value).c_str());
            const AXError e =
                AXUIElementSetAttributeValue(el, kAXValueAttribute, (__bridge CFTypeRef)s);
            CFRelease(el);
            if (e != kAXErrorSuccess) {
                return err(ErrorCode::BackendFailure, "the element refused a new value",
                           "Click it and type instead; many controls only accept typed input.");
            }
            return ok();
        }
    }

private:
    void fill(AXUIElementRef el, Node& n, std::int64_t pid, const std::string& app_name) {
        n.raw_role = string_attr(el, kAXRoleAttribute);
        n.role = map_role(n.raw_role);
        n.name = string_attr(el, kAXTitleAttribute);
        if (n.name.empty()) n.name = string_attr(el, CFSTR("AXDescription"));
        n.value = string_attr(el, kAXValueAttribute);
        n.description = string_attr(el, CFSTR("AXRoleDescription"));
        n.help = string_attr(el, kAXHelpAttribute);
        n.automation_id = string_attr(el, CFSTR("AXIdentifier"));
        n.pid = pid;
        n.app_name = app_name;

        if (!frame_attr(el, &n.bounds)) n.bounds = Rect{};

        const auto enabled = bool_attr(el, kAXEnabledAttribute);
        n.enabled = enabled.value_or(true);
        n.focused = bool_attr(el, kAXFocusedAttribute).value_or(false);
        n.checked = bool_attr(el, CFSTR("AXValue"));
        n.expanded = bool_attr(el, CFSTR("AXExpanded"));
        n.selected = bool_attr(el, CFSTR("AXSelected"));

        // A zero-size or off-virtual-desktop element is present in the tree
        // but cannot be clicked; marking it invisible keeps it out of the
        // labelled set without dropping it from the structure.
        n.visible = !n.bounds.empty();

        CFArrayRef actions = nullptr;
        if (AXUIElementCopyActionNames(el, &actions) == kAXErrorSuccess && actions) {
            const CFIndex count = CFArrayGetCount(actions);
            for (CFIndex i = 0; i < count; ++i) {
                std::string a = cf_to_std(CFArrayGetValueAtIndex(actions, i));
                if (a.rfind("AX", 0) == 0) a = a.substr(2);
                std::transform(a.begin(), a.begin() + 1, a.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                n.actions.push_back(std::move(a));
            }
            CFRelease(actions);
        }

        const bool has_press =
            std::find(n.actions.begin(), n.actions.end(), "press") != n.actions.end();
        n.interactive = n.enabled && n.visible && (has_press || role_is_interactive(n.role));
        n.scrollable = (n.role == Role::ScrollArea);
    }

    bool build(AXUIElementRef el, Node& out, int depth, const TreeOptions& opts, std::int64_t pid,
               const std::string& app_name, Tree& tree) {
        if (depth > opts.max_depth || nodes_left_ <= 0 || Clock::now() > deadline_) {
            if (!tree.truncated) {
                tree.truncated = true;
                tree.truncation_reason = (Clock::now() > deadline_)
                                             ? "time budget exhausted"
                                             : "depth or node budget reached";
            }
            return false;
        }
        --nodes_left_;
        fill(el, out, pid, app_name);

        // Pruning an off-screen subtree is the single biggest win: collapsed
        // sidebars and background tabs are fully populated in the AX tree and
        // account for most of the walk on a real desktop.
        if (!opts.include_offscreen && out.bounds.empty() && depth > 0) return true;
        if (opts.region && !out.bounds.empty() && !opts.region->intersects(out.bounds)) return true;

        CFTypeRef children_ref = copy_attr(el, kAXChildrenAttribute);
        if (children_ref && CFGetTypeID(children_ref) == CFArrayGetTypeID()) {
            CFArrayRef children = static_cast<CFArrayRef>(children_ref);
            const CFIndex n = CFArrayGetCount(children);
            out.children.reserve(static_cast<std::size_t>(n));
            for (CFIndex i = 0; i < n; ++i) {
                AXUIElementRef c = static_cast<AXUIElementRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(children, i)));
                Node child;
                if (build(c, child, depth + 1, opts, pid, app_name, tree)) {
                    out.children.push_back(std::move(child));
                }
            }
        }
        if (children_ref) CFRelease(children_ref);
        return true;
    }

    Clock::time_point deadline_{};
    int nodes_left_ = 0;
};

}  // namespace

Result<std::unique_ptr<AccessibilityBackend>> AccessibilityBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<MacA11y>(std::move(displays));
    // Deliberately not calling initialize(): a caller that only wants
    // screenshots should not be blocked by a missing Accessibility grant.
    return std::unique_ptr<AccessibilityBackend>(std::move(backend));
}

}  // namespace cc
