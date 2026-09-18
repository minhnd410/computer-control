// SPDX-License-Identifier: MIT
#include <cmath>
#include <set>

#include "core/motion.hpp"
#include "test_framework.hpp"

using namespace cc;
using namespace cc::motion;

namespace {
double dist(const Point& a, const Point& b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}
}  // namespace

TEST(line_always_ends_exactly_on_target) {
    // Rounding in the easing must never leave the pointer a pixel short of a
    // small button; this is the invariant that guarantees it.
    for (auto profile : {MotionProfile::Linear, MotionProfile::EaseInOut, MotionProfile::Human}) {
        MotionOptions o;
        o.profile = profile;
        o.seed = 12345;
        auto samples = line(Point{13.5, 7.25}, Point{981.75, 444.5}, o);
        CHECK(!samples.empty());
        CHECK_NEAR(samples.back().at.x, 981.75, 1e-9);
        CHECK_NEAR(samples.back().at.y, 444.5, 1e-9);
    }
}

TEST(instant_profile_emits_one_sample) {
    MotionOptions o;
    o.profile = MotionProfile::Instant;
    auto samples = line(Point{0, 0}, Point{500, 500}, o);
    CHECK_EQ(samples.size(), std::size_t(1));
    CHECK_NEAR(samples[0].at.x, 500, 1e-9);
}

TEST(ease_is_monotonic_and_bounded) {
    double prev = -1;
    for (int i = 0; i <= 100; ++i) {
        const double t = i / 100.0;
        const double e = ease(MotionProfile::EaseInOut, t);
        CHECK(e >= -1e-9 && e <= 1.0 + 1e-9);
        CHECK(e >= prev - 1e-9);
        prev = e;
    }
    CHECK_NEAR(ease(MotionProfile::EaseInOut, 0.0), 0.0, 1e-9);
    CHECK_NEAR(ease(MotionProfile::EaseInOut, 1.0), 1.0, 1e-9);
    CHECK_NEAR(ease(MotionProfile::EaseInOut, 0.5), 0.5, 1e-9);
}

TEST(human_profile_is_reproducible_when_seeded) {
    MotionOptions a;
    a.profile = MotionProfile::Human;
    a.seed = 99;
    MotionOptions b = a;

    auto s1 = line(Point{0, 0}, Point{300, 200}, a);
    auto s2 = line(Point{0, 0}, Point{300, 200}, b);
    CHECK_EQ(s1.size(), s2.size());
    for (std::size_t i = 0; i < s1.size() && i < s2.size(); ++i) {
        CHECK_NEAR(s1[i].at.x, s2[i].at.x, 1e-12);
        CHECK_NEAR(s1[i].at.y, s2[i].at.y, 1e-12);
    }
}

TEST(human_profile_deviates_from_straight_line) {
    MotionOptions o;
    o.profile = MotionProfile::Human;
    o.seed = 7;
    o.jitter_px = 4.0;
    auto samples = line(Point{0, 0}, Point{400, 0}, o);

    double max_dev = 0;
    for (const auto& s : samples) max_dev = std::max(max_dev, std::fabs(s.at.y));
    // Some lateral drift, but never wild: a path that wanders 50px off a
    // 400px horizontal move would miss its target's row entirely.
    CHECK(max_dev > 0.1);
    CHECK(max_dev < 30.0);
}

TEST(step_count_respects_budget) {
    MotionOptions o;
    o.rate_hz = 120;
    o.duration = std::chrono::milliseconds{1000};
    o.max_steps = 50;
    CHECK(step_count(o, 5000) <= 50);
    CHECK(step_count(o, 5000) >= 2);
}

TEST(polyline_passes_through_endpoints) {
    std::vector<PathPoint> pts{PathPoint{Point{0, 0}}, PathPoint{Point{100, 50}},
                               PathPoint{Point{200, 0}}};
    MotionOptions o;
    auto samples = polyline(pts, o);
    CHECK(!samples.empty());
    CHECK_NEAR(samples.back().at.x, 200, 1e-6);
    CHECK_NEAR(samples.back().at.y, 0, 1e-6);
}

TEST(polyline_is_arc_length_parameterised) {
    // One very long segment then a very short one: samples must not bunch up
    // in the short segment.
    std::vector<PathPoint> pts{PathPoint{Point{0, 0}}, PathPoint{Point{1000, 0}},
                               PathPoint{Point{1010, 0}}};
    MotionOptions o;
    o.profile = MotionProfile::Linear;
    o.duration = std::chrono::milliseconds{500};
    auto samples = polyline(pts, o);

    int in_long = 0, in_short = 0;
    for (const auto& s : samples) (s.at.x <= 1000 ? in_long : in_short)++;
    CHECK(in_long > in_short * 5);
}

TEST(pinch_geometry_scales_about_the_centre) {
    GestureRequest g;
    g.kind = GestureKind::Pinch;
    g.center = Point{500, 500};
    g.spread = 100;
    g.scale = 3.0;

    auto start = finger_layout(g.kind, g.center, 2, g.spread, SwipeDirection::Left);
    CHECK_EQ(start.size(), std::size_t(2));
    const double d0 = dist(start[0], start[1]);
    CHECK_NEAR(d0, 100.0, 1e-6);

    auto mid = gesture_frame(g, start, 0.5);
    auto end = gesture_frame(g, start, 1.0);
    const double d_end = dist(end[0], end[1]);
    CHECK_NEAR(d_end, 300.0, 1e-6);
    // Multiplicative interpolation: halfway is sqrt(3)x, not 2x.
    CHECK_NEAR(dist(mid[0], mid[1]), 100.0 * std::sqrt(3.0), 1e-6);

    // The midpoint must not drift.
    CHECK_NEAR((end[0].x + end[1].x) / 2, 500, 1e-6);
    CHECK_NEAR((end[0].y + end[1].y) / 2, 500, 1e-6);
}

TEST(pinch_out_shrinks_separation) {
    GestureRequest g;
    g.kind = GestureKind::Pinch;
    g.center = Point{0, 0};
    g.spread = 200;
    g.scale = 0.5;
    auto start = finger_layout(g.kind, g.center, 2, g.spread, SwipeDirection::Left);
    auto end = gesture_frame(g, start, 1.0);
    CHECK_NEAR(dist(end[0], end[1]), 100.0, 1e-6);
}

TEST(rotate_preserves_separation) {
    GestureRequest g;
    g.kind = GestureKind::Rotate;
    g.center = Point{200, 200};
    g.spread = 150;
    g.rotation_degrees = 90;

    auto start = finger_layout(g.kind, g.center, 2, g.spread, SwipeDirection::Left);
    auto end = gesture_frame(g, start, 1.0);
    CHECK_NEAR(dist(end[0], end[1]), dist(start[0], start[1]), 1e-6);
    // A 90 degree rotation must actually move the contacts.
    CHECK(dist(start[0], end[0]) > 50);
}

TEST(swipe_moves_every_finger_together) {
    GestureRequest g;
    g.kind = GestureKind::Swipe;
    g.center = Point{400, 400};
    g.fingers = 3;
    g.direction = SwipeDirection::Left;
    g.distance = 300;

    auto start = finger_layout(g.kind, g.center, 3, 120, g.direction);
    CHECK_EQ(start.size(), std::size_t(3));
    // Fingers line up perpendicular to travel, so a horizontal swipe spreads
    // them vertically.
    CHECK_NEAR(start[0].x, start[1].x, 1e-9);
    CHECK(std::fabs(start[0].y - start[1].y) > 1);

    auto end = gesture_frame(g, start, 1.0);
    for (std::size_t i = 0; i < start.size(); ++i) {
        CHECK_NEAR(end[i].x, start[i].x - 300, 1e-6);
        CHECK_NEAR(end[i].y, start[i].y, 1e-6);
    }
}

TEST(tap_contacts_do_not_move) {
    GestureRequest g;
    g.kind = GestureKind::Tap;
    g.center = Point{10, 10};
    auto start = finger_layout(g.kind, g.center, 2, 60, SwipeDirection::Up);
    auto end = gesture_frame(g, start, 1.0);
    for (std::size_t i = 0; i < start.size(); ++i) {
        CHECK_NEAR(end[i].x, start[i].x, 1e-12);
        CHECK_NEAR(end[i].y, start[i].y, 1e-12);
    }
}

TEST(catmull_rom_passes_through_input_points) {
    std::vector<PathPoint> pts{PathPoint{Point{0, 0}}, PathPoint{Point{50, 100}},
                               PathPoint{Point{100, 0}}, PathPoint{Point{150, 100}}};
    auto smooth = smooth_catmull_rom(pts, 0.5, 6);
    CHECK(smooth.size() > pts.size());
    CHECK_NEAR(smooth.front().at.x, 0, 1e-6);
    CHECK_NEAR(smooth.back().at.x, 150, 1e-6);
}
