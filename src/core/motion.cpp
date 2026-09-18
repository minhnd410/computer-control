// SPDX-License-Identifier: MIT
#include "core/motion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace cc::motion {
namespace {
constexpr double kPi = 3.14159265358979323846;

double dist(const Point& a, const Point& b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::uint64_t entropy_seed() {
    static std::random_device rd;
    return (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
           static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
}  // namespace

Rng::Rng(std::uint64_t seed) : state_(seed ? seed : entropy_seed()) {}

double Rng::next_unit() {
    // splitmix64: tiny, fast, good enough for jitter, and reproducible.
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return static_cast<double>(z >> 11) / static_cast<double>(1ULL << 53);
}

double Rng::next_symmetric() {
    return next_unit() * 2.0 - 1.0;
}

double ease(MotionProfile profile, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (profile) {
        case MotionProfile::Instant: return 1.0;
        case MotionProfile::Linear: return t;
        case MotionProfile::EaseInOut:
        case MotionProfile::Human:
            // Cosine ease: zero velocity at both ends, which is what makes a
            // drag look deliberate to hover/drag state machines.
            return 0.5 - 0.5 * std::cos(kPi * t);
    }
    return t;
}

int step_count(const MotionOptions& opts, double distance_px) {
    if (opts.profile == MotionProfile::Instant) return 1;
    const double seconds = std::max(0.001, opts.duration.count() / 1000.0);
    int by_rate = static_cast<int>(std::lround(seconds * std::max(1, opts.rate_hz)));
    // Never emit fewer samples than there are pixels/4 for short hops; a 3px
    // drag with 20 samples is wasteful, and a 2000px drag with 8 is jerky.
    const int by_distance = static_cast<int>(std::lround(distance_px / 4.0));
    int n = std::max(2, std::min(std::max(by_rate, std::min(by_distance, 240)), opts.max_steps));
    return n;
}

std::vector<Sample> line(const Point& from, const Point& to, const MotionOptions& opts) {
    std::vector<Sample> out;
    if (opts.profile == MotionProfile::Instant) {
        out.push_back(Sample{to, 1.0, std::chrono::microseconds{0}});
        return out;
    }

    const double d = dist(from, to);
    const int n = step_count(opts, d);
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(opts.duration);
    const auto per_step = std::chrono::microseconds{total_us.count() / std::max(1, n)};

    Rng rng(opts.seed);
    // Perpendicular unit vector, for the Human profile's lateral drift.
    double px = 0, py = 0;
    if (d > 0.0001) {
        px = -(to.y - from.y) / d;
        py = (to.x - from.x) / d;
    }

    out.reserve(static_cast<std::size_t>(n) + 4);
    for (int i = 1; i <= n; ++i) {
        const double t = static_cast<double>(i) / n;
        const double e = ease(opts.profile, t);
        double x = from.x + (to.x - from.x) * e;
        double y = from.y + (to.y - from.y) * e;

        if (opts.profile == MotionProfile::Human) {
            // Drift peaks mid-travel and vanishes at both ends so the path
            // still starts and finishes exactly on target.
            const double envelope = std::sin(kPi * t);
            const double j = opts.jitter_px * envelope * rng.next_symmetric();
            x += px * j;
            y += py * j;
        }
        out.push_back(Sample{Point{x, y, from.space}, 1.0, per_step});
    }

    if (opts.profile == MotionProfile::Human && opts.overshoot_px > 0.5 && d > 40.0) {
        // Overshoot past the target then settle back, the way a hand does.
        // Inserted before the final sample so the path still ends on target.
        const double ux = (to.x - from.x) / d, uy = (to.y - from.y) / d;
        const double over = opts.overshoot_px * (0.5 + 0.5 * rng.next_unit());
        Sample s{Point{to.x + ux * over, to.y + uy * over, from.space}, 1.0, per_step};
        out.insert(out.end() - 1, s);
    }

    // Guarantee the last sample is exactly the requested target: rounding in
    // the ease must never leave the pointer one pixel short of a small button.
    out.back().at = to;
    return out;
}

std::vector<PathPoint> smooth_catmull_rom(const std::vector<PathPoint>& pts, double tension,
                                          int per_segment) {
    if (pts.size() < 3 || per_segment < 2) return pts;
    std::vector<PathPoint> out;
    out.reserve(pts.size() * static_cast<std::size_t>(per_segment));
    const double a = std::clamp(tension, 0.0, 1.0);

    auto get = [&](long i) -> const PathPoint& {
        const long n = static_cast<long>(pts.size());
        return pts[static_cast<std::size_t>(std::clamp(i, 0L, n - 1))];
    };

    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const PathPoint& p0 = get(static_cast<long>(i) - 1);
        const PathPoint& p1 = pts[i];
        const PathPoint& p2 = pts[i + 1];
        const PathPoint& p3 = get(static_cast<long>(i) + 2);
        for (int s = 0; s < per_segment; ++s) {
            const double t = static_cast<double>(s) / per_segment;
            const double t2 = t * t, t3 = t2 * t;
            auto interp = [&](double v0, double v1, double v2, double v3) {
                return 0.5 * ((2 * v1) + (-v0 + v2) * t * (2 * a) +
                              (2 * v0 - 5 * v1 + 4 * v2 - v3) * t2 * (2 * a) +
                              (-v0 + 3 * v1 - 3 * v2 + v3) * t3 * (2 * a)) +
                       (1 - a) * (v1 + (v2 - v1) * t);
            };
            PathPoint q;
            q.at = Point{interp(p0.at.x, p1.at.x, p2.at.x, p3.at.x),
                         interp(p0.at.y, p1.at.y, p2.at.y, p3.at.y), p1.at.space};
            q.pressure = p1.pressure + (p2.pressure - p1.pressure) * t;
            q.tilt_x = p1.tilt_x;
            q.tilt_y = p1.tilt_y;
            out.push_back(q);
        }
    }
    out.push_back(pts.back());
    return out;
}

std::vector<Sample> polyline(const std::vector<PathPoint>& points, const MotionOptions& opts) {
    std::vector<Sample> out;
    if (points.empty()) return out;
    if (points.size() == 1) {
        out.push_back(
            Sample{points[0].at, points[0].pressure,
                   std::chrono::duration_cast<std::chrono::microseconds>(points[0].dwell)});
        return out;
    }

    // Arc-length parameterisation so a path with unevenly spaced points still
    // moves at a constant speed. Without this, a polyline with one long and
    // one short segment sprints through the long one.
    std::vector<double> cum(points.size(), 0.0);
    for (std::size_t i = 1; i < points.size(); ++i)
        cum[i] = cum[i - 1] + dist(points[i - 1].at, points[i].at);
    const double total = cum.back();

    const int n = step_count(opts, total);
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(opts.duration);
    const auto per_step = std::chrono::microseconds{total_us.count() / std::max(1, n)};

    out.reserve(static_cast<std::size_t>(n) + points.size());
    std::size_t seg = 0;
    for (int i = 1; i <= n; ++i) {
        const double t = static_cast<double>(i) / n;
        const double target = ease(opts.profile, t) * total;
        while (seg + 2 < points.size() && cum[seg + 1] < target) ++seg;
        const double span = cum[seg + 1] - cum[seg];
        const double local = (span > 1e-9) ? (target - cum[seg]) / span : 0.0;
        const PathPoint& a = points[seg];
        const PathPoint& b = points[seg + 1];
        Sample s;
        s.at = Point{a.at.x + (b.at.x - a.at.x) * local, a.at.y + (b.at.y - a.at.y) * local,
                     a.at.space};
        s.pressure = a.pressure + (b.pressure - a.pressure) * local;
        s.delay = per_step;
        out.push_back(s);
    }
    out.back().at = points.back().at;
    out.back().pressure = points.back().pressure;

    // Re-attach explicit dwells by extending the delay of the nearest sample.
    for (const auto& p : points) {
        if (p.dwell.count() <= 0) continue;
        auto best = out.begin();
        double best_d = 1e18;
        for (auto it = out.begin(); it != out.end(); ++it) {
            const double d = dist(it->at, p.at);
            if (d < best_d) {
                best_d = d;
                best = it;
            }
        }
        best->delay += std::chrono::duration_cast<std::chrono::microseconds>(p.dwell);
    }
    return out;
}

Point offset_for(SwipeDirection dir, double distance) {
    switch (dir) {
        case SwipeDirection::Up: return Point{0, -distance};
        case SwipeDirection::Down: return Point{0, distance};
        case SwipeDirection::Left: return Point{-distance, 0};
        case SwipeDirection::Right: return Point{distance, 0};
    }
    return Point{0, 0};
}

std::vector<Point> finger_layout(GestureKind kind, const Point& center, int fingers, double spread,
                                 SwipeDirection dir) {
    fingers = std::clamp(fingers, 1, 5);
    std::vector<Point> out;
    out.reserve(static_cast<std::size_t>(fingers));

    if (kind == GestureKind::Pinch || kind == GestureKind::Rotate ||
        kind == GestureKind::SmartZoom) {
        // Two contacts on a 45-degree axis: a horizontal pair gets confused
        // with a two-finger horizontal swipe by some gesture recognisers.
        const double h = spread / 2.0;
        const double c = 0.7071067811865476;  // cos/sin 45deg
        out.push_back(Point{center.x - h * c, center.y - h * c, center.space});
        out.push_back(Point{center.x + h * c, center.y + h * c, center.space});
        return out;
    }

    // Fingers line up perpendicular to the direction of travel.
    const bool horizontal_travel = (dir == SwipeDirection::Left || dir == SwipeDirection::Right);
    const double gap = std::max(24.0, spread / std::max(1, fingers));
    const double span = gap * (fingers - 1);
    for (int i = 0; i < fingers; ++i) {
        const double o = -span / 2.0 + gap * i;
        if (horizontal_travel)
            out.push_back(Point{center.x, center.y + o, center.space});
        else
            out.push_back(Point{center.x + o, center.y, center.space});
    }
    return out;
}

int gesture_steps(const GestureRequest& req, int rate_hz) {
    const double seconds = std::max(0.016, req.duration.count() / 1000.0);
    return std::clamp(static_cast<int>(std::lround(seconds * rate_hz)), 4, 600);
}

std::vector<Point> gesture_frame(const GestureRequest& req, const std::vector<Point>& start,
                                 double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double e = ease(MotionProfile::EaseInOut, t);
    std::vector<Point> out = start;

    switch (req.kind) {
        case GestureKind::Tap:
        case GestureKind::LongPress:
        case GestureKind::ForcePress: return out;  // contacts do not move

        case GestureKind::Swipe:
        case GestureKind::EdgeSwipe: {
            const Point d = offset_for(req.direction, req.distance * e);
            for (auto& p : out) {
                p.x += d.x;
                p.y += d.y;
            }
            return out;
        }

        case GestureKind::Pan: {
            if (req.path.size() < 2) {
                const Point d = offset_for(req.direction, req.distance * e);
                for (auto& p : out) {
                    p.x += d.x;
                    p.y += d.y;
                }
                return out;
            }
            // Walk the supplied path; every finger keeps its offset from the
            // first contact so the hand shape is preserved.
            const double fpos = e * static_cast<double>(req.path.size() - 1);
            const std::size_t i = std::min(static_cast<std::size_t>(fpos), req.path.size() - 2);
            const double local = fpos - static_cast<double>(i);
            const Point a = req.path[i], b = req.path[i + 1];
            const Point cur{a.x + (b.x - a.x) * local, a.y + (b.y - a.y) * local, a.space};
            const Point anchor = req.path.front();
            for (auto& p : out) {
                p.x += cur.x - anchor.x;
                p.y += cur.y - anchor.y;
            }
            return out;
        }

        case GestureKind::Pinch: {
            // Interpolate the separation multiplicatively: a pinch from 1.0 to
            // 4.0 should feel like a constant-rate zoom, not a constant-rate
            // distance change.
            const double s = std::pow(std::max(0.01, req.scale), e);
            for (auto& p : out) {
                p.x = req.center.x + (p.x - req.center.x) * s;
                p.y = req.center.y + (p.y - req.center.y) * s;
            }
            return out;
        }

        case GestureKind::Rotate: {
            const double rad = req.rotation_degrees * kPi / 180.0 * e;
            const double cs = std::cos(rad), sn = std::sin(rad);
            for (auto& p : out) {
                const double dx = p.x - req.center.x, dy = p.y - req.center.y;
                p.x = req.center.x + dx * cs - dy * sn;
                p.y = req.center.y + dx * sn + dy * cs;
            }
            return out;
        }

        case GestureKind::SmartZoom: return out;
    }
    return out;
}

}  // namespace cc::motion
