// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cc/accessibility.hpp"
#include "cc/input.hpp"
#include "cc/screen.hpp"
#include "cc/types.hpp"
#include "cc/window.hpp"

namespace cc {

// ---------------------------------------------------------------------------
// Mobile devices shown on the host screen
// ---------------------------------------------------------------------------
//
// An iOS Simulator, an Android emulator, a physical phone mirrored by scrcpy
// or QuickTime, and iPhone Mirroring all present the same problem: the thing
// you want to drive is a rectangle inside a host window, its internal
// coordinate system is not the host's, and its scale factor is neither the
// host's nor 1.0. An iPhone 15 Pro simulator on a Retina Mac is a 393x852pt
// device, rendered at 3x into a window that macOS then draws at 2x and which
// the user may have scaled to 75%.
//
// Two transports, and the right one depends on what you need:
//
//   Bridge  - talk to the device's own tooling (simctl, idb, adb). Exact
//             coordinates, no window hunting, works when the window is
//             occluded or the simulator is headless. Cannot drive host chrome,
//             and simctl has no public tap/swipe (idb or adb do).
//   Onscreen- find the device's viewport inside its host window and translate
//             device points into host points, then use ordinary host input.
//             Works for anything visible including iPhone Mirroring and
//             scrcpy, survives tooling gaps, and is the only option for
//             devices with no CLI. Needs the window visible and unoccluded.
//
// The default is Auto: use the bridge for anything it does well (install,
// launch, screenshots, text entry, deep links) and fall back to onscreen for
// gestures the bridge cannot express.

enum class DevicePlatform : std::uint8_t { Unknown = 0, IOS, IPadOS, TvOS, WatchOS, Android };
enum class DeviceKind : std::uint8_t { Simulator = 0, Emulator, Physical, Mirrored };
enum class DeviceTransport : std::uint8_t { Auto = 0, Bridge, Onscreen };

const char* to_string(DevicePlatform p) noexcept;
const char* to_string(DeviceKind k) noexcept;

struct DeviceInfo {
    std::string id;    // simctl UDID, adb serial, or "onscreen:<window id>"
    std::string name;  // "iPhone 15 Pro", "Pixel 8 API 34"
    DevicePlatform platform = DevicePlatform::Unknown;
    DeviceKind kind = DeviceKind::Simulator;
    std::string os_version;
    bool booted = false;

    // The device's own screen, in its own units.
    Size screen_points{0, 0, Space::Logical};  // e.g. 393 x 852
    double device_scale = 1.0;                 // e.g. 3.0 on an iPhone Pro
    Orientation orientation = Orientation::Portrait;

    // Set when the device is visible on the host screen.
    std::optional<std::uint64_t> host_window_id;
    // The device's viewport within the host window, in host logical points.
    // Excludes simulator chrome: bezel, title bar, and the rounded-corner mask.
    std::optional<Rect> host_viewport;

    std::vector<std::string> available_transports;
};

// Maps between device points and host logical points. Kept separate from
// DisplayGraph because the device adds its own scale on top of the host's, and
// because the viewport moves whenever the user drags the simulator window.
class DeviceViewport {
public:
    DeviceViewport() = default;
    DeviceViewport(Rect host_rect, Size device_points, Orientation o = Orientation::Portrait);

    bool valid() const noexcept { return valid_; }

    Point to_host(const Point& device_point) const;
    Point to_device(const Point& host_point) const;
    Rect to_host(const Rect& device_rect) const;

    // host_px per device_pt. Below ~1.0 the simulator is rendered smaller than
    // its logical size and single-pixel targets become unreliable; the Device
    // tool warns when this drops under 0.75.
    double effective_scale() const noexcept { return sx_; }

    const Rect& host_rect() const noexcept { return host_; }
    const Size& device_size() const noexcept { return device_; }

private:
    // Orientation is applied by the caller (which swaps the device width and
    // height before constructing the viewport), so it is not stored here.
    Rect host_{};
    Size device_{};
    double sx_ = 1.0, sy_ = 1.0;
    bool valid_ = false;
};

struct DeviceTapOptions {
    int count = 1;
    std::chrono::milliseconds hold{0};
    int fingers = 1;
};

struct DeviceSwipeOptions {
    std::chrono::milliseconds duration{300};
    int fingers = 1;
    // Extra dwell at the start, which is what turns a swipe into a
    // drag-and-drop on both iOS and Android.
    std::chrono::milliseconds press_delay{0};
};

// One attached or discoverable mobile device.
class DeviceSession {
public:
    virtual ~DeviceSession() = default;

    virtual const DeviceInfo& info() const = 0;
    virtual DeviceTransport active_transport() const = 0;

    // Re-locates the host window and recomputes the viewport. Cheap; the
    // Device tool calls it before every onscreen interaction because the user
    // may have moved the window.
    virtual Status refresh() = 0;

    virtual Status tap(const Point& device_point, const DeviceTapOptions& opts) = 0;
    virtual Status swipe(const Point& from, const Point& to, const DeviceSwipeOptions& opts) = 0;
    virtual Status stroke(const std::vector<PathPoint>& device_path, const StrokeOptions& opts) = 0;
    virtual Status gesture(const GestureRequest& req) = 0;  // center in device points
    virtual Status type_text(std::string_view utf8) = 0;
    virtual Status press_button(std::string_view name) = 0;  // home, back, power, volumeup, ...

    virtual Result<Frame> screenshot() = 0;
    virtual Result<std::string> shell(std::string_view command) = 0;  // adb shell / simctl spawn
    virtual Status install_app(std::string_view path) = 0;
    virtual Status launch_app(std::string_view bundle_or_package) = 0;
    virtual Status terminate_app(std::string_view bundle_or_package) = 0;
    virtual Status open_url(std::string_view url) = 0;

    // Android exposes a real accessibility tree over `uiautomator dump`; iOS
    // simulators expose one over idb. Returns Unsupported without them.
    virtual Result<Tree> ui_tree(const TreeOptions& opts) = 0;

    virtual const DeviceViewport& viewport() const = 0;
};

// Discovers devices across every available transport.
class DeviceManager {
public:
    static Result<std::shared_ptr<DeviceManager>> create(std::shared_ptr<DisplayGraph> displays,
                                                         InputBackend* input, ScreenBackend* screen,
                                                         WindowBackend* windows);
    virtual ~DeviceManager() = default;

    virtual Result<std::vector<DeviceInfo>> list(bool booted_only) = 0;
    virtual Result<std::shared_ptr<DeviceSession>> open(std::string_view id_or_name,
                                                        DeviceTransport transport) = 0;
    virtual Status boot(std::string_view id) = 0;
    virtual Status shutdown(std::string_view id) = 0;

    // Which tools were found on PATH. Surfaced in the Device tool output so a
    // missing `idb` is diagnosable without reading the README.
    virtual std::vector<std::string> available_tooling() const = 0;
};

}  // namespace cc
