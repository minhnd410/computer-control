// SPDX-License-Identifier: MIT
//
// macOS input backend, built on CGEvent.
//
// Notes that cost real debugging time and are easy to lose:
//
//  * CGEvent coordinates are global *points* (Space::Logical), with the origin
//    at the top-left of the main display. They are NOT pixels; posting a
//    Retina pixel coordinate lands at double the intended position.
//
//  * Multi-clicks need kCGMouseEventClickState set to 2 or 3 on the whole
//    down/up pair. Posting two independent single clicks does not produce a
//    double-click in AppKit no matter how fast they are.
//
//  * Drag requires kCGEventLeftMouseDragged between down and up. A move event
//    while the button is held is ignored by most drag sources.
//
//  * Events posted to kCGHIDEventTap are subject to the current modifier
//    state of the real keyboard. Every event therefore carries an explicit
//    flags mask rather than relying on separate modifier key-down events.

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#include "cc/input.hpp"
#include "core/motion.hpp"

namespace cc {
namespace {

CGKeyCode to_cgkeycode(Key k) {
    switch (k) {
        case Key::A: return 0;
        case Key::S: return 1;
        case Key::D: return 2;
        case Key::F: return 3;
        case Key::H: return 4;
        case Key::G: return 5;
        case Key::Z: return 6;
        case Key::X: return 7;
        case Key::C: return 8;
        case Key::V: return 9;
        case Key::B: return 11;
        case Key::Q: return 12;
        case Key::W: return 13;
        case Key::E: return 14;
        case Key::R: return 15;
        case Key::Y: return 16;
        case Key::T: return 17;
        case Key::O: return 31;
        case Key::U: return 32;
        case Key::I: return 34;
        case Key::P: return 35;
        case Key::L: return 37;
        case Key::J: return 38;
        case Key::K: return 40;
        case Key::N: return 45;
        case Key::M: return 46;

        case Key::Digit1: return 18;
        case Key::Digit2: return 19;
        case Key::Digit3: return 20;
        case Key::Digit4: return 21;
        case Key::Digit5: return 23;
        case Key::Digit6: return 22;
        case Key::Digit7: return 26;
        case Key::Digit8: return 28;
        case Key::Digit9: return 25;
        case Key::Digit0: return 29;

        case Key::Equal: return 24;
        case Key::Minus: return 27;
        case Key::RightBracket: return 30;
        case Key::LeftBracket: return 33;
        case Key::Return: return 36;
        case Key::Quote: return 39;
        case Key::Semicolon: return 41;
        case Key::Backslash: return 42;
        case Key::Comma: return 43;
        case Key::Slash: return 44;
        case Key::Period: return 47;
        case Key::Tab: return 48;
        case Key::Space: return 49;
        case Key::Grave: return 50;
        case Key::Backspace: return 51;
        case Key::Escape: return 53;
        case Key::CapsLock: return 57;

        case Key::LeftMeta: return 55;
        case Key::RightMeta: return 54;
        case Key::LeftShift: return 56;
        case Key::RightShift: return 60;
        case Key::LeftAlt: return 58;
        case Key::RightAlt: return 61;
        case Key::LeftControl: return 59;
        case Key::RightControl: return 62;
        case Key::Fn: return 63;

        case Key::KeypadPeriod: return 65;
        case Key::KeypadMultiply: return 67;
        case Key::KeypadPlus: return 69;
        case Key::KeypadDivide: return 75;
        case Key::KeypadEnter: return 76;
        case Key::KeypadMinus: return 78;
        case Key::KeypadEqual: return 81;
        case Key::Keypad0: return 82;
        case Key::Keypad1: return 83;
        case Key::Keypad2: return 84;
        case Key::Keypad3: return 85;
        case Key::Keypad4: return 86;
        case Key::Keypad5: return 87;
        case Key::Keypad6: return 88;
        case Key::Keypad7: return 89;
        case Key::Keypad8: return 91;
        case Key::Keypad9: return 92;

        case Key::F1: return 122;
        case Key::F2: return 120;
        case Key::F3: return 99;
        case Key::F4: return 118;
        case Key::F5: return 96;
        case Key::F6: return 97;
        case Key::F7: return 98;
        case Key::F8: return 100;
        case Key::F9: return 101;
        case Key::F10: return 109;
        case Key::F11: return 103;
        case Key::F12: return 111;
        case Key::F13: return 105;
        case Key::F14: return 107;
        case Key::F15: return 113;
        case Key::F16: return 106;
        case Key::F17: return 64;
        case Key::F18: return 79;
        case Key::F19: return 80;

        case Key::Help: return 114;
        case Key::Home: return 115;
        case Key::PageUp: return 116;
        case Key::Delete: return 117;
        case Key::End: return 119;
        case Key::PageDown: return 121;
        case Key::Left: return 123;
        case Key::Right: return 124;
        case Key::Down: return 125;
        case Key::Up: return 126;

        case Key::VolumeUp: return 72;
        case Key::VolumeDown: return 73;
        case Key::Mute: return 74;

        default: return 0xFFFF;
    }
}

CGEventFlags to_cgflags(Modifier m) {
    CGEventFlags f = 0;
    if (has(m, Modifier::Shift)) f |= kCGEventFlagMaskShift;
    if (has(m, Modifier::Control)) f |= kCGEventFlagMaskControl;
    if (has(m, Modifier::Alt)) f |= kCGEventFlagMaskAlternate;
    if (has(m, Modifier::Meta)) f |= kCGEventFlagMaskCommand;
    if (has(m, Modifier::CapsLock)) f |= kCGEventFlagMaskAlphaShift;
    if (has(m, Modifier::Fn)) f |= kCGEventFlagMaskSecondaryFn;
    return f;
}

struct ButtonEvents {
    CGEventType down, up, dragged;
    CGMouseButton button;
};

ButtonEvents events_for(MouseButton b) {
    switch (b) {
        case MouseButton::Left:
            return {kCGEventLeftMouseDown, kCGEventLeftMouseUp, kCGEventLeftMouseDragged,
                    kCGMouseButtonLeft};
        case MouseButton::Right:
            return {kCGEventRightMouseDown, kCGEventRightMouseUp, kCGEventRightMouseDragged,
                    kCGMouseButtonRight};
        default:
            // macOS routes buttons 2..31 through the "other" event family.
            return {kCGEventOtherMouseDown, kCGEventOtherMouseUp, kCGEventOtherMouseDragged,
                    static_cast<CGMouseButton>(static_cast<int>(b))};
    }
}

void sleep_us(std::chrono::microseconds us) {
    if (us.count() <= 0) return;
    std::this_thread::sleep_for(us);
}

class ScopedEvent {
public:
    explicit ScopedEvent(CGEventRef e) : e_(e) {}
    ~ScopedEvent() {
        if (e_) CFRelease(e_);
    }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
    CGEventRef get() const { return e_; }
    explicit operator bool() const { return e_ != nullptr; }

private:
    CGEventRef e_;
};

class MacInput final : public InputBackend {
public:
    explicit MacInput(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }
    ~MacInput() override { (void)release_all(); }

    std::string name() const override { return "CGEvent"; }

    Status initialize() override {
        // A posted event is silently dropped when the host process lacks
        // Accessibility access. There is no error return from CGEventPost, so
        // the only way to give a useful message is to check up front.
        if (!AXIsProcessTrusted()) {
            return err(ErrorCode::PermissionDenied,
                       "this process is not trusted for Accessibility, so synthesized input "
                       "will be silently discarded",
                       "System Settings > Privacy & Security > Accessibility, then add and "
                       "enable the binary or the terminal running it. Restart the process "
                       "afterwards; the trust state is cached at launch.");
        }
        source_ = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
        if (!source_) {
            return err(ErrorCode::BackendFailure, "CGEventSourceCreate failed");
        }
        // Suppress the local-events delay that would otherwise make the real
        // mouse fight our synthetic one for ~250ms after each post.
        CGEventSourceSetLocalEventsSuppressionInterval(source_, 0.0);
        return ok();
    }

    // --- pointer -----------------------------------------------------------

    Result<Point> cursor_position() override {
        ScopedEvent e{CGEventCreate(nullptr)};
        if (!e) return err(ErrorCode::BackendFailure, "CGEventCreate failed");
        const CGPoint p = CGEventGetLocation(e.get());
        return Point{p.x, p.y, Space::Logical};
    }

    Status move(const Point& to, const MotionOptions& opts) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        return emit_motion(cur.value(), to, opts, /*button=*/nullptr, Modifier::None);
    }

    Status click(const Point& at, const ClickOptions& opts) override {
        if (opts.move_first) {
            MotionOptions m;
            m.profile = MotionProfile::EaseInOut;
            m.duration = std::chrono::milliseconds{90};
            if (auto st = move(at, m); !st) return st;
        }
        if (opts.count <= 0) return ok();  // hover only
        return emit_clicks(at, opts);
    }

    Status click_here(const ClickOptions& opts) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        ClickOptions o = opts;
        o.move_first = false;
        return emit_clicks(cur.value(), o);
    }

    Status button_down(MouseButton b, Modifier mods) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        const auto ev = events_for(b);
        if (!post_mouse(ev.down, cur.value(), ev.button, 1, mods)) {
            return err(ErrorCode::BackendFailure, "failed to post mouse down");
        }
        std::lock_guard<std::mutex> lk(mu_);
        held_buttons_.insert(b);
        return ok();
    }

    Status button_up(MouseButton b, Modifier mods) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        const auto ev = events_for(b);
        if (!post_mouse(ev.up, cur.value(), ev.button, 1, mods)) {
            return err(ErrorCode::BackendFailure, "failed to post mouse up");
        }
        std::lock_guard<std::mutex> lk(mu_);
        held_buttons_.erase(b);
        return ok();
    }

    Status scroll(const Point& at, const ScrollOptions& opts) override {
        MotionOptions m;
        m.duration = std::chrono::milliseconds{60};
        if (auto st = move(at, m); !st) return st;
        return emit_scroll(opts, opts.clicks);
    }

    Status drag(const Point& from, const Point& to, const StrokeOptions& opts) override {
        std::vector<PathPoint> path{PathPoint{from}, PathPoint{to}};
        return stroke(path, opts);
    }

    Status stroke(const std::vector<PathPoint>& path_in, const StrokeOptions& opts) override {
        if (path_in.size() < 2) {
            return err(ErrorCode::InvalidArgument, "a stroke needs at least two points");
        }
        std::vector<PathPoint> path = path_in;
        if (opts.smooth) path = motion::smooth_catmull_rom(path, opts.smooth_tension, 8);

        const auto ev = events_for(opts.button);
        const Point start = path.front().at;

        // Land the pointer first, without the button held, so hover state
        // settles before the press. Drag sources that arm on mouse-enter need
        // this; without it the first drag of a session often does nothing.
        MotionOptions approach;
        approach.duration = std::chrono::milliseconds{80};
        if (auto st = move(start, approach); !st) return st;
        sleep_us(std::chrono::microseconds{20000});

        if (!post_mouse(ev.down, start, ev.button, 1, opts.modifiers)) {
            return err(ErrorCode::BackendFailure, "failed to post drag press");
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.insert(opts.button);
        }
        // Small dwell after the press: many drag sources require the button to
        // be down for a frame or two before motion counts as a drag.
        sleep_us(std::chrono::microseconds{30000});

        const auto samples = motion::polyline(path, opts.motion);
        Status result = ok();
        for (const auto& s : samples) {
            if (!post_mouse(ev.dragged, s.at, ev.button, 1, opts.modifiers)) {
                result = err(ErrorCode::BackendFailure, "failed to post drag motion");
                break;
            }
            sleep_us(s.delay);
        }

        // The release must happen even if motion failed, or the desktop is
        // left with a stuck mouse button.
        sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.settle_before_release));
        const Point end = path.back().at;
        if (!post_mouse(ev.up, end, ev.button, 1, opts.modifiers) && result.ok()) {
            result = err(ErrorCode::BackendFailure, "failed to post drag release");
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.erase(opts.button);
        }
        return result;
    }

    // --- keyboard ----------------------------------------------------------

    Status key_down(Key k) override {
        const CGKeyCode code = to_cgkeycode(k);
        if (code == 0xFFFF) return unmapped_key(k);
        ScopedEvent e{CGEventCreateKeyboardEvent(source_, code, true)};
        if (!e) return err(ErrorCode::BackendFailure, "CGEventCreateKeyboardEvent failed");
        if (is_modifier_key(k)) {
            std::lock_guard<std::mutex> lk(mu_);
            held_modifiers_ |= modifier_for_key(k);
            CGEventSetFlags(e.get(), to_cgflags(held_modifiers_));
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            CGEventSetFlags(e.get(), to_cgflags(held_modifiers_));
        }
        CGEventPost(kCGHIDEventTap, e.get());
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_keys_.insert(k);
        }
        return ok();
    }

    Status key_up(Key k) override {
        const CGKeyCode code = to_cgkeycode(k);
        if (code == 0xFFFF) return unmapped_key(k);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (is_modifier_key(k)) {
                held_modifiers_ =
                    static_cast<Modifier>(static_cast<std::uint32_t>(held_modifiers_) &
                                          ~static_cast<std::uint32_t>(modifier_for_key(k)));
            }
            held_keys_.erase(k);
        }
        ScopedEvent e{CGEventCreateKeyboardEvent(source_, code, false)};
        if (!e) return err(ErrorCode::BackendFailure, "CGEventCreateKeyboardEvent failed");
        {
            std::lock_guard<std::mutex> lk(mu_);
            CGEventSetFlags(e.get(), to_cgflags(held_modifiers_));
        }
        CGEventPost(kCGHIDEventTap, e.get());
        return ok();
    }

    Status tap_chord(const Chord& c, int repeat) override {
        repeat = std::max(1, repeat);
        for (int i = 0; i < repeat; ++i) {
            for (Key k : c.keys) {
                const CGKeyCode code = to_cgkeycode(k);
                if (code == 0xFFFF) return unmapped_key(k);
                const CGEventFlags flags = to_cgflags(c.modifiers) | current_flags();
                ScopedEvent down{CGEventCreateKeyboardEvent(source_, code, true)};
                ScopedEvent up{CGEventCreateKeyboardEvent(source_, code, false)};
                if (!down || !up)
                    return err(ErrorCode::BackendFailure, "key event creation failed");
                CGEventSetFlags(down.get(), flags);
                CGEventSetFlags(up.get(), flags);
                CGEventPost(kCGHIDEventTap, down.get());
                sleep_us(std::chrono::microseconds{8000});
                CGEventPost(kCGHIDEventTap, up.get());
            }
            if (i + 1 < repeat) sleep_us(std::chrono::microseconds{25000});
        }
        return ok();
    }

    Status hold_chord(const Chord& c, std::chrono::milliseconds duration) override {
        std::vector<Key> pressed;
        const CGEventFlags flags = to_cgflags(c.modifiers) | current_flags();
        for (Key k : c.keys) {
            const CGKeyCode code = to_cgkeycode(k);
            if (code == 0xFFFF) {
                for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) (void)key_up(*it);
                return unmapped_key(k);
            }
            ScopedEvent e{CGEventCreateKeyboardEvent(source_, code, true)};
            if (!e) return err(ErrorCode::BackendFailure, "key event creation failed");
            CGEventSetFlags(e.get(), flags);
            // Auto-repeat so the target sees a genuine held key rather than a
            // single press followed by silence.
            CGEventSetIntegerValueField(e.get(), kCGKeyboardEventAutorepeat, 0);
            CGEventPost(kCGHIDEventTap, e.get());
            pressed.push_back(k);
        }
        std::this_thread::sleep_for(duration);
        for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) {
            ScopedEvent e{CGEventCreateKeyboardEvent(source_, to_cgkeycode(*it), false)};
            if (e) {
                CGEventSetFlags(e.get(), flags);
                CGEventPost(kCGHIDEventTap, e.get());
            }
        }
        return ok();
    }

    Status type_text(std::string_view utf8, const TypeOptions& opts) override {
        // Unicode injection rather than keycode lookup: it is layout
        // independent, handles emoji and CJK, and avoids dead-key sequences
        // entirely. The cost is that the target sees keystrokes with no
        // meaningful keycode, which a small number of games reject.
        const auto utf16 = to_utf16(utf8);
        if (utf16.empty()) return maybe_enter(opts);

        // CGEventKeyboardSetUnicodeString accepts a run of characters per
        // event. Chunking keeps each event small enough that AppKit's text
        // input system delivers it intact.
        constexpr std::size_t kChunk = 20;
        const auto per_char_delay =
            (opts.cps > 0)
                ? std::chrono::microseconds{static_cast<long long>(1'000'000.0 / opts.cps)}
                : std::chrono::microseconds{0};

        for (std::size_t i = 0; i < utf16.size(); i += kChunk) {
            const std::size_t n = std::min(kChunk, utf16.size() - i);
            ScopedEvent down{CGEventCreateKeyboardEvent(source_, 0, true)};
            ScopedEvent up{CGEventCreateKeyboardEvent(source_, 0, false)};
            if (!down || !up) return err(ErrorCode::BackendFailure, "key event creation failed");
            CGEventKeyboardSetUnicodeString(down.get(), n, utf16.data() + i);
            CGEventKeyboardSetUnicodeString(up.get(), n, utf16.data() + i);
            const CGEventFlags flags = to_cgflags(opts.modifiers) | current_flags();
            CGEventSetFlags(down.get(), flags);
            CGEventSetFlags(up.get(), flags);
            CGEventPost(kCGHIDEventTap, down.get());
            CGEventPost(kCGHIDEventTap, up.get());
            if (per_char_delay.count() > 0)
                sleep_us(per_char_delay * static_cast<long long>(n));
            else
                sleep_us(std::chrono::microseconds{1500});
        }
        return maybe_enter(opts);
    }

    // --- gestures ----------------------------------------------------------

    GestureSupport gesture_support(GestureKind kind, int fingers) const override {
        GestureSupport s;
        switch (kind) {
            case GestureKind::Swipe:
            case GestureKind::Pan:
                if (fingers <= 2) {
                    // Phased scroll events with kCGScrollWheelEventScrollPhase
                    // are exactly what a trackpad emits, so momentum-aware apps
                    // treat these as a genuine two-finger gesture.
                    s.fidelity = GestureFidelity::Native;
                    s.backend = "CGEvent-phased-scroll";
                    s.max_fingers = 2;
                    s.note = "Real trackpad-phase scroll events.";
                } else {
                    s.fidelity = GestureFidelity::Emulated;
                    s.backend = "mission-control-keys";
                    s.max_fingers = 4;
                    s.note =
                        "3+ finger swipes map to the Spaces/Mission Control key "
                        "equivalents (ctrl+arrow, ctrl+up). The system reacts correctly; "
                        "an app listening for raw touches does not see contacts.";
                }
                return s;
            case GestureKind::Pinch:
            case GestureKind::SmartZoom:
                s.fidelity = GestureFidelity::Emulated;
                s.backend = "cmd-scroll";
                s.max_fingers = 2;
                s.note =
                    "Pinch maps to command+scroll, which nearly every macOS app treats "
                    "as zoom. Apps that implement magnification only via NSMagnify "
                    "will not respond.";
                return s;
            case GestureKind::Tap:
            case GestureKind::LongPress:
                s.fidelity = (fingers <= 1) ? GestureFidelity::Native : GestureFidelity::Emulated;
                s.backend = (fingers <= 1) ? "CGEvent-click" : "CGEvent-click+modifier";
                s.max_fingers = 2;
                s.note = (fingers == 2) ? "A two-finger tap is emitted as a right click, which "
                                          "is what it means on a Mac trackpad."
                                        : "";
                return s;
            case GestureKind::ForcePress:
                s.fidelity = GestureFidelity::Emulated;
                s.backend = "long-press";
                s.max_fingers = 1;
                s.note =
                    "Force Touch pressure cannot be synthesized; emitted as a long press, "
                    "which triggers the same look-up behaviour in most apps.";
                return s;
            case GestureKind::Rotate:
                s.fidelity = GestureFidelity::Unsupported;
                s.backend = "none";
                s.note =
                    "macOS exposes no public API for synthesizing rotation, and there is "
                    "no keyboard or scroll equivalent that generalises. Drive the app's "
                    "own rotate control instead.";
                return s;
            case GestureKind::EdgeSwipe:
                s.fidelity = GestureFidelity::Emulated;
                s.backend = "phased-scroll-from-edge";
                s.max_fingers = 2;
                return s;
        }
        return s;
    }

    Status gesture(const GestureRequest& req) override {
        const auto support = gesture_support(req.kind, req.fingers);
        if (support.fidelity == GestureFidelity::Unsupported) {
            return err(ErrorCode::Unsupported,
                       std::string("gesture not supported on macOS: ") + support.note,
                       "Call Capabilities to see which gestures this platform can perform.");
        }
        if (req.require_native && support.fidelity != GestureFidelity::Native) {
            return err(ErrorCode::Unsupported,
                       "require_native was set but macOS can only emulate this gesture (" +
                           support.backend + ")",
                       "Drop require_native to accept the emulation, or run the gesture on "
                       "Windows/Linux where real touch injection is available.");
        }

        switch (req.kind) {
            case GestureKind::Tap: return gesture_tap(req);
            case GestureKind::LongPress: return gesture_long_press(req);
            case GestureKind::ForcePress: return gesture_long_press(req);
            case GestureKind::Pinch:
            case GestureKind::SmartZoom: return gesture_pinch(req);
            case GestureKind::Swipe:
            case GestureKind::EdgeSwipe: return gesture_swipe(req);
            case GestureKind::Pan: return gesture_pan(req);
            case GestureKind::Rotate:
                return err(ErrorCode::Unsupported, "rotation cannot be synthesized on macOS");
        }
        return err(ErrorCode::InvalidArgument, "unknown gesture kind");
    }

    Status release_all() override {
        std::set<MouseButton> buttons;
        std::set<Key> keys;
        {
            std::lock_guard<std::mutex> lk(mu_);
            buttons = held_buttons_;
            keys = held_keys_;
        }
        for (MouseButton b : buttons) (void)button_up(b, Modifier::None);
        for (Key k : keys) (void)key_up(k);
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.clear();
            held_keys_.clear();
            held_modifiers_ = Modifier::None;
        }
        return ok();
    }

private:
    Status unmapped_key(Key k) {
        return err(ErrorCode::Unsupported, "no macOS keycode for key '" + to_string(k) + "'",
                   "Media and brightness keys other than volume require HID usage pages that "
                   "CGEvent does not expose. Use the app's own menu command instead.");
    }

    Status maybe_enter(const TypeOptions& opts) {
        if (!opts.press_enter) return ok();
        Chord c;
        c.keys.push_back(Key::Return);
        return tap_chord(c, 1);
    }

    CGEventFlags current_flags() {
        std::lock_guard<std::mutex> lk(mu_);
        return to_cgflags(held_modifiers_);
    }

    static std::vector<UniChar> to_utf16(std::string_view utf8) {
        std::vector<UniChar> out;
        out.reserve(utf8.size());
        std::size_t i = 0;
        while (i < utf8.size()) {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            char32_t cp = 0;
            int extra = 0;
            if (c < 0x80) {
                cp = c;
                extra = 0;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                extra = 1;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                extra = 2;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                extra = 3;
            } else {
                ++i;
                continue;
            }  // invalid lead byte, skip
            if (i + static_cast<std::size_t>(extra) >= utf8.size()) break;
            for (int k = 1; k <= extra; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
            }
            i += static_cast<std::size_t>(extra) + 1;
            if (cp < 0x10000) {
                out.push_back(static_cast<UniChar>(cp));
            } else {
                cp -= 0x10000;
                out.push_back(static_cast<UniChar>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<UniChar>(0xDC00 + (cp & 0x3FF)));
            }
        }
        return out;
    }

    bool post_mouse(CGEventType type, const Point& at, CGMouseButton button, int click_state,
                    Modifier mods) {
        ScopedEvent e{CGEventCreateMouseEvent(source_, type, CGPointMake(at.x, at.y), button)};
        if (!e) return false;
        CGEventSetIntegerValueField(e.get(), kCGMouseEventClickState, click_state);
        CGEventSetFlags(e.get(), to_cgflags(mods) | current_flags());
        CGEventPost(kCGHIDEventTap, e.get());
        return true;
    }

    Status emit_motion(const Point& from, const Point& to, const MotionOptions& opts,
                       const MouseButton* dragging, Modifier mods) {
        const auto samples = motion::line(from, to, opts);
        const CGEventType type = dragging ? events_for(*dragging).dragged : kCGEventMouseMoved;
        const CGMouseButton btn = dragging ? events_for(*dragging).button : kCGMouseButtonLeft;
        for (const auto& s : samples) {
            if (!post_mouse(type, s.at, btn, 1, mods)) {
                return err(ErrorCode::BackendFailure, "failed to post mouse motion");
            }
            sleep_us(s.delay);
        }
        return ok();
    }

    Status emit_clicks(const Point& at, const ClickOptions& opts) {
        const auto ev = events_for(opts.button);
        const int n = std::clamp(opts.count, 1, 3);
        for (int i = 1; i <= n; ++i) {
            // click_state is cumulative: 1, then 2, then 3. AppKit derives
            // double/triple click semantics from this field, not from timing.
            if (!post_mouse(ev.down, at, ev.button, i, opts.modifiers)) {
                return err(ErrorCode::BackendFailure, "failed to post mouse down");
            }
            sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.press_duration));
            if (!post_mouse(ev.up, at, ev.button, i, opts.modifiers)) {
                return err(ErrorCode::BackendFailure, "failed to post mouse up");
            }
            if (i < n) {
                sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.inter_click));
            }
        }
        return ok();
    }

    // Emits one scroll event. `phase` is 0 for a plain wheel event, or one of
    // the kCGScrollPhase* values for a trackpad-style gesture.
    bool post_scroll(double dy, double dx, bool pixel_units, Modifier mods, int phase,
                     int momentum_phase) {
        const CGScrollEventUnit unit =
            pixel_units ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine;
        ScopedEvent e{CGEventCreateScrollWheelEvent(source_, unit, 2,
                                                    static_cast<std::int32_t>(std::lround(dy)),
                                                    static_cast<std::int32_t>(std::lround(dx)))};
        if (!e) return false;
        CGEventSetFlags(e.get(), to_cgflags(mods) | current_flags());
        if (phase >= 0) {
            CGEventSetIntegerValueField(e.get(), kCGScrollWheelEventScrollPhase, phase);
        }
        if (momentum_phase >= 0) {
            CGEventSetIntegerValueField(e.get(), kCGScrollWheelEventMomentumPhase, momentum_phase);
        }
        CGEventPost(kCGHIDEventTap, e.get());
        return true;
    }

    Status emit_scroll(const ScrollOptions& opts, int clicks) {
        double dy = 0, dx = 0;
        const double mag = opts.pixel_units ? opts.pixels_per_click : 1.0;
        switch (opts.direction) {
            case ScrollDirection::Up: dy = mag; break;
            case ScrollDirection::Down: dy = -mag; break;
            case ScrollDirection::Left: dx = mag; break;
            case ScrollDirection::Right: dx = -mag; break;
        }
        if (opts.axis == ScrollAxis::Horizontal && dx == 0) {
            dx = dy;
            dy = 0;
        }

        const int n = std::max(1, clicks);
        if (!opts.phased) {
            for (int i = 0; i < n; ++i) {
                if (!post_scroll(dy, dx, opts.pixel_units, opts.modifiers, -1, -1)) {
                    return err(ErrorCode::BackendFailure, "failed to post scroll");
                }
                sleep_us(std::chrono::microseconds{12000});
            }
            return ok();
        }

        // kCGScrollPhaseBegan = 1, Changed = 2, Ended = 4.
        if (!post_scroll(dy, dx, opts.pixel_units, opts.modifiers, 1, 0)) {
            return err(ErrorCode::BackendFailure, "failed to post scroll begin");
        }
        for (int i = 1; i < n; ++i) {
            if (!post_scroll(dy, dx, opts.pixel_units, opts.modifiers, 2, 0)) {
                return err(ErrorCode::BackendFailure, "failed to post scroll");
            }
            sleep_us(std::chrono::microseconds{8000});
        }
        if (!post_scroll(0, 0, opts.pixel_units, opts.modifiers, 4, 0)) {
            return err(ErrorCode::BackendFailure, "failed to post scroll end");
        }
        return ok();
    }

    Status gesture_tap(const GestureRequest& req) {
        ClickOptions o;
        o.modifiers = req.modifiers;
        // A two-finger tap on a Mac trackpad is a secondary click; three or
        // more have no click meaning, so they fall back to a plain click.
        o.button = (req.fingers == 2) ? MouseButton::Right : MouseButton::Left;
        o.count = 1;
        return click(req.center, o);
    }

    Status gesture_long_press(const GestureRequest& req) {
        MotionOptions m;
        m.duration = std::chrono::milliseconds{90};
        if (auto st = move(req.center, m); !st) return st;
        if (auto st = button_down(MouseButton::Left, req.modifiers); !st) return st;
        const auto hold = (req.hold.count() > 0) ? req.hold : std::chrono::milliseconds{600};
        std::this_thread::sleep_for(hold);
        return button_up(MouseButton::Left, req.modifiers);
    }

    Status gesture_pinch(const GestureRequest& req) {
        MotionOptions m;
        m.duration = std::chrono::milliseconds{80};
        if (auto st = move(req.center, m); !st) return st;

        // Command+scroll is the near-universal zoom idiom on macOS. Convert the
        // requested scale factor into a plausible number of detents: each
        // detent is roughly a 10% step in most apps, so log-scale it.
        const double factor = std::max(0.05, req.scale);
        const int detents = std::clamp(
            static_cast<int>(std::lround(std::fabs(std::log(factor)) / std::log(1.10))), 1, 60);

        ScrollOptions so;
        so.axis = ScrollAxis::Vertical;
        so.direction = (factor >= 1.0) ? ScrollDirection::Up : ScrollDirection::Down;
        so.clicks = detents;
        so.modifiers = req.modifiers | Modifier::Meta;
        so.phased = false;

        const auto per = std::chrono::microseconds{
            std::max<long long>(4000, req.duration.count() * 1000 / std::max(1, detents))};
        for (int i = 0; i < detents; ++i) {
            ScrollOptions one = so;
            one.clicks = 1;
            if (auto st = emit_scroll(one, 1); !st) return st;
            sleep_us(per);
        }
        return ok();
    }

    Status gesture_swipe(const GestureRequest& req) {
        if (req.fingers >= 3) return gesture_spaces_swipe(req);

        MotionOptions m;
        m.duration = std::chrono::milliseconds{70};
        if (auto st = move(req.center, m); !st) return st;

        // A real two-finger swipe is a phased pixel scroll in the *opposite*
        // direction to the content movement: swiping left moves content right.
        const int steps = motion::gesture_steps(req, 90);
        const double per_step = req.distance / std::max(1, steps);
        double dx = 0, dy = 0;
        switch (req.direction) {
            case SwipeDirection::Left: dx = -per_step; break;
            case SwipeDirection::Right: dx = per_step; break;
            case SwipeDirection::Up: dy = -per_step; break;
            case SwipeDirection::Down: dy = per_step; break;
        }

        if (!post_scroll(dy, dx, true, req.modifiers, 1, 0)) {
            return err(ErrorCode::BackendFailure, "failed to post swipe begin");
        }
        const auto per = std::chrono::microseconds{
            std::max<long long>(2000, req.duration.count() * 1000 / std::max(1, steps))};
        for (int i = 1; i < steps; ++i) {
            if (!post_scroll(dy, dx, true, req.modifiers, 2, 0)) {
                return err(ErrorCode::BackendFailure, "failed to post swipe motion");
            }
            sleep_us(per);
        }
        if (!post_scroll(0, 0, true, req.modifiers, 4, 0)) {
            return err(ErrorCode::BackendFailure, "failed to post swipe end");
        }
        if (req.hold.count() > 0) std::this_thread::sleep_for(req.hold);
        return ok();
    }

    Status gesture_spaces_swipe(const GestureRequest& req) {
        // Three- and four-finger swipes are system gestures on macOS: they move
        // between Spaces, open Mission Control, or show App Exposé. Their
        // keyboard equivalents produce the identical system response.
        //
        // The direction mapping follows the trackpad, where content tracks the
        // fingers: swiping *left* pulls the next Space in from the right, which
        // is ctrl+Right. Mapping left to ctrl+Left - the obvious-looking
        // choice - moves the wrong way.
        Chord c;
        c.modifiers = Modifier::Control;
        switch (req.direction) {
            case SwipeDirection::Left: c.keys.push_back(Key::Right); break;
            case SwipeDirection::Right: c.keys.push_back(Key::Left); break;
            case SwipeDirection::Up: c.keys.push_back(Key::Up); break;
            case SwipeDirection::Down: c.keys.push_back(Key::Down); break;
        }
        return tap_chord(c, 1);
    }

    Status gesture_pan(const GestureRequest& req) {
        if (req.path.size() >= 2) {
            // A multi-finger pan along an explicit path has no scroll analogue;
            // emit it as a drag, which is what a pan does inside a canvas.
            std::vector<PathPoint> pts;
            pts.reserve(req.path.size());
            for (const auto& p : req.path) pts.push_back(PathPoint{p});
            StrokeOptions so;
            so.modifiers = req.modifiers;
            so.motion.duration = req.duration;
            return stroke(pts, so);
        }
        return gesture_swipe(req);
    }

    CGEventSourceRef source_ = nullptr;
    std::mutex mu_;
    std::set<MouseButton> held_buttons_;
    std::set<Key> held_keys_;
    Modifier held_modifiers_ = Modifier::None;
};

}  // namespace

Result<std::unique_ptr<InputBackend>> InputBackend::create(std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<MacInput>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<InputBackend>(std::move(backend));
}

const char* to_string(MouseButton b) noexcept {
    switch (b) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
        case MouseButton::Back: return "back";
        case MouseButton::Forward: return "forward";
    }
    return "left";
}

Result<MouseButton> button_from_string(std::string_view s) {
    if (s == "left") return MouseButton::Left;
    if (s == "right" || s == "secondary") return MouseButton::Right;
    if (s == "middle") return MouseButton::Middle;
    if (s == "back") return MouseButton::Back;
    if (s == "forward") return MouseButton::Forward;
    return err(ErrorCode::InvalidArgument, "unknown mouse button: " + std::string(s));
}

}  // namespace cc
