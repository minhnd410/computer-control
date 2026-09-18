// SPDX-License-Identifier: MIT
#include <memory>

#include "cc/display.hpp"
#include "test_framework.hpp"

using namespace cc;

// These read the machine's actual display topology, so they cannot run
// without a desktop. A headless CI runner or a container build stage has no X
// server; skipping there is correct, and failing would only train people to
// ignore a red suite.
namespace {
std::shared_ptr<DisplayGraph> require_displays() {
    auto g = DisplayGraph::create();
    if (!g) SKIP("no display available: " + g.error().message);
    return g.value();
}
}  // namespace

TEST(display_graph_reports_the_real_desktop) {
    auto g = require_displays();
    CHECK(!g->displays().empty());
    CHECK(g->primary() != nullptr);

    for (const auto& d : g->displays()) {
        CHECK(d.bounds_logical.w > 0);
        CHECK(d.bounds_logical.h > 0);
        CHECK(d.scale > 0);
        // A display whose physical size is not a sane multiple of its logical
        // size means the scale detection is broken, which silently misplaces
        // every click derived from a screenshot.
        CHECK(d.bounds_physical.w >= d.bounds_logical.w - 1);
        CHECK_NEAR(d.bounds_physical.w / d.bounds_logical.w, d.scale, 0.01);
    }
}

TEST(logical_physical_roundtrip_is_lossless) {
    auto graph = require_displays();
    auto& g = graph;
    const Display* d = g->primary();
    CHECK(d != nullptr);
    if (!d) return;

    for (double fx : {0.0, 0.25, 0.5, 0.9}) {
        for (double fy : {0.0, 0.33, 0.75}) {
            const Point logical{d->bounds_logical.x + d->bounds_logical.w * fx,
                                d->bounds_logical.y + d->bounds_logical.h * fy, Space::Logical};
            const Point phys = g->convert(logical, Space::Physical);
            const Point back = g->convert(phys, Space::Logical);
            CHECK_NEAR(back.x, logical.x, 0.01);
            CHECK_NEAR(back.y, logical.y, 0.01);
        }
    }
}

TEST(image_space_requires_a_capture) {
    auto graph = require_displays();
    auto& g = graph;
    // With no transform published, an image-space point converts as a no-op
    // rather than producing a plausible-looking wrong answer.
    CHECK(!g->image_transform().valid);
}

TEST(image_space_maps_back_through_the_transform) {
    auto graph = require_displays();
    auto& g = graph;
    const Display* d = g->primary();
    if (!d) return;

    // Simulate a capture of the primary display downscaled by half.
    DisplayGraph::ImageTransform t;
    t.valid = true;
    t.source_physical = d->bounds_physical;
    t.scale = 0.5;
    t.width = static_cast<int>(d->bounds_physical.w * 0.5);
    t.height = static_cast<int>(d->bounds_physical.h * 0.5);
    g->set_image_transform(t);

    const Point image_centre{t.width / 2.0, t.height / 2.0, Space::Image};
    const Point logical = g->convert(image_centre, Space::Logical);
    CHECK_NEAR(logical.x, d->bounds_logical.x + d->bounds_logical.w / 2, 1.0);
    CHECK_NEAR(logical.y, d->bounds_logical.y + d->bounds_logical.h / 2, 1.0);

    // And back again.
    const Point image_again = g->convert(logical, Space::Image);
    CHECK_NEAR(image_again.x, image_centre.x, 1.0);
    CHECK_NEAR(image_again.y, image_centre.y, 1.0);
}

TEST(containing_falls_back_to_nearest_display) {
    auto graph = require_displays();
    auto& g = graph;
    // Far outside every display: must still resolve, not return null, so a
    // slightly out-of-bounds coordinate converts with a sane scale.
    const Display* d = g->containing(Point{-99999, -99999, Space::Logical});
    CHECK(d != nullptr);
}

TEST(virtual_bounds_covers_every_display) {
    auto graph = require_displays();
    auto& g = graph;
    const Rect vb = g->virtual_bounds(Space::Logical);
    for (const auto& d : g->displays()) {
        CHECK(d.bounds_logical.x >= vb.x - 0.01);
        CHECK(d.bounds_logical.right() <= vb.right() + 0.01);
    }
}
