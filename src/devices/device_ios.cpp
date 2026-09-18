// SPDX-License-Identifier: MIT
//
// iOS / iPadOS / tvOS / watchOS simulators, and mirrored physical devices.
//
// The awkward truth about simctl: it can boot, install, launch, open URLs and
// take screenshots, but it has **no public tap, swipe or text-entry command**.
// Apple never shipped one. Three ways around it, in preference order:
//
//   1. idb (Meta's iOS Device Bridge) if installed - real `idb ui tap`,
//      `ui swipe`, `ui text`, and an accessibility tree. Best fidelity.
//   2. Onscreen - drive the Simulator.app window with host mouse and keyboard.
//      Always available when the window is visible, and the only option for
//      iPhone Mirroring or QuickTime device mirroring.
//   3. simctl for everything it does cover, regardless of which of the above
//      handles input.
//
// The default transport mixes them: bridge for lifecycle and screenshots,
// onscreen for input when idb is absent.

#include <algorithm>
#include <cmath>
#include <map>

#include "core/json.hpp"
#include "devices/device_internal.hpp"

namespace cc::devices {
namespace {

using Ms = std::chrono::milliseconds;

// Device screen sizes in points, keyed by a substring of the simulator name.
// simctl reports a device *type* but not its screen geometry, and the
// coordinate mapping is worthless without it. Ordered longest-match-first so
// "iPhone 15 Pro Max" does not match the "iPhone 15 Pro" entry.
struct Metrics {
    const char* match;
    double w, h;
    double scale;
};
const Metrics kMetrics[] = {
    {"iPhone 17 Pro Max", 440, 956, 3.0},
    {"iPhone 17 Pro", 402, 874, 3.0},
    {"iPhone 16 Pro Max", 440, 956, 3.0},
    {"iPhone 16 Pro", 402, 874, 3.0},
    {"iPhone 16 Plus", 430, 932, 3.0},
    {"iPhone 16e", 390, 844, 3.0},
    {"iPhone 16", 393, 852, 3.0},
    {"iPhone 15 Pro Max", 430, 932, 3.0},
    {"iPhone 15 Pro", 393, 852, 3.0},
    {"iPhone 15 Plus", 430, 932, 3.0},
    {"iPhone 15", 393, 852, 3.0},
    {"iPhone 14 Pro Max", 430, 932, 3.0},
    {"iPhone 14 Pro", 393, 852, 3.0},
    {"iPhone 14 Plus", 428, 926, 3.0},
    {"iPhone 14", 390, 844, 3.0},
    {"iPhone 13 mini", 375, 812, 3.0},
    {"iPhone 13", 390, 844, 3.0},
    {"iPhone 12 mini", 375, 812, 3.0},
    {"iPhone 12", 390, 844, 3.0},
    {"iPhone 11 Pro Max", 414, 896, 3.0},
    {"iPhone 11 Pro", 375, 812, 3.0},
    {"iPhone 11", 414, 896, 2.0},
    {"iPhone SE", 375, 667, 2.0},
    {"iPhone XR", 414, 896, 2.0},
    {"iPhone X", 375, 812, 3.0},
    {"iPad Pro 13", 1032, 1376, 2.0},
    {"iPad Pro 12.9", 1024, 1366, 2.0},
    {"iPad Pro 11", 834, 1194, 2.0},
    {"iPad Air 13", 1024, 1366, 2.0},
    {"iPad Air 11", 820, 1180, 2.0},
    {"iPad Air", 820, 1180, 2.0},
    {"iPad mini", 744, 1133, 2.0},
    {"iPad", 810, 1080, 2.0},
    {"Apple TV", 1920, 1080, 1.0},
    {"Apple Watch Ultra", 205, 251, 2.0},
    {"Apple Watch", 176, 215, 2.0},
};

void apply_metrics_impl(DeviceInfo& d) {
    for (const auto& m : kMetrics) {
        if (d.name.find(m.match) != std::string::npos) {
            d.screen_points = Size{m.w, m.h, Space::Logical};
            d.device_scale = m.scale;
            return;
        }
    }
    // Unknown device: leave the size at zero. The onscreen driver then treats
    // the window content as 1:1, which is wrong but predictable, and the
    // Device tool reports the metrics as unknown instead of inventing them.
}

DevicePlatform platform_for(const std::string& name, const std::string& runtime) {
    if (name.find("iPad") != std::string::npos) return DevicePlatform::IPadOS;
    if (name.find("Apple TV") != std::string::npos) return DevicePlatform::TvOS;
    if (name.find("Watch") != std::string::npos) return DevicePlatform::WatchOS;
    if (runtime.find("tvOS") != std::string::npos) return DevicePlatform::TvOS;
    if (runtime.find("watchOS") != std::string::npos) return DevicePlatform::WatchOS;
    return DevicePlatform::IOS;
}

std::string runtime_version(const std::string& runtime_id) {
    // "com.apple.CoreSimulator.SimRuntime.iOS-18-0" -> "18.0"
    const auto pos = runtime_id.rfind('.');
    if (pos == std::string::npos) return runtime_id;
    std::string tail = runtime_id.substr(pos + 1);
    const auto dash = tail.find('-');
    if (dash == std::string::npos) return tail;
    std::string version = tail.substr(dash + 1);
    std::replace(version.begin(), version.end(), '-', '.');
    return version;
}

bool simctl_available() {
    static const bool available = [] {
        if (!have("xcrun")) return false;
        auto r = exec("xcrun", {"simctl", "help"}, Ms{8000});
        return r.exit_code == 0;
    }();
    return available;
}

// ---------------------------------------------------------------------------

class IosSession final : public DeviceSession {
public:
    IosSession(DeviceInfo info, DeviceTransport requested, std::shared_ptr<OnscreenDriver> driver)
        : info_(std::move(info)), requested_(requested), driver_(std::move(driver)) {
        use_idb_ = have("idb");
        resolve_transport();
    }

    const DeviceInfo& info() const override { return info_; }
    DeviceTransport active_transport() const override { return active_; }
    const DeviceViewport& viewport() const override { return viewport_; }

    Status refresh() override {
        if (active_ != DeviceTransport::Onscreen && !needs_onscreen_input()) return ok();
        if (!driver_) return err(ErrorCode::Unsupported, "no onscreen driver available");
        auto vp = driver_->locate(info_);
        if (!vp) return vp.error();
        viewport_ = vp.value();
        if (viewport_.effective_scale() < 0.75) {
            // Not fatal, but worth knowing: at this scale a 1pt target is
            // under one host pixel and taps get rounded onto neighbours.
            last_warning_ = "the simulator window is displayed at " +
                            std::to_string(static_cast<int>(viewport_.effective_scale() * 100)) +
                            "% of device size; small targets may be missed. Use Window > "
                            "Physical Size in Simulator, or switch to transport=\"bridge\".";
        }
        return ok();
    }

    Status tap(const Point& p, const DeviceTapOptions& opts) override {
        if (use_idb_ && active_ == DeviceTransport::Bridge) {
            std::vector<std::string> args{"ui", "tap", "--udid", info_.id, fmt(p.x), fmt(p.y)};
            if (opts.hold.count() > 0) {
                args.push_back("--duration");
                args.push_back(fmt(opts.hold.count() / 1000.0));
            }
            auto r = exec("idb", args, Ms{10000});
            if (r.exit_code == 0) return ok();
            // Fall through to onscreen: idb commonly fails when its companion
            // process has died, and a working fallback beats a hard error.
        }
        if (auto st = refresh(); !st) return st;
        return driver_->tap(viewport_, p, opts);
    }

    Status swipe(const Point& from, const Point& to, const DeviceSwipeOptions& opts) override {
        if (use_idb_ && active_ == DeviceTransport::Bridge) {
            auto r = exec("idb",
                          {"ui", "swipe", "--udid", info_.id, fmt(from.x), fmt(from.y), fmt(to.x),
                           fmt(to.y), "--duration", fmt(opts.duration.count() / 1000.0)},
                          Ms{15000});
            if (r.exit_code == 0) return ok();
        }
        if (auto st = refresh(); !st) return st;
        return driver_->swipe(viewport_, from, to, opts);
    }

    Status stroke(const std::vector<PathPoint>& path, const StrokeOptions& opts) override {
        // idb has no freehand path command, so this is always onscreen.
        if (auto st = refresh(); !st) return st;
        return driver_->stroke(viewport_, path, opts);
    }

    Status gesture(const GestureRequest& req) override {
        if (auto st = refresh(); !st) return st;
        return driver_->gesture(viewport_, req);
    }

    Status type_text(std::string_view utf8) override {
        if (use_idb_) {
            auto r = exec("idb", {"ui", "text", "--udid", info_.id, std::string(utf8)}, Ms{20000});
            if (r.exit_code == 0) return ok();
        }
        if (auto st = refresh(); !st) return st;
        // Hardware keyboard must be connected in the Simulator for host
        // keystrokes to reach the device (cmd+shift+K toggles it).
        return driver_->type_text(utf8);
    }

    Status press_button(std::string_view name) override {
        static const std::map<std::string, std::string> kSimctlButtons = {
            {"home", "home"}, {"lock", "lock"},           {"power", "lock"},
            {"siri", "siri"}, {"apple-pay", "apple-pay"}, {"side", "side"},
        };
        const std::string key(name);
        auto it = kSimctlButtons.find(key);
        if (it != kSimctlButtons.end() && simctl_available()) {
            auto r = exec("xcrun", {"simctl", "ui", info_.id, "button", it->second}, Ms{8000});
            if (r.exit_code == 0) return ok();
            // `simctl ui ... button` only exists on newer Xcode; fall back to
            // the Simulator menu's keyboard equivalents.
        }
        if (key == "home") {
            if (auto st = refresh(); !st) return st;
            Chord c;
            c.modifiers = Modifier::Meta | Modifier::Shift;
            c.keys.push_back(Key::H);
            return driver_->input()->tap_chord(c, 1);
        }
        return err(ErrorCode::Unsupported, "unsupported button '" + key + "' for an iOS simulator",
                   "Supported: home, lock, power, siri, side.");
    }

    Result<Frame> screenshot() override {
        if (simctl_available()) {
            // `--type=png -` writes to stdout, avoiding a temp file entirely.
            auto r = exec("xcrun", {"simctl", "io", info_.id, "screenshot", "--type=png", "-"},
                          Ms{20000});
            if (r.exit_code == 0 && r.out.size() > 8) {
                std::vector<std::uint8_t> bytes(r.out.begin(), r.out.end());
                auto f = decode_image(bytes);
                if (f) return f;
            }
        }
        if (auto st = refresh(); !st) return st.error();
        return driver_->screenshot(viewport_);
    }

    Result<std::string> shell(std::string_view command) override {
        if (!simctl_available()) return no_simctl();
        auto r = exec("xcrun", {"simctl", "spawn", info_.id, "/bin/sh", "-c", std::string(command)},
                      Ms{30000});
        if (r.exit_code != 0 && r.out.empty()) {
            return err(ErrorCode::DeviceError, "simctl spawn failed: " + trim(r.err));
        }
        return r.out;
    }

    Status install_app(std::string_view path) override {
        if (!simctl_available()) return no_simctl().error();
        auto r = exec("xcrun", {"simctl", "install", info_.id, std::string(path)}, Ms{120000});
        if (r.exit_code != 0) {
            return err(ErrorCode::DeviceError, "install failed: " + trim(r.err),
                       "The path must be a .app bundle built for the simulator (x86_64/arm64 "
                       "simulator slice), not a device .ipa.");
        }
        return ok();
    }

    Status launch_app(std::string_view bundle) override {
        if (!simctl_available()) return no_simctl().error();
        auto r = exec("xcrun", {"simctl", "launch", info_.id, std::string(bundle)}, Ms{30000});
        if (r.exit_code != 0) {
            return err(ErrorCode::DeviceError, "launch failed: " + trim(r.err));
        }
        return ok();
    }

    Status terminate_app(std::string_view bundle) override {
        if (!simctl_available()) return no_simctl().error();
        auto r = exec("xcrun", {"simctl", "terminate", info_.id, std::string(bundle)}, Ms{15000});
        if (r.exit_code != 0)
            return err(ErrorCode::DeviceError, "terminate failed: " + trim(r.err));
        return ok();
    }

    Status open_url(std::string_view url) override {
        if (!simctl_available()) return no_simctl().error();
        auto r = exec("xcrun", {"simctl", "openurl", info_.id, std::string(url)}, Ms{15000});
        if (r.exit_code != 0) return err(ErrorCode::DeviceError, "openurl failed: " + trim(r.err));
        return ok();
    }

    Result<Tree> ui_tree(const TreeOptions& opts) override {
        if (!use_idb_) {
            return err(ErrorCode::Unsupported,
                       "an iOS simulator accessibility tree needs idb, which is not installed",
                       "brew tap facebook/fb && brew install idb-companion, then "
                       "pip install fb-idb. Without it, use a screenshot plus OCR, or drive "
                       "the simulator through its own XCUITest target.");
        }
        auto r = exec("idb", {"ui", "describe-all", "--udid", info_.id, "--json"}, Ms{30000});
        if (r.exit_code != 0) {
            return err(ErrorCode::DeviceError, "idb ui describe-all failed: " + trim(r.err));
        }

        Tree tree;
        // idb emits one JSON object per line rather than a single array.
        for (const auto& line : split_lines(r.out)) {
            if (trim(line).empty()) continue;
            json::ParseError pe;
            json::Value v = json::parse(line, &pe);
            if (!pe.ok || !v.is_object()) continue;
            Node n;
            n.name = v["AXLabel"].as_string();
            n.value = v["AXValue"].as_string();
            n.raw_role = v["type"].as_string();
            n.enabled = v["enabled"].as_bool(true);
            const auto& fr = v["frame"];
            n.bounds = Rect{fr["x"].as_double(), fr["y"].as_double(), fr["width"].as_double(),
                            fr["height"].as_double(), Space::Logical};
            n.interactive =
                n.enabled && !n.bounds.empty() &&
                (n.raw_role == "Button" || n.raw_role == "TextField" || n.raw_role == "Link" ||
                 n.raw_role == "Cell" || n.raw_role == "SearchField" || n.raw_role == "Switch");
            n.visible = !n.bounds.empty();
            tree.roots.push_back(std::move(n));
        }
        assign_labels(tree, opts);
        return tree;
    }

    const std::string& warning() const { return last_warning_; }

private:
    static std::string fmt(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return buf;
    }

    Result<std::string> no_simctl() {
        return err(ErrorCode::Unsupported, "xcrun simctl is not available",
                   "Install Xcode and run `xcode-select --install`, or point xcode-select at a "
                   "full Xcode with `sudo xcode-select -s /Applications/Xcode.app`.");
    }

    bool needs_onscreen_input() const { return !use_idb_; }

    void resolve_transport() {
        switch (requested_) {
            case DeviceTransport::Onscreen: active_ = DeviceTransport::Onscreen; return;
            case DeviceTransport::Bridge: active_ = DeviceTransport::Bridge; return;
            case DeviceTransport::Auto:
                active_ = (use_idb_ || simctl_available()) ? DeviceTransport::Bridge
                                                           : DeviceTransport::Onscreen;
                return;
        }
    }

    DeviceInfo info_;
    DeviceTransport requested_;
    DeviceTransport active_ = DeviceTransport::Auto;
    std::shared_ptr<OnscreenDriver> driver_;
    DeviceViewport viewport_;
    bool use_idb_ = false;
    std::string last_warning_;
};

}  // namespace

void apply_ios_metrics(DeviceInfo& info) {
    apply_metrics_impl(info);
}

Result<std::vector<DeviceInfo>> list_ios_devices(bool booted_only) {
    std::vector<DeviceInfo> out;
    if (!simctl_available()) return out;

    auto r = exec("xcrun", {"simctl", "list", "devices", "--json"}, Ms{20000});
    if (r.exit_code != 0) {
        return err(ErrorCode::DeviceError, "simctl list failed: " + trim(r.err));
    }

    json::ParseError pe;
    json::Value root = json::parse(r.out, &pe);
    if (!pe.ok) return err(ErrorCode::DeviceError, "could not parse simctl output: " + pe.message);

    const auto& devices_by_runtime = root["devices"].as_object();
    for (const auto& [runtime_id, list] : devices_by_runtime) {
        for (const auto& d : list.as_array()) {
            const bool booted = d["state"].as_string() == "Booted";
            if (booted_only && !booted) continue;
            // Unavailable devices are runtimes Xcode has since removed; they
            // cannot be booted and would only clutter the list.
            if (d.contains("isAvailable") && !d["isAvailable"].as_bool(true)) continue;

            DeviceInfo info;
            info.id = d["udid"].as_string();
            info.name = d["name"].as_string();
            info.kind = DeviceKind::Simulator;
            info.booted = booted;
            info.os_version = runtime_version(runtime_id);
            info.platform = platform_for(info.name, runtime_id);
            apply_metrics_impl(info);
            info.available_transports.push_back("bridge");
            if (booted) info.available_transports.push_back("onscreen");
            if (have("idb")) info.available_transports.push_back("idb");
            out.push_back(std::move(info));
        }
    }
    return out;
}

Result<std::shared_ptr<DeviceSession>> open_ios(const DeviceInfo& info, DeviceTransport transport,
                                                std::shared_ptr<OnscreenDriver> driver) {
    if (!info.booted && transport != DeviceTransport::Bridge) {
        return err(ErrorCode::DeviceError, "device '" + info.name + "' is not booted",
                   "Boot it first: xcrun simctl boot " + info.id + " && open -a Simulator");
    }
    auto session = std::make_shared<IosSession>(info, transport, std::move(driver));
    return std::static_pointer_cast<DeviceSession>(session);
}

Status boot_ios(std::string_view id) {
    if (!simctl_available()) {
        return err(ErrorCode::Unsupported, "xcrun simctl is not available");
    }
    auto r = exec("xcrun", {"simctl", "boot", std::string(id)}, Ms{90000});
    // "Unable to boot device in current state: Booted" is success for our
    // purposes, and treating it as an error makes boot non-idempotent.
    if (r.exit_code != 0 && r.err.find("current state: Booted") == std::string::npos) {
        return err(ErrorCode::DeviceError, "boot failed: " + trim(r.err));
    }
    exec("open", {"-a", "Simulator"}, Ms{10000});
    return ok();
}

Status shutdown_ios(std::string_view id) {
    if (!simctl_available()) {
        return err(ErrorCode::Unsupported, "xcrun simctl is not available");
    }
    auto r = exec("xcrun", {"simctl", "shutdown", std::string(id)}, Ms{30000});
    if (r.exit_code != 0 && r.err.find("current state: Shutdown") == std::string::npos) {
        return err(ErrorCode::DeviceError, "shutdown failed: " + trim(r.err));
    }
    return ok();
}

}  // namespace cc::devices
