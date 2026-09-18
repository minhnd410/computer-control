// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cc/types.hpp"

namespace cc {

// Values are the rotation in degrees, so std::uint16_t rather than uint8_t.
enum class Orientation : std::uint16_t {
    Landscape = 0,
    Portrait = 90,
    LandscapeFlipped = 180,
    PortraitFlipped = 270,
};

struct Display {
    std::int32_t index = 0;  // zero-based, stable within one enumeration
    std::uint64_t id = 0;    // CGDirectDisplayID / HMONITOR / RandR output id
    std::string name;        // "Built-in Retina Display", "\\\\.\\DISPLAY1", "eDP-1"

    // Both spaces are reported because neither can be derived from the other
    // without the scale, and fractional scaling (125%, 150%) makes rounding
    // direction matter.
    Rect bounds_logical;     // position within the virtual desktop, points
    Rect bounds_physical;    // position within the virtual desktop, device px
    Rect work_area_logical;  // bounds minus dock / taskbar / menu bar

    double scale = 1.0;  // physical / logical, e.g. 2.0 Retina, 1.5 = 150%
    double dpi = 96.0;   // effective DPI
    double refresh_hz = 0.0;
    Orientation orientation = Orientation::Landscape;
    bool primary = false;
};

// Owns the display topology and every conversion between coordinate spaces.
//
// The graph is snapshotted rather than queried per-call: enumerating displays
// costs a few hundred microseconds to low milliseconds on every platform, and
// a gesture that emits 600 events must not pay that 600 times. Call refresh()
// after a display hot-plug or resolution change; the MCP server refreshes on
// every capture.
class DisplayGraph {
public:
    static Result<std::shared_ptr<DisplayGraph>> create();

    Status refresh();

    const std::vector<Display>& displays() const noexcept { return displays_; }
    const Display* primary() const noexcept;
    const Display* by_index(std::int32_t index) const noexcept;
    const Display* by_id(std::uint64_t id) const noexcept;
    const Display* by_name(std::string_view name) const noexcept;
    // Display whose logical bounds contain the point, else the nearest one.
    const Display* containing(const Point& p) const noexcept;

    // Union of every display, in each space.
    Rect virtual_bounds(Space space) const noexcept;

    // --- conversions -------------------------------------------------------
    // Logical <-> Physical are display-local: a point is resolved against the
    // display that contains it, so mixed-DPI setups convert correctly instead
    // of applying the primary display's scale everywhere.
    Point convert(const Point& p, Space to) const;
    Rect convert(const Rect& r, Space to) const;

    // Image space is defined by whoever produced the last capture. The capture
    // pipeline calls set_image_transform(); conversions to/from Space::Image
    // fail with Unsupported until it has.
    struct ImageTransform {
        bool valid = false;
        Rect source_physical;  // region of the physical virtual desktop captured
        double scale = 1.0;    // image_px / physical_px (<= 1.0 when downscaled)
        std::int32_t width = 0;
        std::int32_t height = 0;
    };
    void set_image_transform(const ImageTransform& t);
    ImageTransform image_transform() const;

private:
    DisplayGraph() = default;
    double scale_at(const Point& p, Space space) const;

    std::vector<Display> displays_;
    ImageTransform image_{};
};

}  // namespace cc
