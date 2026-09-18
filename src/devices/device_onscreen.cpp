// SPDX-License-Identifier: MIT
//
// Devices that exist only as a rectangle on the host screen: iPhone Mirroring,
// QuickTime device mirroring, scrcpy without adb reachable, a simulator whose
// CLI is missing, or a phone on a capture card. No bridge, no tooling, just
// pixels and host input.
//
// The whole value here is that the coordinate mapping is explicit. A caller
// works in device points and this translates, so the same automation script
// runs against a bridged simulator and a mirrored handset unchanged.

#include <algorithm>

#include "devices/device_internal.hpp"

namespace cc::devices {
namespace {

class OnscreenSession final : public DeviceSession {
public:
    OnscreenSession(DeviceInfo info, std::shared_ptr<OnscreenDriver> driver)
        : info_(std::move(info)), driver_(std::move(driver)) {}

    const DeviceInfo& info() const override { return info_; }
    DeviceTransport active_transport() const override { return DeviceTransport::Onscreen; }
    const DeviceViewport& viewport() const override { return viewport_; }

    Status refresh() override {
        if (!driver_) return err(ErrorCode::Unsupported, "no onscreen driver");
        auto vp = driver_->locate(info_);
        if (!vp) return vp.error();
        viewport_ = vp.value();
        return ok();
    }

    Status tap(const Point& p, const DeviceTapOptions& o) override {
        if (auto st = refresh(); !st) return st;
        return driver_->tap(viewport_, p, o);
    }
    Status swipe(const Point& a, const Point& b, const DeviceSwipeOptions& o) override {
        if (auto st = refresh(); !st) return st;
        return driver_->swipe(viewport_, a, b, o);
    }
    Status stroke(const std::vector<PathPoint>& path, const StrokeOptions& o) override {
        if (auto st = refresh(); !st) return st;
        return driver_->stroke(viewport_, path, o);
    }
    Status gesture(const GestureRequest& req) override {
        if (auto st = refresh(); !st) return st;
        return driver_->gesture(viewport_, req);
    }
    Status type_text(std::string_view utf8) override {
        if (auto st = refresh(); !st) return st;
        return driver_->type_text(utf8);
    }
    Result<Frame> screenshot() override {
        if (auto st = refresh(); !st) return st.error();
        return driver_->screenshot(viewport_);
    }

    Status press_button(std::string_view name) override {
        return err(ErrorCode::Unsupported,
                   "hardware buttons cannot be pressed over a mirrored connection",
                   "Tap the on-screen equivalent, or connect the device over adb/idb for "
                   "real button events.");
    }
    Result<std::string> shell(std::string_view) override {
        return err(ErrorCode::Unsupported, "a mirrored device has no shell");
    }
    Status install_app(std::string_view) override {
        return err(ErrorCode::Unsupported, "cannot install over a mirrored connection");
    }
    Status launch_app(std::string_view) override {
        return err(ErrorCode::Unsupported, "cannot launch apps over a mirrored connection",
                   "Tap the app icon: go to the home screen and use tap().");
    }
    Status terminate_app(std::string_view) override {
        return err(ErrorCode::Unsupported, "cannot terminate apps over a mirrored connection");
    }
    Status open_url(std::string_view) override {
        return err(ErrorCode::Unsupported, "cannot open URLs over a mirrored connection");
    }
    Result<Tree> ui_tree(const TreeOptions&) override {
        return err(ErrorCode::Unsupported, "a mirrored device exposes no accessibility tree",
                   "Take a screenshot and work from the image.");
    }

private:
    DeviceInfo info_;
    std::shared_ptr<OnscreenDriver> driver_;
    DeviceViewport viewport_;
};

}  // namespace

Result<std::shared_ptr<DeviceSession>> open_onscreen(const DeviceInfo& info,
                                                     std::shared_ptr<OnscreenDriver> driver) {
    return std::static_pointer_cast<DeviceSession>(
        std::make_shared<OnscreenSession>(info, std::move(driver)));
}

}  // namespace cc::devices
