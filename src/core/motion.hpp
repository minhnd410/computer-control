// SPDX-License-Identifier: MIT
#pragma once

// Path generation shared by every platform backend.
//
// Keeping this out of the backends means a "human" drag traces the same curve
// on macOS, Windows and Linux, and that gesture geometry (where five fingers
// sit, how a pinch interpolates) is written once and tested without a desktop.

#include <chrono>
#include <cstdint>
#include <vector>

#include "cc/input.hpp"
#include "cc/types.hpp"

namespace cc::motion {

// One emitted sample: where to be, and how long to sleep before the next one.
struct Sample {
    Point at;
    double pressure = 1.0;
    std::chrono::microseconds delay{0};  // sleep after emitting this sample
};

// Number of intermediate steps for a move, clamped by rate and max_steps.
int step_count(const MotionOptions& opts, double distance_px);

double ease(MotionProfile profile, double t);

// Straight-line path from -> to, honouring the profile (including the Human
// profile's jitter and overshoot).
std::vector<Sample> line(const Point& from, const Point& to, const MotionOptions& opts);

// Resamples a polyline into `total_steps` evenly-timed samples, preserving
// per-point dwell. Used by stroke() and by Pan gestures.
std::vector<Sample> polyline(const std::vector<PathPoint>& points, const MotionOptions& opts);

// Catmull-Rom smoothing. Returns a denser polyline passing through every input
// point; `per_segment` controls density.
std::vector<PathPoint> smooth_catmull_rom(const std::vector<PathPoint>& pts, double tension,
                                          int per_segment);

// --- gesture geometry ------------------------------------------------------

// Where `n` fingers start for a gesture centred at `center`.
//   Pinch/Rotate: 2 contacts on opposite sides of the centre, `spread` apart.
//   Swipe/Pan/Tap: n contacts in a row, perpendicular to the travel direction,
//                  which is how a real hand lands on a trackpad or screen.
std::vector<Point> finger_layout(GestureKind kind, const Point& center, int fingers, double spread,
                                 SwipeDirection dir);

// Positions of every contact at normalised time t (0..1) for the request.
// The returned vector always has req.fingers entries, in the same order as
// finger_layout().
std::vector<Point> gesture_frame(const GestureRequest& req, const std::vector<Point>& start,
                                 double t);

// Frame count for a gesture, from its duration and a target rate.
int gesture_steps(const GestureRequest& req, int rate_hz = 120);

Point offset_for(SwipeDirection dir, double distance);

// Deterministic when seeded, so a "human" path can be replayed in a test.
class Rng {
public:
    explicit Rng(std::uint64_t seed);
    double next_unit();       // [0,1)
    double next_symmetric();  // [-1,1)
private:
    std::uint64_t state_;
};

}  // namespace cc::motion
