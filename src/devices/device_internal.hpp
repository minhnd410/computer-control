// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cc/device.hpp"

namespace cc::devices {

// Minimal subprocess helper. Every mobile bridge (simctl, idb, adb, scrcpy) is
// a CLI, so this is the workhorse of the whole device layer.
struct ExecResult {
    int exit_code = -1;
    std::string out;
    std::string err;
    bool timed_out = false;
    bool spawn_failed = false;
};

// Arguments are passed as a vector and never concatenated into a shell string:
// a device name like `iPhone 15 Pro (Rosetta)` or a path with a space would
// otherwise need quoting that is easy to get wrong and, with attacker-supplied
// package names, easy to turn into command injection.
ExecResult exec(const std::string& program, const std::vector<std::string>& args,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{15000},
                const std::string& stdin_data = {});

// Absolute path of a tool on PATH, or empty. Results are cached: `which` on
// every tap would dominate the cost of the tap itself.
std::string which(const std::string& tool);
bool have(const std::string& tool);

// Decodes a PNG or JPEG the device handed back into a Frame. Devices return
// encoded images, and the caller wants pixels.
Result<Frame> decode_image(const std::vector<std::uint8_t>& bytes);

std::string trim(std::string s);
std::vector<std::string> split_lines(const std::string& s);

// Shared implementation of everything a DeviceSession can do by driving the
// host screen, used by the onscreen transport and as the gesture fallback for
// both bridges.
class OnscreenDriver {
public:
    OnscreenDriver(std::shared_ptr<DisplayGraph> displays, InputBackend* input,
                   ScreenBackend* screen, WindowBackend* windows);

    // Locates the host window for this device and derives the viewport by
    // fitting the device's aspect ratio inside the window's content area.
    Result<DeviceViewport> locate(const DeviceInfo& info);

    Status tap(const DeviceViewport& vp, const Point& device_point, const DeviceTapOptions& opts);
    Status swipe(const DeviceViewport& vp, const Point& from, const Point& to,
                 const DeviceSwipeOptions& opts);
    Status stroke(const DeviceViewport& vp, const std::vector<PathPoint>& path,
                  const StrokeOptions& opts);
    Status gesture(const DeviceViewport& vp, const GestureRequest& req);
    Status type_text(std::string_view utf8);
    Result<Frame> screenshot(const DeviceViewport& vp);

    InputBackend* input() const { return input_; }
    WindowBackend* windows() const { return windows_; }

private:
    std::shared_ptr<DisplayGraph> displays_;
    InputBackend* input_;
    ScreenBackend* screen_;
    WindowBackend* windows_;
};

// Chrome the host application draws around the device screen, in host points.
// Values are measured from the window frame reported by the window server.
struct ChromeInsets {
    double top = 0;  // title bar
    double left = 0;
    double right = 0;
    double bottom = 0;
};

ChromeInsets chrome_for(const DeviceInfo& info, const WindowInfo& window);

// Largest rect with `aspect` (w/h) that fits inside `container`, centred.
// The simulators letterbox rather than stretch, so this recovers the real
// device screen rect from the window's content area.
Rect fit_aspect(const Rect& container, double aspect);

// Per-platform discovery and session construction. Declared here so the
// manager and the tests can reach them without each backend exporting a header.
Result<std::vector<DeviceInfo>> list_ios_devices(bool booted_only);
Result<std::shared_ptr<DeviceSession>> open_ios(const DeviceInfo& info, DeviceTransport transport,
                                                std::shared_ptr<OnscreenDriver> driver);
Status boot_ios(std::string_view id);
Status shutdown_ios(std::string_view id);

Result<std::vector<DeviceInfo>> list_android_devices(bool booted_only);
Result<std::shared_ptr<DeviceSession>> open_android(const DeviceInfo& info,
                                                    DeviceTransport transport,
                                                    std::shared_ptr<OnscreenDriver> driver);

Result<std::shared_ptr<DeviceSession>> open_onscreen(const DeviceInfo& info,
                                                     std::shared_ptr<OnscreenDriver> driver);

// Fills in screen_points / device_scale from a device model name. Exposed so
// onscreen discovery can size a simulator it found by window title alone.
void apply_ios_metrics(DeviceInfo& info);

// Devices that are only visible as a window: a simulator on a host with no
// CLI tooling, iPhone Mirroring, scrcpy, QuickTime device mirroring. Without
// this, a perfectly drivable simulator is invisible just because `simctl` is
// missing, which is the common case when Xcode is installed but
// `xcode-select` still points at the Command Line Tools.
Result<std::vector<DeviceInfo>> list_onscreen_devices(WindowBackend* windows);

}  // namespace cc::devices
