// SPDX-License-Identifier: MIT
//
// Android emulators and physical devices, over adb.
//
// adb is the happy case of this whole layer: `input tap`, `input swipe`,
// `input text`, `input keyevent` and `uiautomator dump` cover everything, they
// work identically on emulators and physical hardware, and they do not need
// the device to be visible on screen. The onscreen path still exists because
// `input swipe` cannot express multi-touch, a freehand path, or pressure.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>

#include "devices/device_internal.hpp"

namespace cc::devices {
namespace {

using Ms = std::chrono::milliseconds;

std::string adb_path() {
    return which("adb");
}
bool adb_available() {
    return !adb_path().empty();
}

ExecResult adb(const std::string& serial, std::vector<std::string> args, Ms timeout = Ms{15000}) {
    std::vector<std::string> full;
    if (!serial.empty()) {
        full.push_back("-s");
        full.push_back(serial);
    }
    for (auto& a : args) full.push_back(std::move(a));
    return exec("adb", full, timeout);
}

std::string getprop(const std::string& serial, const std::string& key) {
    auto r = adb(serial, {"shell", "getprop", key}, Ms{6000});
    return (r.exit_code == 0) ? trim(r.out) : std::string{};
}

// `wm size` prints "Physical size: 1080x2400" and, when overridden,
// "Override size: 720x1600". The override is what apps actually see.
bool screen_size(const std::string& serial, double* w, double* h) {
    auto r = adb(serial, {"shell", "wm", "size"}, Ms{6000});
    if (r.exit_code != 0) return false;
    double pw = 0, ph = 0, ow = 0, oh = 0;
    for (const auto& line : split_lines(r.out)) {
        if (line.find("Physical size:") != std::string::npos) {
            std::sscanf(line.c_str(), "Physical size: %lfx%lf", &pw, &ph);
        } else if (line.find("Override size:") != std::string::npos) {
            std::sscanf(line.c_str(), "Override size: %lfx%lf", &ow, &oh);
        }
    }
    if (ow > 0 && oh > 0) {
        *w = ow;
        *h = oh;
        return true;
    }
    if (pw > 0 && ph > 0) {
        *w = pw;
        *h = ph;
        return true;
    }
    return false;
}

double screen_density(const std::string& serial) {
    auto r = adb(serial, {"shell", "wm", "density"}, Ms{6000});
    double physical = 0, override_d = 0;
    for (const auto& line : split_lines(r.out)) {
        if (line.find("Physical density:") != std::string::npos) {
            std::sscanf(line.c_str(), "Physical density: %lf", &physical);
        } else if (line.find("Override density:") != std::string::npos) {
            std::sscanf(line.c_str(), "Override density: %lf", &override_d);
        }
    }
    const double dpi = (override_d > 0) ? override_d : physical;
    return (dpi > 0) ? dpi / 160.0 : 1.0;  // Android's baseline is 160 dpi
}

Orientation orientation_of(const std::string& serial) {
    auto r =
        adb(serial, {"shell", "dumpsys", "input", "|", "grep", "SurfaceOrientation"}, Ms{6000});
    // The pipe above is interpreted by the device shell, which is fine, but
    // the reliable source is the display rotation property.
    auto r2 = adb(serial, {"shell", "dumpsys", "display"}, Ms{8000});
    const std::string& text = r2.exit_code == 0 ? r2.out : r.out;
    if (text.find("rotation=1") != std::string::npos ||
        text.find("rotation=3") != std::string::npos) {
        return Orientation::Landscape;
    }
    return Orientation::Portrait;
}

class AndroidSession final : public DeviceSession {
public:
    AndroidSession(DeviceInfo info, DeviceTransport requested,
                   std::shared_ptr<OnscreenDriver> driver)
        : info_(std::move(info)), requested_(requested), driver_(std::move(driver)) {
        active_ = (requested_ == DeviceTransport::Onscreen) ? DeviceTransport::Onscreen
                                                            : DeviceTransport::Bridge;
    }

    const DeviceInfo& info() const override { return info_; }
    DeviceTransport active_transport() const override { return active_; }
    const DeviceViewport& viewport() const override { return viewport_; }

    Status refresh() override {
        if (active_ == DeviceTransport::Bridge) return ok();
        if (!driver_) return err(ErrorCode::Unsupported, "no onscreen driver available");
        auto vp = driver_->locate(info_);
        if (!vp) return vp.error();
        viewport_ = vp.value();
        return ok();
    }

    Status tap(const Point& p, const DeviceTapOptions& opts) override {
        if (active_ == DeviceTransport::Onscreen) {
            if (auto st = refresh(); !st) return st;
            return driver_->tap(viewport_, p, opts);
        }
        if (opts.hold.count() > 0) {
            // A long press is a zero-distance swipe with a duration; there is
            // no `input longpress`.
            return run_swipe(p, p, opts.hold);
        }
        const int n = std::max(1, opts.count);
        for (int i = 0; i < n; ++i) {
            auto r = adb(info_.id, {"shell", "input", "tap", ivstr(p.x), ivstr(p.y)}, Ms{10000});
            if (r.exit_code != 0) {
                return err(ErrorCode::DeviceError, "adb input tap failed: " + trim(r.err));
            }
            // Below Android's ViewConfiguration double-tap timeout (300ms).
            if (i + 1 < n) exec("sleep", {"0.09"}, Ms{1000});
        }
        return ok();
    }

    Status swipe(const Point& from, const Point& to, const DeviceSwipeOptions& opts) override {
        if (active_ == DeviceTransport::Onscreen) {
            if (auto st = refresh(); !st) return st;
            return driver_->swipe(viewport_, from, to, opts);
        }
        if (opts.press_delay.count() > 0) {
            // Drag-and-drop: `input draganddrop` exists on API 29+ and is the
            // only bridge command that produces a real long-press-then-move.
            auto r = adb(info_.id,
                         {"shell", "input", "draganddrop", ivstr(from.x), ivstr(from.y),
                          ivstr(to.x), ivstr(to.y), ivstr(opts.duration.count())},
                         Ms{20000});
            if (r.exit_code == 0) return ok();
        }
        return run_swipe(from, to, opts.duration);
    }

    Status stroke(const std::vector<PathPoint>& path, const StrokeOptions& opts) override {
        // `input swipe` only takes two points, so a real freehand path needs
        // the onscreen route (or `sendevent`, which requires knowing the
        // device's input device node and is too fragile to rely on).
        if (auto st = force_onscreen(); !st) return st;
        return driver_->stroke(viewport_, path, opts);
    }

    Status gesture(const GestureRequest& req) override {
        if (req.kind == GestureKind::Swipe && req.fingers == 1 &&
            active_ == DeviceTransport::Bridge) {
            const Point d = motion_offset(req.direction, req.distance);
            return run_swipe(req.center, Point{req.center.x + d.x, req.center.y + d.y},
                             req.duration);
        }
        if (req.kind == GestureKind::LongPress && active_ == DeviceTransport::Bridge) {
            return run_swipe(req.center, req.center, req.hold.count() > 0 ? req.hold : Ms{700});
        }
        // Pinch, rotate and multi-finger swipes have no adb equivalent.
        if (auto st = force_onscreen(); !st) return st;
        return driver_->gesture(viewport_, req);
    }

    Status type_text(std::string_view utf8) override {
        if (active_ == DeviceTransport::Onscreen) {
            if (auto st = refresh(); !st) return st;
            return driver_->type_text(utf8);
        }
        // `input text` treats %s as a space and chokes on shell metacharacters
        // even through the argv path, because the device-side shell re-parses
        // the argument. Percent-encoding spaces and quoting the rest is the
        // standard workaround.
        std::string encoded;
        encoded.reserve(utf8.size() * 2);
        for (char c : utf8) {
            switch (c) {
                case ' ': encoded += "%s"; break;
                case '\n':
                    // Flush what we have, then send Enter as a keyevent.
                    if (!encoded.empty()) {
                        if (auto st = send_text(encoded); !st) return st;
                        encoded.clear();
                    }
                    if (auto st = press_button("enter"); !st) return st;
                    break;
                default: encoded.push_back(c);
            }
        }
        if (!encoded.empty()) return send_text(encoded);
        return ok();
    }

    Status press_button(std::string_view name) override {
        const std::string key = keycode_for(name);
        if (key.empty()) {
            return err(ErrorCode::InvalidArgument,
                       "unknown Android button '" + std::string(name) + "'",
                       "Supported: home, back, menu, power, enter, tab, delete, search, "
                       "volumeup, volumedown, mute, camera, appswitch, wakeup, sleep.");
        }
        auto r = adb(info_.id, {"shell", "input", "keyevent", key}, Ms{10000});
        if (r.exit_code != 0) {
            return err(ErrorCode::DeviceError, "adb keyevent failed: " + trim(r.err));
        }
        return ok();
    }

    Result<Frame> screenshot() override {
        if (active_ == DeviceTransport::Onscreen) {
            if (auto st = refresh(); !st) return st.error();
            return driver_->screenshot(viewport_);
        }
        // `exec-out` (not `shell`) is essential: `shell` mangles binary data by
        // translating LF to CRLF on some adb versions, which corrupts the PNG.
        auto r = adb(info_.id, {"exec-out", "screencap", "-p"}, Ms{30000});
        if (r.exit_code != 0 || r.out.size() < 8) {
            return err(ErrorCode::DeviceError, "screencap failed: " + trim(r.err));
        }
        std::vector<std::uint8_t> bytes(r.out.begin(), r.out.end());
        return decode_image(bytes);
    }

    Result<std::string> shell(std::string_view command) override {
        auto r = adb(info_.id, {"shell", std::string(command)}, Ms{30000});
        if (r.exit_code != 0 && r.out.empty()) {
            return err(ErrorCode::DeviceError, "adb shell failed: " + trim(r.err));
        }
        return r.out;
    }

    Status install_app(std::string_view path) override {
        auto r = adb(info_.id, {"install", "-r", "-g", std::string(path)}, Ms{180000});
        if (r.exit_code != 0 || r.out.find("Success") == std::string::npos) {
            return err(ErrorCode::DeviceError,
                       "install failed: " + trim(r.out.empty() ? r.err : r.out));
        }
        return ok();
    }

    Status launch_app(std::string_view package) override {
        // monkey is crude but it resolves the launcher activity itself, which
        // `am start` cannot do from a package name alone.
        auto r = adb(info_.id,
                     {"shell", "monkey", "-p", std::string(package), "-c",
                      "android.intent.category.LAUNCHER", "1"},
                     Ms{20000});
        if (r.exit_code != 0 || r.out.find("No activities found") != std::string::npos) {
            return err(ErrorCode::DeviceError, "could not launch '" + std::string(package) + "'",
                       "Check the package name with: adb shell pm list packages | grep <name>");
        }
        return ok();
    }

    Status terminate_app(std::string_view package) override {
        auto r = adb(info_.id, {"shell", "am", "force-stop", std::string(package)}, Ms{15000});
        if (r.exit_code != 0)
            return err(ErrorCode::DeviceError, "force-stop failed: " + trim(r.err));
        return ok();
    }

    Status open_url(std::string_view url) override {
        auto r = adb(
            info_.id,
            {"shell", "am", "start", "-a", "android.intent.action.VIEW", "-d", std::string(url)},
            Ms{20000});
        if (r.exit_code != 0) return err(ErrorCode::DeviceError, "am start failed: " + trim(r.err));
        return ok();
    }

    Result<Tree> ui_tree(const TreeOptions& opts) override {
        // `--compressed` prunes layout-only containers, which cuts the dump
        // size several-fold with no loss of interactive nodes.
        auto r = adb(info_.id, {"exec-out", "uiautomator", "dump", "--compressed", "/dev/tty"},
                     Ms{30000});
        std::string xml = r.out;
        if (xml.find("<hierarchy") == std::string::npos) {
            // Older devices refuse /dev/tty; fall back to a file round trip.
            adb(info_.id, {"shell", "uiautomator", "dump", "/sdcard/cc_dump.xml"}, Ms{30000});
            auto cat = adb(info_.id, {"exec-out", "cat", "/sdcard/cc_dump.xml"}, Ms{20000});
            xml = cat.out;
        }
        if (xml.find("<hierarchy") == std::string::npos) {
            return err(ErrorCode::DeviceError, "uiautomator dump produced no hierarchy",
                       "Some secure screens (keyguard, payment sheets) block dumping.");
        }
        return parse_uiautomator(xml, opts);
    }

private:
    static std::string ivstr(double v) {
        return std::to_string(static_cast<long long>(std::lround(v)));
    }

    static Point motion_offset(SwipeDirection d, double dist) {
        switch (d) {
            case SwipeDirection::Up: return Point{0, -dist};
            case SwipeDirection::Down: return Point{0, dist};
            case SwipeDirection::Left: return Point{-dist, 0};
            case SwipeDirection::Right: return Point{dist, 0};
        }
        return Point{0, 0};
    }

    Status force_onscreen() {
        if (!driver_) {
            return err(ErrorCode::Unsupported,
                       "this gesture needs the onscreen transport, but no window backend is "
                       "available",
                       "adb cannot express multi-touch or freehand paths. Show the emulator "
                       "window and retry, or use scrcpy to mirror a physical device.");
        }
        const auto saved = active_;
        active_ = DeviceTransport::Onscreen;
        auto st = refresh();
        if (!st) active_ = saved;
        return st;
    }

    Status send_text(const std::string& encoded) {
        auto r = adb(info_.id, {"shell", "input", "text", encoded}, Ms{20000});
        if (r.exit_code != 0) {
            return err(ErrorCode::DeviceError, "adb input text failed: " + trim(r.err),
                       "Non-ASCII text cannot be sent with `input text`. Install and select "
                       "the ADBKeyBoard IME, or use the onscreen transport.");
        }
        return ok();
    }

    Status run_swipe(const Point& from, const Point& to, Ms duration) {
        auto r = adb(info_.id,
                     {"shell", "input", "swipe", ivstr(from.x), ivstr(from.y), ivstr(to.x),
                      ivstr(to.y), ivstr(static_cast<double>(duration.count()))},
                     Ms{std::max<long long>(15000, duration.count() + 8000)});
        if (r.exit_code != 0) {
            return err(ErrorCode::DeviceError, "adb input swipe failed: " + trim(r.err));
        }
        return ok();
    }

    static std::string keycode_for(std::string_view name) {
        std::string k(name);
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "home") return "KEYCODE_HOME";
        if (k == "back") return "KEYCODE_BACK";
        if (k == "menu") return "KEYCODE_MENU";
        if (k == "power") return "KEYCODE_POWER";
        if (k == "enter" || k == "return") return "KEYCODE_ENTER";
        if (k == "tab") return "KEYCODE_TAB";
        if (k == "delete" || k == "backspace") return "KEYCODE_DEL";
        if (k == "search") return "KEYCODE_SEARCH";
        if (k == "volumeup") return "KEYCODE_VOLUME_UP";
        if (k == "volumedown") return "KEYCODE_VOLUME_DOWN";
        if (k == "mute") return "KEYCODE_VOLUME_MUTE";
        if (k == "camera") return "KEYCODE_CAMERA";
        if (k == "appswitch" || k == "recents") return "KEYCODE_APP_SWITCH";
        if (k == "wakeup") return "KEYCODE_WAKEUP";
        if (k == "sleep") return "KEYCODE_SLEEP";
        return {};
    }

    // Minimal XML scraper for uiautomator output. A full parser is unnecessary:
    // the dump is machine-generated, flat attribute syntax with no namespaces,
    // entities beyond the standard five, or mixed content.
    Result<Tree> parse_uiautomator(const std::string& xml, const TreeOptions& opts) {
        Tree tree;
        std::size_t pos = 0;
        while ((pos = xml.find("<node ", pos)) != std::string::npos) {
            const std::size_t end = xml.find('>', pos);
            if (end == std::string::npos) break;
            const std::string tag = xml.substr(pos, end - pos);
            pos = end + 1;

            auto attr = [&](const char* key) -> std::string {
                const std::string needle = std::string(key) + "=\"";
                const auto a = tag.find(needle);
                if (a == std::string::npos) return {};
                const auto b = tag.find('"', a + needle.size());
                if (b == std::string::npos) return {};
                std::string v = tag.substr(a + needle.size(), b - a - needle.size());
                // Unescape the five predefined entities.
                const std::pair<const char*, const char*> subs[] = {{"&amp;", "&"},
                                                                    {"&lt;", "<"},
                                                                    {"&gt;", ">"},
                                                                    {"&quot;", "\""},
                                                                    {"&apos;", "'"}};
                for (const auto& [from, to] : subs) {
                    std::size_t i = 0;
                    while ((i = v.find(from, i)) != std::string::npos) {
                        v.replace(i, std::strlen(from), to);
                        i += std::strlen(to);
                    }
                }
                return v;
            };

            Node n;
            n.name = attr("text");
            if (n.name.empty()) n.name = attr("content-desc");
            n.value = attr("text");
            n.raw_role = attr("class");
            n.automation_id = attr("resource-id");
            n.enabled = attr("enabled") == "true";
            n.focused = attr("focused") == "true";
            const bool clickable = attr("clickable") == "true";
            const bool long_clickable = attr("long-clickable") == "true";
            const bool editable = n.raw_role.find("EditText") != std::string::npos;
            n.scrollable = attr("scrollable") == "true";
            if (attr("checkable") == "true") n.checked = (attr("checked") == "true");
            n.selected = (attr("selected") == "true");

            // bounds="[x1,y1][x2,y2]" in device pixels.
            const std::string b = attr("bounds");
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (std::sscanf(b.c_str(), "[%d,%d][%d,%d]", &x1, &y1, &x2, &y2) == 4) {
                n.bounds =
                    Rect{double(x1), double(y1), double(x2 - x1), double(y2 - y1), Space::Logical};
            }
            n.visible = !n.bounds.empty();
            n.interactive = n.enabled && n.visible && (clickable || long_clickable || editable);

            if (n.raw_role.find("Button") != std::string::npos)
                n.role = Role::Button;
            else if (editable)
                n.role = Role::TextField;
            else if (n.raw_role.find("CheckBox") != std::string::npos)
                n.role = Role::CheckBox;
            else if (n.raw_role.find("ImageView") != std::string::npos)
                n.role = Role::Image;
            else if (n.raw_role.find("TextView") != std::string::npos)
                n.role = Role::StaticText;
            else if (n.raw_role.find("RecyclerView") != std::string::npos)
                n.role = Role::List;
            else if (n.scrollable)
                n.role = Role::ScrollArea;
            else
                n.role = Role::Group;

            if (opts.interactive_only && !n.interactive && !opts.include_static_text) continue;
            tree.roots.push_back(std::move(n));
            if (static_cast<int>(tree.roots.size()) >= opts.max_nodes) {
                tree.truncated = true;
                tree.truncation_reason = "node budget exhausted";
                break;
            }
        }
        assign_labels(tree, opts);
        return tree;
    }

    DeviceInfo info_;
    DeviceTransport requested_;
    DeviceTransport active_;
    std::shared_ptr<OnscreenDriver> driver_;
    DeviceViewport viewport_;
};

}  // namespace

Result<std::vector<DeviceInfo>> list_android_devices(bool booted_only) {
    std::vector<DeviceInfo> out;
    if (!adb_available()) return out;

    auto r = exec("adb", {"devices", "-l"}, Ms{15000});
    if (r.exit_code != 0) return err(ErrorCode::DeviceError, "adb devices failed: " + trim(r.err));

    for (const auto& line : split_lines(r.out)) {
        const std::string t = trim(line);
        if (t.empty() || t.rfind("List of devices", 0) == 0 || t.rfind("*", 0) == 0) continue;

        std::istringstream iss(t);
        std::string serial, state;
        iss >> serial >> state;
        if (serial.empty()) continue;

        const bool online = (state == "device");
        if (booted_only && !online) continue;

        DeviceInfo info;
        info.id = serial;
        info.platform = DevicePlatform::Android;
        info.kind =
            (serial.rfind("emulator-", 0) == 0) ? DeviceKind::Emulator : DeviceKind::Physical;
        info.booted = online;

        // The rest of the line carries `model:Pixel_8 device:shiba`.
        std::string token;
        std::string model;
        while (iss >> token) {
            if (token.rfind("model:", 0) == 0) model = token.substr(6);
        }
        std::replace(model.begin(), model.end(), '_', ' ');
        info.name = model.empty() ? serial : model;

        if (online) {
            info.os_version = getprop(serial, "ro.build.version.release");
            double w = 0, h = 0;
            if (screen_size(serial, &w, &h)) {
                info.screen_points = Size{w, h, Space::Logical};
            }
            info.device_scale = screen_density(serial);
            info.orientation = orientation_of(serial);
            if (info.kind == DeviceKind::Emulator) {
                const std::string avd = getprop(serial, "ro.boot.qemu.avd_name");
                if (!avd.empty()) {
                    std::string pretty = avd;
                    std::replace(pretty.begin(), pretty.end(), '_', ' ');
                    info.name = pretty;
                }
            }
        }

        info.available_transports.push_back("bridge");
        if (online) info.available_transports.push_back("onscreen");
        out.push_back(std::move(info));
    }
    return out;
}

Result<std::shared_ptr<DeviceSession>> open_android(const DeviceInfo& info,
                                                    DeviceTransport transport,
                                                    std::shared_ptr<OnscreenDriver> driver) {
    if (!adb_available()) {
        return err(ErrorCode::Unsupported, "adb is not on PATH",
                   "Install Android platform-tools and add them to PATH: "
                   "https://developer.android.com/tools/releases/platform-tools");
    }
    if (!info.booted) {
        return err(ErrorCode::DeviceError, "device '" + info.name + "' is offline or unauthorized",
                   "Check `adb devices`; an 'unauthorized' state means the USB debugging "
                   "prompt on the device has not been accepted.");
    }
    auto session = std::make_shared<AndroidSession>(info, transport, std::move(driver));
    return std::static_pointer_cast<DeviceSession>(session);
}

}  // namespace cc::devices
