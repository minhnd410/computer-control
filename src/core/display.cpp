// SPDX-License-Identifier: MIT
#include "cc/display.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cc {

// Implemented per platform in src/platform/<os>/display_<os>.cpp.
Status platform_enumerate_displays(std::vector<Display>& out);

Result<std::shared_ptr<DisplayGraph>> DisplayGraph::create() {
    auto g = std::shared_ptr<DisplayGraph>(new DisplayGraph());
    if (auto st = g->refresh(); !st) return st.error();
    return g;
}

Status DisplayGraph::refresh() {
    std::vector<Display> fresh;
    if (auto st = platform_enumerate_displays(fresh); !st) return st;
    if (fresh.empty()) {
        return err(ErrorCode::BackendFailure, "no displays reported by the OS",
                   "On a headless host start a virtual display (Xvfb on Linux, a headless "
                   "resolution on Windows) before using computer-control.");
    }
    // Stable ordering: primary first, then left-to-right, top-to-bottom. The
    // index is part of the public API, so it must not shuffle between calls on
    // an unchanged desktop.
    std::sort(fresh.begin(), fresh.end(), [](const Display& a, const Display& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.bounds_logical.x != b.bounds_logical.x)
            return a.bounds_logical.x < b.bounds_logical.x;
        return a.bounds_logical.y < b.bounds_logical.y;
    });
    for (std::size_t i = 0; i < fresh.size(); ++i) fresh[i].index = static_cast<std::int32_t>(i);
    displays_ = std::move(fresh);
    return ok();
}

const Display* DisplayGraph::primary() const noexcept {
    for (const auto& d : displays_)
        if (d.primary) return &d;
    return displays_.empty() ? nullptr : &displays_.front();
}

const Display* DisplayGraph::by_index(std::int32_t index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= displays_.size()) return nullptr;
    return &displays_[static_cast<std::size_t>(index)];
}

const Display* DisplayGraph::by_id(std::uint64_t id) const noexcept {
    for (const auto& d : displays_)
        if (d.id == id) return &d;
    return nullptr;
}

const Display* DisplayGraph::by_name(std::string_view name) const noexcept {
    for (const auto& d : displays_)
        if (d.name == name) return &d;
    return nullptr;
}

const Display* DisplayGraph::containing(const Point& p) const noexcept {
    if (displays_.empty()) return nullptr;
    const bool physical = (p.space == Space::Physical);
    for (const auto& d : displays_) {
        const Rect& r = physical ? d.bounds_physical : d.bounds_logical;
        if (r.contains(p)) return &d;
    }
    // Off every display (negative coordinates on a secondary-left layout,
    // or a stale coordinate after a hot-unplug): fall back to the nearest, so
    // a slightly out-of-bounds click still converts with a sane scale rather
    // than silently using the primary's.
    const Display* best = &displays_.front();
    double best_d = std::numeric_limits<double>::max();
    for (const auto& d : displays_) {
        const Rect& r = physical ? d.bounds_physical : d.bounds_logical;
        const double cx = std::clamp(p.x, r.x, r.right());
        const double cy = std::clamp(p.y, r.y, r.bottom());
        const double dist = (p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy);
        if (dist < best_d) {
            best_d = dist;
            best = &d;
        }
    }
    return best;
}

Rect DisplayGraph::virtual_bounds(Space space) const noexcept {
    if (displays_.empty()) return {};
    double l = std::numeric_limits<double>::max(), t = l;
    double r = std::numeric_limits<double>::lowest(), b = r;
    for (const auto& d : displays_) {
        const Rect& rc = (space == Space::Physical) ? d.bounds_physical : d.bounds_logical;
        l = std::min(l, rc.x);
        t = std::min(t, rc.y);
        r = std::max(r, rc.right());
        b = std::max(b, rc.bottom());
    }
    return Rect{l, t, r - l, b - t, space};
}

Point DisplayGraph::convert(const Point& p, Space to) const {
    if (p.space == to) return p;

    // Normalise through Physical, which is the only space every other one has
    // a well-defined relationship to.
    Point phys = p;
    if (p.space == Space::Logical) {
        const Display* d = containing(p);
        if (d) {
            const double sx =
                (d->bounds_logical.w > 0) ? d->bounds_physical.w / d->bounds_logical.w : d->scale;
            const double sy =
                (d->bounds_logical.h > 0) ? d->bounds_physical.h / d->bounds_logical.h : d->scale;
            phys = Point{d->bounds_physical.x + (p.x - d->bounds_logical.x) * sx,
                         d->bounds_physical.y + (p.y - d->bounds_logical.y) * sy, Space::Physical};
        } else {
            phys = Point{p.x, p.y, Space::Physical};
        }
    } else if (p.space == Space::Image) {
        if (!image_.valid) return p;  // caller checks; conversion is a no-op
        phys = Point{image_.source_physical.x + p.x / image_.scale,
                     image_.source_physical.y + p.y / image_.scale, Space::Physical};
    }

    if (to == Space::Physical) return phys;

    if (to == Space::Logical) {
        const Display* d = containing(phys);
        if (!d) return Point{phys.x, phys.y, Space::Logical};
        const double sx = (d->bounds_physical.w > 0) ? d->bounds_logical.w / d->bounds_physical.w
                                                     : 1.0 / d->scale;
        const double sy = (d->bounds_physical.h > 0) ? d->bounds_logical.h / d->bounds_physical.h
                                                     : 1.0 / d->scale;
        return Point{d->bounds_logical.x + (phys.x - d->bounds_physical.x) * sx,
                     d->bounds_logical.y + (phys.y - d->bounds_physical.y) * sy, Space::Logical};
    }

    // to == Image
    if (!image_.valid) return phys;
    return Point{(phys.x - image_.source_physical.x) * image_.scale,
                 (phys.y - image_.source_physical.y) * image_.scale, Space::Image};
}

Rect DisplayGraph::convert(const Rect& r, Space to) const {
    if (r.space == to) return r;
    const Point tl = convert(Point{r.x, r.y, r.space}, to);
    const Point br = convert(Point{r.right(), r.bottom(), r.space}, to);
    return Rect{tl.x, tl.y, br.x - tl.x, br.y - tl.y, to};
}

void DisplayGraph::set_image_transform(const ImageTransform& t) {
    image_ = t;
}
DisplayGraph::ImageTransform DisplayGraph::image_transform() const {
    return image_;
}

double DisplayGraph::scale_at(const Point& p, Space space) const {
    const Display* d = containing(Point{p.x, p.y, space});
    return d ? d->scale : 1.0;
}

}  // namespace cc
