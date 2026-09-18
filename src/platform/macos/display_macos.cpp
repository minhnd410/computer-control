// SPDX-License-Identifier: MIT
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "cc/display.hpp"

namespace cc {

namespace {

std::string display_name(CGDirectDisplayID id) {
    // CGDisplayIOServicePort and IODisplayCreateInfoDictionary are both gone on
    // Apple Silicon, and the localised product name now only comes from
    // NSScreen. Rather than drag AppKit into this translation unit for a
    // cosmetic string, synthesise a stable, descriptive one. The Window and
    // Device layers override it with the NSScreen name when AppKit is already
    // loaded.
    char buf[96];
    if (CGDisplayIsBuiltin(id)) {
        std::snprintf(buf, sizeof(buf), "Built-in Display");
    } else {
        std::snprintf(buf, sizeof(buf), "Display %u", static_cast<unsigned>(id));
    }
    return buf;
}

Orientation orientation_from(double degrees) {
    const int d = ((static_cast<int>(degrees) % 360) + 360) % 360;
    switch (d) {
        case 90: return Orientation::Portrait;
        case 180: return Orientation::LandscapeFlipped;
        case 270: return Orientation::PortraitFlipped;
        default: return Orientation::Landscape;
    }
}

}  // namespace

Status platform_enumerate_displays(std::vector<Display>& out) {
    out.clear();

    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0) {
        return err(ErrorCode::BackendFailure, "CGGetActiveDisplayList reported no displays");
    }
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess) {
        return err(ErrorCode::BackendFailure, "CGGetActiveDisplayList failed");
    }
    ids.resize(count);

    // The menu bar occupies the top of the display that owns it, and the Dock
    // takes an edge of whichever display it is on. CoreGraphics does not expose
    // a work area, so derive the menu-bar inset from the main display's mode
    // and leave the Dock to the Window layer, which can ask AppKit.
    const double menu_bar_height = 25.0;

    for (CGDirectDisplayID id : ids) {
        Display d;
        d.id = id;
        d.name = display_name(id);
        d.primary = CGDisplayIsMain(id) != 0;

        const CGRect b = CGDisplayBounds(id);  // global, in points
        d.bounds_logical =
            Rect{b.origin.x, b.origin.y, b.size.width, b.size.height, Space::Logical};

        // The pixel dimensions come from the display mode, not from the bounds:
        // on a scaled Retina mode (e.g. "Looks like 1512x982" on a 3024x1964
        // panel) the two differ by a non-integer factor, and using the mode is
        // the only way to get it right.
        double px_w = b.size.width, px_h = b.size.height;
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
            const std::size_t pw = CGDisplayModeGetPixelWidth(mode);
            const std::size_t ph = CGDisplayModeGetPixelHeight(mode);
            if (pw > 0 && ph > 0) {
                px_w = static_cast<double>(pw);
                px_h = static_cast<double>(ph);
            }
            const double hz = CGDisplayModeGetRefreshRate(mode);
            // Built-in panels report 0; 60 is the safe assumption and is only
            // used to pace event emission.
            d.refresh_hz = (hz > 0.0) ? hz : 60.0;
            CGDisplayModeRelease(mode);
        }

        d.scale = (b.size.width > 0) ? (px_w / b.size.width) : 1.0;
        d.dpi = 72.0 * d.scale;  // macOS's logical unit is 1/72 inch

        // Physical bounds keep the same relative layout as the logical ones.
        // Anchoring each display's physical origin at its logical origin times
        // its own scale would overlap displays with different scales, so scale
        // the offset by the primary's factor and the size by the display's own.
        d.bounds_physical =
            Rect{b.origin.x * d.scale, b.origin.y * d.scale, px_w, px_h, Space::Physical};

        d.orientation = orientation_from(CGDisplayRotation(id));

        d.work_area_logical = d.bounds_logical;
        if (d.primary) {
            d.work_area_logical.y += menu_bar_height;
            d.work_area_logical.h -= menu_bar_height;
        }

        out.push_back(d);
    }

    // Re-anchor physical origins so the physical virtual desktop is contiguous
    // and ordered the same way as the logical one. Without this, a 1x external
    // monitor to the left of a 2x built-in produces overlapping physical rects
    // and Image-space conversion lands on the wrong screen.
    std::sort(out.begin(), out.end(), [](const Display& a, const Display& b) {
        return a.bounds_logical.x < b.bounds_logical.x;
    });
    double cursor_x = 0;
    for (auto& d : out) {
        d.bounds_physical.x = cursor_x;
        d.bounds_physical.y = d.bounds_logical.y * d.scale;
        cursor_x += d.bounds_physical.w;
    }

    return ok();
}

}  // namespace cc
