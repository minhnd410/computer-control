// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "cc/display.hpp"
#include "cc/keys.hpp"
#include "cc/types.hpp"

namespace cc {

enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2, Back = 3, Forward = 4 };

const char* to_string(MouseButton b) noexcept;
Result<MouseButton> button_from_string(std::string_view s);

enum class ScrollAxis : std::uint8_t { Vertical = 0, Horizontal = 1 };
enum class ScrollDirection : std::uint8_t { Up = 0, Down = 1, Left = 2, Right = 3 };

// How a pointer travels between two points. Straight-line teleports are the
// fastest but some UIs (drag handles, hover menus, canvas apps, and most
// drag-and-drop implementations) only react to intermediate motion, and some
// anti-automation heuristics reject perfectly linear motion.
enum class MotionProfile : std::uint8_t {
    Instant = 0,  // single warp, no intermediate events
    Linear = 1,
    EaseInOut = 2,  // cosine ease, the default for anything user-visible
    Human = 3,      // ease + small perpendicular noise + overshoot-and-settle
};

struct MotionOptions {
    MotionProfile profile = MotionProfile::EaseInOut;
    std::chrono::milliseconds duration{180};
    // Event emission rate. 120 Hz matches ProMotion / high-refresh panels;
    // the emitter clamps so that duration * rate stays under max_steps.
    int rate_hz = 120;
    int max_steps = 600;
    // Human profile only: jitter amplitude in logical px, and how far past the
    // target to overshoot before settling back.
    double jitter_px = 1.2;
    double overshoot_px = 6.0;
    std::uint64_t seed = 0;  // 0 = nondeterministic; set for reproducible paths
};

struct ClickOptions {
    MouseButton button = MouseButton::Left;
    // 0 = hover only (move, no press), 1 = single, 2 = double, 3 = triple.
    // Multi-clicks are emitted as one stream with the OS click-count field set
    // so apps see a real double/triple click rather than N unrelated clicks.
    int count = 1;
    Modifier modifiers = Modifier::None;
    // Delay between press and release. A few ms is enough for most apps; some
    // custom controls need 40ms+ to register.
    std::chrono::milliseconds press_duration{12};
    // Gap between clicks in a multi-click. Must stay under the OS double-click
    // interval; the default is safely below every platform default.
    std::chrono::milliseconds inter_click{60};
    bool move_first = true;
};

struct ScrollOptions {
    ScrollAxis axis = ScrollAxis::Vertical;
    ScrollDirection direction = ScrollDirection::Down;
    int clicks = 3;  // wheel detents
    Modifier modifiers = Modifier::None;
    // Pixel-precise ("smooth") scrolling instead of line detents. Trackpad-like
    // and required by some canvas apps that ignore line-unit wheel events.
    bool pixel_units = false;
    int pixels_per_click = 40;
    // Emit begin/changed/end phase markers so momentum-aware apps treat the
    // scroll as one trackpad gesture.
    bool phased = false;
};

// One waypoint of a freehand path. Pressure and tilt are carried through to
// backends that support them (Windows pen injection, uinput pen device) and
// ignored elsewhere.
struct PathPoint {
    Point at;
    double pressure = 1.0;               // 0..1
    double tilt_x = 0.0, tilt_y = 0.0;   // degrees, -90..90
    std::chrono::milliseconds dwell{0};  // pause here before continuing
};

struct StrokeOptions {
    MouseButton button = MouseButton::Left;
    Modifier modifiers = Modifier::None;
    MotionOptions motion{};
    // Resample the polyline into a smooth Catmull-Rom spline before emitting.
    // Drawing apps produce visibly nicer output; UI automation should leave it
    // off so the pointer passes exactly through the given points.
    bool smooth = false;
    double smooth_tension = 0.5;
    std::chrono::milliseconds settle_before_release{40};
    bool use_pen = false;  // route through the pen/stylus device when available
};

// ---------------------------------------------------------------------------
// Multi-touch gestures
// ---------------------------------------------------------------------------
//
// Backend support is genuinely uneven and the API says so rather than
// pretending otherwise:
//
//   Windows  - real multi-touch via InjectTouchInput (Win8+). Full fidelity:
//              the compositor and the target app see actual contacts.
//   Linux    - real multi-touch via a virtual /dev/uinput ABS_MT touchscreen.
//              Needs write access to /dev/uinput (see README).
//   macOS    - there is no public multi-touch synthesis API. Two paths:
//              (a) the private NSEvent/CGEvent gesture constructors, resolved
//                  by dlsym at runtime and used only if present;
//              (b) semantic fallback: pinch -> cmd+scroll, two-finger swipe ->
//                  phased scroll, three/four-finger swipe -> the Mission
//                  Control key equivalents.
//              `GestureSupport` reports which one you will get.

enum class GestureKind : std::uint8_t {
    Tap = 0,     // n-finger tap
    Swipe,       // n-finger directional swipe
    Pan,         // n-finger free drag along a path
    Pinch,       // 2-finger pinch in/out (zoom)
    Rotate,      // 2-finger rotation
    SmartZoom,   // 2-finger double tap (macOS smart zoom)
    ForcePress,  // pressure-sensitive deep press
    EdgeSwipe,   // swipe originating off-screen (Windows charms, Android nav)
    LongPress,   // press and hold
};

enum class SwipeDirection : std::uint8_t { Up = 0, Down, Left, Right };

struct GestureRequest {
    GestureKind kind = GestureKind::Swipe;
    Point center{};   // anchor of the gesture
    int fingers = 2;  // 1..5
    SwipeDirection direction = SwipeDirection::Left;

    double distance = 200.0;        // swipe/pan travel, logical px
    double scale = 2.0;             // pinch: >1 zoom in, <1 zoom out
    double rotation_degrees = 0.0;  // rotate: positive = counter-clockwise
    double spread = 120.0;          // initial finger separation for pinch/rotate
    double pressure = 1.0;          // force press target, 0..1

    std::chrono::milliseconds duration{300};
    std::chrono::milliseconds hold{0};  // dwell at the end before lift
    std::vector<Point> path;            // Pan only; overrides distance
    Modifier modifiers = Modifier::None;
    // Refuse to fall back to the semantic emulation on macOS. Set this when a
    // wrong-but-plausible substitute would be worse than a clear failure.
    bool require_native = false;
};

enum class GestureFidelity : std::uint8_t {
    Unsupported = 0,
    Emulated = 1,  // approximated with scroll/keyboard equivalents
    Native = 2,    // real synthesized touch contacts
};

struct GestureSupport {
    GestureFidelity fidelity = GestureFidelity::Unsupported;
    int max_fingers = 0;
    std::string backend;  // "InjectTouchInput", "uinput-mt", "CGEvent-private", "scroll-emulation"
    std::string note;
};

// ---------------------------------------------------------------------------

struct TypeOptions {
    // Characters per second. 0 = as fast as the backend allows (a single
    // batched unicode injection where the platform supports it).
    double cps = 0;
    // Use the clipboard for long text: paste is ~100x faster than per-character
    // injection and immune to layout problems, but it clobbers the clipboard
    // (restored afterwards) and some fields reject paste.
    bool allow_clipboard_fast_path = true;
    int clipboard_threshold = 256;  // characters
    bool restore_clipboard = true;
    // Press Return at the end.
    bool press_enter = false;
    Modifier modifiers = Modifier::None;
};

// The mouse/keyboard/touch backend for one platform.
class InputBackend {
public:
    virtual ~InputBackend() = default;

    static Result<std::unique_ptr<InputBackend>> create(std::shared_ptr<DisplayGraph> displays);

    virtual std::string name() const = 0;
    // Everything that could block at startup (TCC prompts, /dev/uinput open,
    // XTest probe) happens here so callers get one clear error.
    virtual Status initialize() = 0;

    // --- pointer -----------------------------------------------------------
    virtual Result<Point> cursor_position() = 0;
    virtual Status move(const Point& to, const MotionOptions& opts) = 0;
    virtual Status click(const Point& at, const ClickOptions& opts) = 0;
    virtual Status click_here(const ClickOptions& opts) = 0;
    virtual Status button_down(MouseButton b, Modifier mods) = 0;
    virtual Status button_up(MouseButton b, Modifier mods) = 0;
    virtual Status scroll(const Point& at, const ScrollOptions& opts) = 0;

    // Press at `from`, travel, release at `to`. Emitted as one uninterrupted
    // event stream with a settle pause before the release, which is what makes
    // drag-and-drop actually land in most toolkits.
    virtual Status drag(const Point& from, const Point& to, const StrokeOptions& opts) = 0;
    virtual Status stroke(const std::vector<PathPoint>& path, const StrokeOptions& opts) = 0;

    // --- keyboard ----------------------------------------------------------
    virtual Status key_down(Key k) = 0;
    virtual Status key_up(Key k) = 0;
    virtual Status tap_chord(const Chord& c, int repeat) = 0;
    virtual Status hold_chord(const Chord& c, std::chrono::milliseconds duration) = 0;
    virtual Status type_text(std::string_view utf8, const TypeOptions& opts) = 0;

    // --- gestures ----------------------------------------------------------
    virtual GestureSupport gesture_support(GestureKind kind, int fingers) const = 0;
    virtual Status gesture(const GestureRequest& req) = 0;

    // Releases every button/key this backend is currently holding. Called on
    // shutdown and after an error mid-drag so the desktop is never left with a
    // stuck modifier or a held mouse button.
    virtual Status release_all() = 0;

protected:
    std::shared_ptr<DisplayGraph> displays_;
};

}  // namespace cc
