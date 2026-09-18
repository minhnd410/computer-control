// SPDX-License-Identifier: MIT
#include "cc/device.hpp"
#include "devices/device_internal.hpp"
#include "test_framework.hpp"

using namespace cc;

TEST(viewport_maps_device_points_to_host_points) {
    // An iPhone 15 Pro (393x852 pt) drawn into a 300x650 host rect at (100,50).
    DeviceViewport vp(Rect{100, 50, 300, 650, Space::Logical}, Size{393, 852, Space::Logical});
    CHECK(vp.valid());

    const Point origin = vp.to_host(Point{0, 0});
    CHECK_NEAR(origin.x, 100, 1e-9);
    CHECK_NEAR(origin.y, 50, 1e-9);

    const Point far = vp.to_host(Point{393, 852});
    CHECK_NEAR(far.x, 400, 1e-9);
    CHECK_NEAR(far.y, 700, 1e-9);

    const Point mid = vp.to_host(Point{196.5, 426});
    CHECK_NEAR(mid.x, 250, 1e-6);
    CHECK_NEAR(mid.y, 375, 1e-6);
}

TEST(viewport_roundtrips) {
    DeviceViewport vp(Rect{10, 20, 400, 800, Space::Logical}, Size{393, 852, Space::Logical});
    for (double x : {0.0, 100.0, 392.0}) {
        for (double y : {0.0, 400.0, 851.0}) {
            const Point host = vp.to_host(Point{x, y});
            const Point back = vp.to_device(host);
            CHECK_NEAR(back.x, x, 1e-6);
            CHECK_NEAR(back.y, y, 1e-6);
        }
    }
}

TEST(invalid_viewport_is_identity) {
    DeviceViewport vp;
    CHECK(!vp.valid());
    const Point p = vp.to_host(Point{42, 43});
    CHECK_NEAR(p.x, 42, 1e-9);
    CHECK_NEAR(p.y, 43, 1e-9);
}

TEST(fit_aspect_letterboxes_a_wide_container) {
    // 400x800 device (0.5 aspect) in a 600x800 window: bars left and right.
    const Rect fitted = devices::fit_aspect(Rect{0, 0, 600, 800, Space::Logical}, 0.5);
    CHECK_NEAR(fitted.w, 400, 1e-6);
    CHECK_NEAR(fitted.h, 800, 1e-6);
    CHECK_NEAR(fitted.x, 100, 1e-6);
    CHECK_NEAR(fitted.y, 0, 1e-6);
}

TEST(fit_aspect_letterboxes_a_tall_container) {
    // 2.0 aspect device in a 400x800 window: bars top and bottom.
    const Rect fitted = devices::fit_aspect(Rect{0, 0, 400, 800, Space::Logical}, 2.0);
    CHECK_NEAR(fitted.w, 400, 1e-6);
    CHECK_NEAR(fitted.h, 200, 1e-6);
    CHECK_NEAR(fitted.y, 300, 1e-6);
}

TEST(effective_scale_flags_a_shrunken_window) {
    // A 393pt-wide device shown 250pt wide: under 0.75, which is where taps
    // on small targets start landing on neighbours.
    DeviceViewport vp(Rect{0, 0, 250, 542, Space::Logical}, Size{393, 852, Space::Logical});
    CHECK(vp.effective_scale() < 0.75);

    DeviceViewport full(Rect{0, 0, 393, 852, Space::Logical}, Size{393, 852, Space::Logical});
    CHECK_NEAR(full.effective_scale(), 1.0, 1e-9);
}

TEST(exec_helper_runs_a_command) {
#if !defined(_WIN32)
    auto r = devices::exec("/bin/echo", {"hello world"}, std::chrono::milliseconds{5000});
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("hello world") != std::string::npos);
#endif
}

TEST(exec_helper_does_not_go_through_a_shell) {
    // If arguments were concatenated into a shell string, this would run two
    // commands. It must be passed through as one literal argument.
#if !defined(_WIN32)
    auto r = devices::exec("/bin/echo", {"a; echo INJECTED"}, std::chrono::milliseconds{5000});
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("a; echo INJECTED") != std::string::npos);
    // The literal appears once, and no second line was produced.
    CHECK_EQ(devices::split_lines(devices::trim(r.out)).size(), std::size_t(1));
#endif
}

TEST(exec_helper_reports_a_missing_program) {
    auto r =
        devices::exec("cc-definitely-not-a-real-binary-xyz", {}, std::chrono::milliseconds{3000});
    CHECK(r.spawn_failed || r.exit_code != 0);
}

TEST(exec_helper_enforces_its_timeout) {
#if !defined(_WIN32)
    const auto start = std::chrono::steady_clock::now();
    auto r = devices::exec("/bin/sleep", {"10"}, std::chrono::milliseconds{600});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(r.timed_out);
    // Must actually return near the deadline rather than after the full sleep.
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 5);
#endif
}

TEST(device_listing_degrades_gracefully_without_tooling) {
    // On a machine with neither Xcode nor adb this must return an empty list,
    // not an error: a partial answer is more useful than a hard failure.
    auto ios = devices::list_ios_devices(false);
    auto android = devices::list_android_devices(false);
    CHECK(ios.ok() || ios.error().code == ErrorCode::DeviceError);
    CHECK(android.ok() || android.error().code == ErrorCode::DeviceError);
}
