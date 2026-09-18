// SPDX-License-Identifier: MIT
//
// Linux input: XTest for mouse and keyboard, a virtual uinput multitouch
// device for gestures.
//
// XTest can only move a pointer and press keys - it has no concept of a touch
// contact, so pinch, rotate and n-finger swipes are impossible through it. The
// kernel's uinput interface can create a virtual multitouch touchscreen that
// libinput and the compositor treat as real hardware, which gives genuine
// multi-touch. The catch is that /dev/uinput is root-owned on most
// distributions, so the gesture path reports exactly what is missing and how
// to grant it rather than failing opaquely.
//
// Wayland: XTest works only through XWayland and cannot reach native Wayland
// clients. The portal-based route (xdg-desktop-portal RemoteDesktop over
// libei) is the supported answer there; this backend detects Wayland and says
// so instead of silently doing nothing, which is the single most confusing
// failure mode in this whole space.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cc/input.hpp"
#include "core/motion.hpp"

#if defined(CC_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#if defined(CC_HAVE_XTEST)
#include <X11/extensions/XTest.h>
#endif
#endif

#if defined(__linux__)
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace cc {

#if defined(CC_HAVE_X11)
::Display* x11_display() {
    static ::Display* dpy = [] {
        // Xlib is only safe from multiple threads after this, and the MCP
        // server handles requests off the main thread.
        ::XInitThreads();
        return ::XOpenDisplay(nullptr);
    }();
    return dpy;
}
#endif

namespace {

bool running_wayland() {
    if (const char* t = std::getenv("XDG_SESSION_TYPE")) {
        if (std::strcmp(t, "wayland") == 0) return true;
    }
    return std::getenv("WAYLAND_DISPLAY") != nullptr;
}

void sleep_us(std::chrono::microseconds us) {
    if (us.count() > 0) std::this_thread::sleep_for(us);
}

#if defined(CC_HAVE_X11)

KeySym to_keysym(Key k) {
    switch (k) {
        case Key::A: return XK_a;
        case Key::B: return XK_b;
        case Key::C: return XK_c;
        case Key::D: return XK_d;
        case Key::E: return XK_e;
        case Key::F: return XK_f;
        case Key::G: return XK_g;
        case Key::H: return XK_h;
        case Key::I: return XK_i;
        case Key::J: return XK_j;
        case Key::K: return XK_k;
        case Key::L: return XK_l;
        case Key::M: return XK_m;
        case Key::N: return XK_n;
        case Key::O: return XK_o;
        case Key::P: return XK_p;
        case Key::Q: return XK_q;
        case Key::R: return XK_r;
        case Key::S: return XK_s;
        case Key::T: return XK_t;
        case Key::U: return XK_u;
        case Key::V: return XK_v;
        case Key::W: return XK_w;
        case Key::X: return XK_x;
        case Key::Y: return XK_y;
        case Key::Z: return XK_z;

        case Key::Digit0: return XK_0;
        case Key::Digit1: return XK_1;
        case Key::Digit2: return XK_2;
        case Key::Digit3: return XK_3;
        case Key::Digit4: return XK_4;
        case Key::Digit5: return XK_5;
        case Key::Digit6: return XK_6;
        case Key::Digit7: return XK_7;
        case Key::Digit8: return XK_8;
        case Key::Digit9: return XK_9;

        case Key::Return: return XK_Return;
        case Key::Escape: return XK_Escape;
        case Key::Backspace: return XK_BackSpace;
        case Key::Tab: return XK_Tab;
        case Key::Space: return XK_space;
        case Key::Minus: return XK_minus;
        case Key::Equal: return XK_equal;
        case Key::LeftBracket: return XK_bracketleft;
        case Key::RightBracket: return XK_bracketright;
        case Key::Backslash: return XK_backslash;
        case Key::Semicolon: return XK_semicolon;
        case Key::Quote: return XK_apostrophe;
        case Key::Grave: return XK_grave;
        case Key::Comma: return XK_comma;
        case Key::Period: return XK_period;
        case Key::Slash: return XK_slash;
        case Key::CapsLock: return XK_Caps_Lock;

        case Key::F1: return XK_F1;
        case Key::F2: return XK_F2;
        case Key::F3: return XK_F3;
        case Key::F4: return XK_F4;
        case Key::F5: return XK_F5;
        case Key::F6: return XK_F6;
        case Key::F7: return XK_F7;
        case Key::F8: return XK_F8;
        case Key::F9: return XK_F9;
        case Key::F10: return XK_F10;
        case Key::F11: return XK_F11;
        case Key::F12: return XK_F12;
        case Key::F13: return XK_F13;
        case Key::F14: return XK_F14;
        case Key::F15: return XK_F15;
        case Key::F16: return XK_F16;
        case Key::F17: return XK_F17;
        case Key::F18: return XK_F18;
        case Key::F19: return XK_F19;
        case Key::F20: return XK_F20;

        case Key::PrintScreen: return XK_Print;
        case Key::ScrollLock: return XK_Scroll_Lock;
        case Key::Pause: return XK_Pause;
        case Key::Insert: return XK_Insert;
        case Key::Home: return XK_Home;
        case Key::PageUp: return XK_Prior;
        case Key::Delete: return XK_Delete;
        case Key::End: return XK_End;
        case Key::PageDown: return XK_Next;
        case Key::Right: return XK_Right;
        case Key::Left: return XK_Left;
        case Key::Down: return XK_Down;
        case Key::Up: return XK_Up;
        case Key::NumLock: return XK_Num_Lock;

        case Key::KeypadDivide: return XK_KP_Divide;
        case Key::KeypadMultiply: return XK_KP_Multiply;
        case Key::KeypadMinus: return XK_KP_Subtract;
        case Key::KeypadPlus: return XK_KP_Add;
        case Key::KeypadEnter: return XK_KP_Enter;
        case Key::KeypadPeriod: return XK_KP_Decimal;
        case Key::Keypad0: return XK_KP_0;
        case Key::Keypad1: return XK_KP_1;
        case Key::Keypad2: return XK_KP_2;
        case Key::Keypad3: return XK_KP_3;
        case Key::Keypad4: return XK_KP_4;
        case Key::Keypad5: return XK_KP_5;
        case Key::Keypad6: return XK_KP_6;
        case Key::Keypad7: return XK_KP_7;
        case Key::Keypad8: return XK_KP_8;
        case Key::Keypad9: return XK_KP_9;

        case Key::LeftControl: return XK_Control_L;
        case Key::RightControl: return XK_Control_R;
        case Key::LeftShift: return XK_Shift_L;
        case Key::RightShift: return XK_Shift_R;
        case Key::LeftAlt: return XK_Alt_L;
        case Key::RightAlt: return XK_Alt_R;
        case Key::LeftMeta: return XK_Super_L;
        case Key::RightMeta: return XK_Super_R;
        case Key::Menu: return XK_Menu;
        case Key::Help: return XK_Help;
        default: return NoSymbol;
    }
}

#endif  // CC_HAVE_X11

#if defined(__linux__)

// A virtual multitouch touchscreen. Registering ABS_MT_SLOT and friends is
// what makes libinput classify the device as a touchscreen rather than a
// tablet, which in turn is what makes the compositor run gesture recognition
// over the contacts.
class UinputTouch {
public:
    ~UinputTouch() { close_device(); }

    bool open_device(int width, int height, int max_contacts) {
        fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd_ < 0) return false;

        width_ = std::max(1, width);
        height_ = std::max(1, height);
        slots_ = std::max(1, max_contacts);

        ::ioctl(fd_, UI_SET_EVBIT, EV_ABS);
        ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        ::ioctl(fd_, UI_SET_EVBIT, EV_SYN);
        ::ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
        ::ioctl(fd_, UI_SET_KEYBIT, BTN_TOUCH);

        for (int axis : {ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
                         ABS_MT_PRESSURE, ABS_MT_TOUCH_MAJOR, ABS_X, ABS_Y}) {
            ::ioctl(fd_, UI_SET_ABSBIT, axis);
        }

        uinput_abs_setup abs{};
        auto setup_axis = [&](int code, int min, int max) {
            abs.code = static_cast<__u16>(code);
            abs.absinfo = input_absinfo{};
            abs.absinfo.minimum = min;
            abs.absinfo.maximum = max;
            ::ioctl(fd_, UI_ABS_SETUP, &abs);
        };
        setup_axis(ABS_MT_SLOT, 0, slots_ - 1);
        setup_axis(ABS_MT_TRACKING_ID, 0, 65535);
        setup_axis(ABS_MT_POSITION_X, 0, width_ - 1);
        setup_axis(ABS_MT_POSITION_Y, 0, height_ - 1);
        setup_axis(ABS_MT_PRESSURE, 0, 255);
        setup_axis(ABS_MT_TOUCH_MAJOR, 0, 255);
        setup_axis(ABS_X, 0, width_ - 1);
        setup_axis(ABS_Y, 0, height_ - 1);

        uinput_setup setup{};
        setup.id.bustype = BUS_VIRTUAL;
        setup.id.vendor = 0x4343;  // "CC"
        setup.id.product = 0x0001;
        setup.id.version = 1;
        std::snprintf(setup.name, sizeof(setup.name), "computer-control virtual touchscreen");
        if (::ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ::ioctl(fd_, UI_DEV_CREATE) < 0) {
            close_device();
            return false;
        }

        // The compositor needs a moment to notice the new device and start
        // routing events from it; without the pause the first gesture is lost.
        std::this_thread::sleep_for(std::chrono::milliseconds{350});
        return true;
    }

    void close_device() {
        if (fd_ >= 0) {
            ::ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool ready() const { return fd_ >= 0; }
    int slots() const { return slots_; }

    bool emit(int type, int code, int value) {
        if (fd_ < 0) return false;
        input_event ev{};
        ev.type = static_cast<__u16>(type);
        ev.code = static_cast<__u16>(code);
        ev.value = value;
        return ::write(fd_, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
    }

    bool sync() { return emit(EV_SYN, SYN_REPORT, 0); }

    // Writes one frame: every active contact's slot, id and position.
    bool frame(const std::vector<std::pair<int, std::pair<int, int>>>& contacts, int pressure,
               bool first, bool last) {
        for (const auto& [slot, pos] : contacts) {
            if (slot >= slots_) continue;
            emit(EV_ABS, ABS_MT_SLOT, slot);
            if (first) {
                emit(EV_ABS, ABS_MT_TRACKING_ID, next_tracking_id_++);
                emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 12);
            }
            if (last) {
                // -1 on the tracking id is how the kernel is told a contact
                // lifted; omitting it leaves a phantom finger down forever.
                emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
                continue;
            }
            emit(EV_ABS, ABS_MT_POSITION_X, std::clamp(pos.first, 0, width_ - 1));
            emit(EV_ABS, ABS_MT_POSITION_Y, std::clamp(pos.second, 0, height_ - 1));
            emit(EV_ABS, ABS_MT_PRESSURE, std::clamp(pressure, 1, 255));
        }
        if (first) emit(EV_KEY, BTN_TOUCH, 1);
        if (last) emit(EV_KEY, BTN_TOUCH, 0);
        // Single-touch emulation for clients that do not read MT events.
        if (!contacts.empty() && !last) {
            emit(EV_ABS, ABS_X, std::clamp(contacts[0].second.first, 0, width_ - 1));
            emit(EV_ABS, ABS_Y, std::clamp(contacts[0].second.second, 0, height_ - 1));
        }
        return sync();
    }

private:
    int fd_ = -1;
    int width_ = 1920, height_ = 1080, slots_ = 10;
    int next_tracking_id_ = 1;
};

#endif  // __linux__

class LinuxInput final : public InputBackend {
public:
    explicit LinuxInput(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }
    ~LinuxInput() override { (void)release_all(); }

    std::string name() const override {
#if defined(CC_HAVE_XTEST)
        return touch_ready_ ? "XTest+uinput" : "XTest";
#else
        return "unavailable";
#endif
    }

    Status initialize() override {
#if !defined(CC_HAVE_X11) || !defined(CC_HAVE_XTEST)
        return err(ErrorCode::Unsupported, "this build has no XTest support",
                   "Install libx11-dev and libxtst-dev and rebuild.");
#else
        dpy_ = x11_display();
        if (!dpy_) {
            return err(ErrorCode::BackendFailure, "cannot open the X display",
                       running_wayland()
                           ? "This is a Wayland session. XTest reaches XWayland clients only, "
                             "and native Wayland clients will not receive synthetic input. Run "
                             "the session under X11, or use the uinput path which works at the "
                             "kernel level for both."
                           : "Set DISPLAY (for example :0) and make sure this process can reach "
                             "the X server.");
        }
        int event_base = 0, error_base = 0, major = 0, minor = 0;
        if (!::XTestQueryExtension(dpy_, &event_base, &error_base, &major, &minor)) {
            return err(ErrorCode::Unsupported, "the X server has no XTEST extension",
                       "XTEST is normally built in. A server started with -tst disables it.");
        }
        if (running_wayland()) {
            // Not fatal, but the user needs to know before they spend an hour
            // wondering why clicks do nothing in a GTK4 app.
            wayland_warning_ =
                "Running under Wayland: XTest input reaches XWayland clients only. Native "
                "Wayland applications will not see it. Grant access to /dev/uinput for "
                "kernel-level input that works everywhere.";
        }
        open_touch();
        return ok();
#endif
    }

    // --- pointer -----------------------------------------------------------

    Result<Point> cursor_position() override {
#if defined(CC_HAVE_X11)
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        ::Window root_ret = 0, child_ret = 0;
        int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
        unsigned mask = 0;
        if (!::XQueryPointer(dpy_, DefaultRootWindow(dpy_), &root_ret, &child_ret, &root_x, &root_y,
                             &win_x, &win_y, &mask)) {
            return err(ErrorCode::BackendFailure, "XQueryPointer failed");
        }
        return displays_->convert(
            Point{static_cast<double>(root_x), static_cast<double>(root_y), Space::Physical},
            Space::Logical);
#else
        return err(ErrorCode::Unsupported, "no X11 support in this build");
#endif
    }

    Status move(const Point& to, const MotionOptions& opts) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        for (const auto& s : motion::line(cur.value(), to, opts)) {
            if (auto st = warp(s.at); !st) return st;
            sleep_us(s.delay);
        }
        return ok();
    }

    Status click(const Point& at, const ClickOptions& opts) override {
        if (opts.move_first) {
            MotionOptions m;
            m.duration = std::chrono::milliseconds{90};
            if (auto st = move(at, m); !st) return st;
        }
        if (opts.count <= 0) return ok();

        ModifierGuard guard(this, opts.modifiers);
        const int n = std::clamp(opts.count, 1, 3);
        for (int i = 0; i < n; ++i) {
            if (auto st = button(opts.button, true); !st) return st;
            sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.press_duration));
            if (auto st = button(opts.button, false); !st) return st;
            if (i + 1 < n) {
                sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.inter_click));
            }
        }
        return ok();
    }

    Status click_here(const ClickOptions& opts) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        ClickOptions o = opts;
        o.move_first = false;
        return click(cur.value(), o);
    }

    Status button_down(MouseButton b, Modifier mods) override {
        ModifierGuard guard(this, mods);
        if (auto st = button(b, true); !st) return st;
        std::lock_guard<std::mutex> lk(mu_);
        held_buttons_.insert(b);
        return ok();
    }

    Status button_up(MouseButton b, Modifier mods) override {
        if (auto st = button(b, false); !st) return st;
        std::lock_guard<std::mutex> lk(mu_);
        held_buttons_.erase(b);
        return ok();
    }

    Status scroll(const Point& at, const ScrollOptions& opts) override {
        MotionOptions m;
        m.duration = std::chrono::milliseconds{60};
        if (auto st = move(at, m); !st) return st;

        // X11 encodes the wheel as buttons 4/5 (vertical) and 6/7
        // (horizontal). There is no pixel-precise scroll in core X.
        int btn = 5;
        switch (opts.direction) {
            case ScrollDirection::Up: btn = 4; break;
            case ScrollDirection::Down: btn = 5; break;
            case ScrollDirection::Left: btn = 6; break;
            case ScrollDirection::Right: btn = 7; break;
        }
        ModifierGuard guard(this, opts.modifiers);
        for (int i = 0; i < std::max(1, opts.clicks); ++i) {
            if (auto st = raw_button(btn, true); !st) return st;
            if (auto st = raw_button(btn, false); !st) return st;
            sleep_us(std::chrono::microseconds{12000});
        }
        return ok();
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

        MotionOptions approach;
        approach.duration = std::chrono::milliseconds{80};
        if (auto st = move(path.front().at, approach); !st) return st;
        sleep_us(std::chrono::microseconds{20000});

        ModifierGuard guard(this, opts.modifiers);
        if (auto st = button(opts.button, true); !st) return st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.insert(opts.button);
        }
        sleep_us(std::chrono::microseconds{30000});

        Status result = ok();
        for (const auto& s : motion::polyline(path, opts.motion)) {
            if (auto st = warp(s.at); !st) {
                result = st;
                break;
            }
            sleep_us(s.delay);
        }

        sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.settle_before_release));
        if (auto st = button(opts.button, false); !st && result.ok()) result = st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.erase(opts.button);
        }
        return result;
    }

    // --- keyboard ----------------------------------------------------------

    Status key_down(Key k) override { return key_event(k, true); }
    Status key_up(Key k) override { return key_event(k, false); }

    Status tap_chord(const Chord& c, int repeat) override {
        ModifierGuard guard(this, c.modifiers);
        for (int i = 0; i < std::max(1, repeat); ++i) {
            for (Key k : c.keys) {
                if (auto st = key_event(k, true); !st) return st;
                sleep_us(std::chrono::microseconds{8000});
                if (auto st = key_event(k, false); !st) return st;
            }
            if (i + 1 < repeat) sleep_us(std::chrono::microseconds{25000});
        }
        return ok();
    }

    Status hold_chord(const Chord& c, std::chrono::milliseconds duration) override {
        ModifierGuard guard(this, c.modifiers);
        std::vector<Key> pressed;
        for (Key k : c.keys) {
            if (auto st = key_event(k, true); !st) {
                for (auto it = pressed.rbegin(); it != pressed.rend(); ++it)
                    (void)key_event(*it, false);
                return st;
            }
            pressed.push_back(k);
        }
        std::this_thread::sleep_for(duration);
        for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) (void)key_event(*it, false);
        return ok();
    }

    Status type_text(std::string_view utf8, const TypeOptions& opts) override {
#if !defined(CC_HAVE_XTEST)
        return err(ErrorCode::Unsupported, "no XTest support");
#else
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");

        // X has no unicode injection. The portable trick is to temporarily
        // rebind a spare keycode to each character's keysym, press it, and
        // restore the mapping. Slower than a native path but layout-proof, and
        // the only way to type a character the user's layout cannot produce.
        const auto per_char =
            (opts.cps > 0)
                ? std::chrono::microseconds{static_cast<long long>(1'000'000.0 / opts.cps)}
                : std::chrono::microseconds{4000};

        const int spare = find_spare_keycode();
        if (spare <= 0) {
            return err(ErrorCode::BackendFailure, "no spare X keycode available for text input",
                       "The keymap is full. Type through the clipboard instead, or reduce the "
                       "number of mapped keycodes.");
        }

        std::size_t i = 0;
        while (i < utf8.size()) {
            char32_t cp = 0;
            int extra = 0;
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
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
            }
            if (i + static_cast<std::size_t>(extra) >= utf8.size()) break;
            for (int k = 1; k <= extra; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
            }
            i += static_cast<std::size_t>(extra) + 1;

            if (cp == '\n') {
                Chord enter;
                enter.keys.push_back(Key::Return);
                if (auto st = tap_chord(enter, 1); !st) return st;
                continue;
            }

            // Latin-1 maps directly; everything else uses the Unicode keysym
            // range, which is 0x01000000 + codepoint.
            KeySym sym =
                (cp < 0x100) ? static_cast<KeySym>(cp) : static_cast<KeySym>(0x01000000 + cp);
            KeySym mapping[2] = {sym, sym};
            ::XChangeKeyboardMapping(dpy_, spare, 2, mapping, 1);
            ::XSync(dpy_, False);

            ::XTestFakeKeyEvent(dpy_, static_cast<unsigned>(spare), True, 0);
            ::XTestFakeKeyEvent(dpy_, static_cast<unsigned>(spare), False, 0);
            ::XFlush(dpy_);
            sleep_us(per_char);
        }

        // Restore the borrowed keycode so the user's keyboard is unchanged.
        KeySym none[2] = {NoSymbol, NoSymbol};
        ::XChangeKeyboardMapping(dpy_, spare, 2, none, 1);
        ::XSync(dpy_, False);

        if (opts.press_enter) {
            Chord enter;
            enter.keys.push_back(Key::Return);
            return tap_chord(enter, 1);
        }
        return ok();
#endif
    }

    // --- gestures ----------------------------------------------------------

    GestureSupport gesture_support(GestureKind kind, int fingers) const override {
        GestureSupport s;
#if defined(__linux__)
        if (touch_ready_) {
            s.fidelity = GestureFidelity::Native;
            s.backend = "uinput-mt";
            s.max_fingers = touch_.slots();
            s.note =
                "A virtual multitouch touchscreen at the kernel level; libinput and the "
                "compositor treat the contacts as real hardware, so this also works on "
                "Wayland.";
            if (fingers > s.max_fingers) {
                s.fidelity = GestureFidelity::Unsupported;
                s.note = "Too many contacts for the virtual device.";
            }
            return s;
        }
#endif
        if (kind == GestureKind::Tap || kind == GestureKind::LongPress) {
            s.fidelity = GestureFidelity::Emulated;
            s.backend = "xtest-click";
            s.max_fingers = 1;
            s.note = uinput_hint_;
            return s;
        }
        if (kind == GestureKind::Pinch || kind == GestureKind::SmartZoom) {
            s.fidelity = GestureFidelity::Emulated;
            s.backend = "ctrl-scroll";
            s.max_fingers = 2;
            s.note = "Falling back to ctrl+scroll, which most Linux applications treat as zoom. " +
                     uinput_hint_;
            return s;
        }
        if (kind == GestureKind::Swipe && fingers <= 2) {
            s.fidelity = GestureFidelity::Emulated;
            s.backend = "xtest-wheel";
            s.max_fingers = 2;
            s.note = uinput_hint_;
            return s;
        }
        s.fidelity = GestureFidelity::Unsupported;
        s.backend = "none";
        s.note = uinput_hint_;
        return s;
    }

    Status gesture(const GestureRequest& req) override {
        const auto support = gesture_support(req.kind, req.fingers);
        if (support.fidelity == GestureFidelity::Unsupported) {
            return err(ErrorCode::Unsupported,
                       "this gesture needs real touch input, which is unavailable", support.note);
        }
        if (support.fidelity == GestureFidelity::Emulated) {
            if (req.require_native) {
                return err(ErrorCode::Unsupported,
                           "require_native was set but only emulation is available", support.note);
            }
            return emulate(req);
        }
#if defined(__linux__)
        return inject(req);
#else
        return err(ErrorCode::Unsupported, "touch injection is Linux-only");
#endif
    }

    Status release_all() override {
        std::set<MouseButton> buttons;
        std::set<Key> keys;
        {
            std::lock_guard<std::mutex> lk(mu_);
            buttons = held_buttons_;
            keys = held_keys_;
        }
        for (MouseButton b : buttons) (void)button(b, false);
        for (Key k : keys) (void)key_event(k, false);
#if defined(__linux__)
        if (touch_ready_ && contacts_down_ > 0) {
            std::vector<std::pair<int, std::pair<int, int>>> lift;
            for (int i = 0; i < contacts_down_; ++i) lift.push_back({i, {0, 0}});
            touch_.frame(lift, 0, false, true);
            contacts_down_ = 0;
        }
#endif
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.clear();
            held_keys_.clear();
        }
        return ok();
    }

private:
    void open_touch() {
#if defined(__linux__)
        const Rect vb = displays_->virtual_bounds(Space::Physical);
        touch_ready_ = touch_.open_device(static_cast<int>(vb.w), static_cast<int>(vb.h), 10);
        if (!touch_ready_) {
            uinput_hint_ =
                "Multi-touch gestures need write access to /dev/uinput. Either run as root, or "
                "grant it once: `sudo groupadd -f uinput && sudo usermod -aG uinput $USER` plus "
                "a udev rule "
                "`KERNEL==\"uinput\", GROUP=\"uinput\", MODE=\"0660\", "
                "OPTIONS+=\"static_node=uinput\"` "
                "in /etc/udev/rules.d/99-uinput.rules, then log out and back in. Also make sure "
                "the uinput module is loaded (`sudo modprobe uinput`).";
        }
#else
        uinput_hint_ = "uinput is Linux-only.";
#endif
    }

    Status warp(const Point& to) {
#if defined(CC_HAVE_XTEST)
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const Point phys = displays_->convert(to, Space::Physical);
        ::XTestFakeMotionEvent(dpy_, -1, static_cast<int>(std::lround(phys.x)),
                               static_cast<int>(std::lround(phys.y)), 0);
        ::XFlush(dpy_);
        return ok();
#else
        return err(ErrorCode::Unsupported, "no XTest support");
#endif
    }

    Status raw_button(int x_button, bool down) {
#if defined(CC_HAVE_XTEST)
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        ::XTestFakeButtonEvent(dpy_, static_cast<unsigned>(x_button), down ? True : False, 0);
        ::XFlush(dpy_);
        return ok();
#else
        return err(ErrorCode::Unsupported, "no XTest support");
#endif
    }

    Status button(MouseButton b, bool down) {
        int x_button = 1;
        switch (b) {
            case MouseButton::Left: x_button = 1; break;
            case MouseButton::Middle: x_button = 2; break;
            case MouseButton::Right: x_button = 3; break;
            case MouseButton::Back: x_button = 8; break;
            case MouseButton::Forward: x_button = 9; break;
        }
        return raw_button(x_button, down);
    }

    Status key_event(Key k, bool down) {
#if defined(CC_HAVE_XTEST)
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const KeySym sym = to_keysym(k);
        if (sym == NoSymbol) {
            return err(ErrorCode::Unsupported, "no X keysym for '" + to_string(k) + "'");
        }
        const KeyCode code = ::XKeysymToKeycode(dpy_, sym);
        if (code == 0) {
            return err(ErrorCode::Unsupported,
                       "'" + to_string(k) + "' is not on the current keyboard layout",
                       "Switch to a layout that has the key, or use type() which rebinds a "
                       "spare keycode.");
        }
        ::XTestFakeKeyEvent(dpy_, code, down ? True : False, 0);
        ::XFlush(dpy_);

        std::lock_guard<std::mutex> lk(mu_);
        if (down)
            held_keys_.insert(k);
        else
            held_keys_.erase(k);
        return ok();
#else
        return err(ErrorCode::Unsupported, "no XTest support");
#endif
    }

    int find_spare_keycode() {
#if defined(CC_HAVE_X11)
        if (!dpy_) return -1;
        int min_code = 0, max_code = 0;
        ::XDisplayKeycodes(dpy_, &min_code, &max_code);
        int per_code = 0;
        KeySym* map = ::XGetKeyboardMapping(dpy_, static_cast<KeyCode>(min_code),
                                            max_code - min_code + 1, &per_code);
        if (!map) return -1;
        int spare = -1;
        // Search from the top: high keycodes are far less likely to be bound
        // than low ones, so borrowing one is less disruptive if we crash
        // before restoring it.
        for (int code = max_code; code >= min_code && spare < 0; --code) {
            bool empty = true;
            for (int j = 0; j < per_code; ++j) {
                if (map[(code - min_code) * per_code + j] != NoSymbol) {
                    empty = false;
                    break;
                }
            }
            if (empty) spare = code;
        }
        ::XFree(map);
        return spare;
#else
        return -1;
#endif
    }

    class ModifierGuard {
    public:
        ModifierGuard(LinuxInput* self, Modifier m) : self_(self) {
            if (has(m, Modifier::Control)) add(Key::LeftControl);
            if (has(m, Modifier::Shift)) add(Key::LeftShift);
            if (has(m, Modifier::Alt)) add(Key::LeftAlt);
            if (has(m, Modifier::Meta)) add(Key::LeftMeta);
        }
        ~ModifierGuard() {
            for (auto it = keys_.rbegin(); it != keys_.rend(); ++it) {
                (void)self_->key_event(*it, false);
            }
        }
        ModifierGuard(const ModifierGuard&) = delete;
        ModifierGuard& operator=(const ModifierGuard&) = delete;

    private:
        void add(Key k) {
            if (self_->key_event(k, true).ok()) keys_.push_back(k);
        }
        LinuxInput* self_;
        std::vector<Key> keys_;
    };

#if defined(__linux__)
    Status inject(const GestureRequest& req) {
        const int fingers = std::clamp(req.fingers, 1, touch_.slots());
        auto start =
            motion::finger_layout(req.kind, req.center, fingers, req.spread, req.direction);

        auto to_contacts = [&](const std::vector<Point>& pts) {
            std::vector<std::pair<int, std::pair<int, int>>> out;
            out.reserve(pts.size());
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const Point phys = displays_->convert(pts[i], Space::Physical);
                out.push_back({static_cast<int>(i),
                               { static_cast<int>(std::lround(phys.x)),
                                 static_cast<int>(std::lround(phys.y)) }});
            }
            return out;
        };

        const int pressure = static_cast<int>(std::clamp(req.pressure, 0.05, 1.0) * 255);
        if (!touch_.frame(to_contacts(start), pressure, true, false)) {
            return err(ErrorCode::BackendFailure, "writing the initial touch frame failed");
        }
        contacts_down_ = fingers;

        const int steps = (req.kind == GestureKind::Tap) ? 2 : motion::gesture_steps(req, 100);
        const auto per_step = std::chrono::microseconds{
            std::max<long long>(1000, req.duration.count() * 1000 / std::max(1, steps))};

        Status result = ok();
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            if (!touch_.frame(to_contacts(motion::gesture_frame(req, start, t)), pressure, false,
                              false)) {
                result = err(ErrorCode::BackendFailure, "writing a touch frame failed");
                break;
            }
            sleep_us(per_step);
        }

        if (req.hold.count() > 0 && result.ok()) std::this_thread::sleep_for(req.hold);

        // Always lift, even after a failure.
        touch_.frame(to_contacts(motion::gesture_frame(req, start, 1.0)), pressure, false, true);
        contacts_down_ = 0;
        return result;
    }
#endif

    Status emulate(const GestureRequest& req) {
        if (req.kind == GestureKind::Pinch || req.kind == GestureKind::SmartZoom) {
            const double factor = std::max(0.05, req.scale);
            const int detents = std::clamp(
                static_cast<int>(std::lround(std::fabs(std::log(factor)) / std::log(1.10))), 1, 60);
            ScrollOptions so;
            so.direction = (factor >= 1.0) ? ScrollDirection::Up : ScrollDirection::Down;
            so.clicks = detents;
            so.modifiers = req.modifiers | Modifier::Control;
            return scroll(req.center, so);
        }
        if (req.kind == GestureKind::Swipe) {
            ScrollOptions so;
            switch (req.direction) {
                case SwipeDirection::Up: so.direction = ScrollDirection::Up; break;
                case SwipeDirection::Down: so.direction = ScrollDirection::Down; break;
                case SwipeDirection::Left: so.direction = ScrollDirection::Left; break;
                case SwipeDirection::Right: so.direction = ScrollDirection::Right; break;
            }
            so.clicks = std::max(1, static_cast<int>(req.distance / 60));
            so.modifiers = req.modifiers;
            return scroll(req.center, so);
        }
        if (req.kind == GestureKind::LongPress) {
            MotionOptions m;
            if (auto st = move(req.center, m); !st) return st;
            if (auto st = button_down(MouseButton::Left, req.modifiers); !st) return st;
            std::this_thread::sleep_for(req.hold.count() > 0 ? req.hold
                                                             : std::chrono::milliseconds{700});
            return button_up(MouseButton::Left, req.modifiers);
        }
        if (req.kind == GestureKind::Tap) {
            ClickOptions c;
            c.modifiers = req.modifiers;
            return click(req.center, c);
        }
        return err(ErrorCode::Unsupported, "no emulation available for this gesture");
    }

#if defined(CC_HAVE_X11)
    ::Display* dpy_ = nullptr;
#endif
#if defined(__linux__)
    UinputTouch touch_;
#endif
    bool touch_ready_ = false;
    int contacts_down_ = 0;
    std::string uinput_hint_;
    std::string wayland_warning_;
    std::mutex mu_;
    std::set<MouseButton> held_buttons_;
    std::set<Key> held_keys_;
};

}  // namespace

Result<std::unique_ptr<InputBackend>> InputBackend::create(std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<LinuxInput>(std::move(displays));
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
