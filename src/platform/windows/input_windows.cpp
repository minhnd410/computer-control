// SPDX-License-Identifier: MIT
//
// Windows input, on SendInput plus InjectTouchInput.
//
// Windows is the best of the three platforms for this library: SendInput
// covers mouse and keyboard, and InjectTouchInput (Windows 8+) synthesizes
// *real* touch contacts, so pinch, rotate and n-finger swipes are genuine
// multi-touch that the compositor and the target app both see. No emulation.
//
// Two traps worth stating:
//
//  * Absolute mouse coordinates are normalised to 0..65535 across the virtual
//    desktop, not pixels, and the mapping must use SM_XVIRTUALSCREEN /
//    SM_CXVIRTUALSCREEN. Using the primary display's size puts every click on
//    a multi-monitor setup in the wrong place.
//
//  * UIPI blocks synthetic input to any window running at a higher integrity
//    level. Clicks into an elevated app from a non-elevated process are
//    silently discarded, which looks exactly like a coordinate bug. The error
//    path below names this explicitly.
// <windows.h> must come before every other Windows SDK header: psapi.h and
// friends use BOOL, DWORD and WINAPI without declaring them. The blank lines
// keep clang-format from sorting these groups into one another.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "cc/input.hpp"
#include "core/motion.hpp"

namespace cc {
namespace {

constexpr int kMaxContacts = 10;

WORD to_vk(Key k) {
    switch (k) {
        case Key::A: return 'A';
        case Key::B: return 'B';
        case Key::C: return 'C';
        case Key::D: return 'D';
        case Key::E: return 'E';
        case Key::F: return 'F';
        case Key::G: return 'G';
        case Key::H: return 'H';
        case Key::I: return 'I';
        case Key::J: return 'J';
        case Key::K: return 'K';
        case Key::L: return 'L';
        case Key::M: return 'M';
        case Key::N: return 'N';
        case Key::O: return 'O';
        case Key::P: return 'P';
        case Key::Q: return 'Q';
        case Key::R: return 'R';
        case Key::S: return 'S';
        case Key::T: return 'T';
        case Key::U: return 'U';
        case Key::V: return 'V';
        case Key::W: return 'W';
        case Key::X: return 'X';
        case Key::Y: return 'Y';
        case Key::Z: return 'Z';

        case Key::Digit0: return '0';
        case Key::Digit1: return '1';
        case Key::Digit2: return '2';
        case Key::Digit3: return '3';
        case Key::Digit4: return '4';
        case Key::Digit5: return '5';
        case Key::Digit6: return '6';
        case Key::Digit7: return '7';
        case Key::Digit8: return '8';
        case Key::Digit9: return '9';

        case Key::Return: return VK_RETURN;
        case Key::Escape: return VK_ESCAPE;
        case Key::Backspace: return VK_BACK;
        case Key::Tab: return VK_TAB;
        case Key::Space: return VK_SPACE;
        case Key::Minus: return VK_OEM_MINUS;
        case Key::Equal: return VK_OEM_PLUS;
        case Key::LeftBracket: return VK_OEM_4;
        case Key::RightBracket: return VK_OEM_6;
        case Key::Backslash: return VK_OEM_5;
        case Key::Semicolon: return VK_OEM_1;
        case Key::Quote: return VK_OEM_7;
        case Key::Grave: return VK_OEM_3;
        case Key::Comma: return VK_OEM_COMMA;
        case Key::Period: return VK_OEM_PERIOD;
        case Key::Slash: return VK_OEM_2;
        case Key::CapsLock: return VK_CAPITAL;

        case Key::F1: return VK_F1;
        case Key::F2: return VK_F2;
        case Key::F3: return VK_F3;
        case Key::F4: return VK_F4;
        case Key::F5: return VK_F5;
        case Key::F6: return VK_F6;
        case Key::F7: return VK_F7;
        case Key::F8: return VK_F8;
        case Key::F9: return VK_F9;
        case Key::F10: return VK_F10;
        case Key::F11: return VK_F11;
        case Key::F12: return VK_F12;
        case Key::F13: return VK_F13;
        case Key::F14: return VK_F14;
        case Key::F15: return VK_F15;
        case Key::F16: return VK_F16;
        case Key::F17: return VK_F17;
        case Key::F18: return VK_F18;
        case Key::F19: return VK_F19;
        case Key::F20: return VK_F20;
        case Key::F21: return VK_F21;
        case Key::F22: return VK_F22;
        case Key::F23: return VK_F23;
        case Key::F24: return VK_F24;

        case Key::PrintScreen: return VK_SNAPSHOT;
        case Key::ScrollLock: return VK_SCROLL;
        case Key::Pause: return VK_PAUSE;
        case Key::Insert: return VK_INSERT;
        case Key::Home: return VK_HOME;
        case Key::PageUp: return VK_PRIOR;
        case Key::Delete: return VK_DELETE;
        case Key::End: return VK_END;
        case Key::PageDown: return VK_NEXT;
        case Key::Right: return VK_RIGHT;
        case Key::Left: return VK_LEFT;
        case Key::Down: return VK_DOWN;
        case Key::Up: return VK_UP;

        case Key::NumLock: return VK_NUMLOCK;
        case Key::KeypadDivide: return VK_DIVIDE;
        case Key::KeypadMultiply: return VK_MULTIPLY;
        case Key::KeypadMinus: return VK_SUBTRACT;
        case Key::KeypadPlus: return VK_ADD;
        case Key::KeypadEnter: return VK_RETURN;
        case Key::KeypadPeriod: return VK_DECIMAL;
        case Key::Keypad0: return VK_NUMPAD0;
        case Key::Keypad1: return VK_NUMPAD1;
        case Key::Keypad2: return VK_NUMPAD2;
        case Key::Keypad3: return VK_NUMPAD3;
        case Key::Keypad4: return VK_NUMPAD4;
        case Key::Keypad5: return VK_NUMPAD5;
        case Key::Keypad6: return VK_NUMPAD6;
        case Key::Keypad7: return VK_NUMPAD7;
        case Key::Keypad8: return VK_NUMPAD8;
        case Key::Keypad9: return VK_NUMPAD9;

        case Key::LeftControl: return VK_LCONTROL;
        case Key::RightControl: return VK_RCONTROL;
        case Key::LeftShift: return VK_LSHIFT;
        case Key::RightShift: return VK_RSHIFT;
        case Key::LeftAlt: return VK_LMENU;
        case Key::RightAlt: return VK_RMENU;
        case Key::LeftMeta: return VK_LWIN;
        case Key::RightMeta: return VK_RWIN;

        case Key::VolumeUp: return VK_VOLUME_UP;
        case Key::VolumeDown: return VK_VOLUME_DOWN;
        case Key::Mute: return VK_VOLUME_MUTE;
        case Key::PlayPause: return VK_MEDIA_PLAY_PAUSE;
        case Key::NextTrack: return VK_MEDIA_NEXT_TRACK;
        case Key::PreviousTrack: return VK_MEDIA_PREV_TRACK;
        case Key::Stop: return VK_MEDIA_STOP;
        case Key::Menu: return VK_APPS;
        case Key::Help: return VK_HELP;
        default: return 0;
    }
}

// Keys on the extended set need KEYEVENTF_EXTENDEDKEY or they are delivered as
// their numpad twins: without it, Right arrow types '6'.
bool is_extended(WORD vk) {
    switch (vk) {
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS: return true;
        default: return false;
    }
}

void sleep_us(std::chrono::microseconds us) {
    if (us.count() > 0) std::this_thread::sleep_for(us);
}

class WinInput final : public InputBackend {
public:
    explicit WinInput(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }
    ~WinInput() override { (void)release_all(); }

    std::string name() const override {
        return touch_ready_ ? "SendInput+InjectTouchInput" : "SendInput";
    }

    Status initialize() override {
        // Touch injection is optional: it fails on Windows 7 and inside some
        // session-0 services. Mouse and keyboard still work, and
        // gesture_support() reports the downgrade rather than failing later.
        touch_ready_ = ::InitializeTouchInjection(kMaxContacts, TOUCH_FEEDBACK_NONE) != FALSE;
        return ok();
    }

    // --- pointer -----------------------------------------------------------

    Result<Point> cursor_position() override {
        POINT p{};
        if (!::GetCursorPos(&p)) {
            return err(ErrorCode::BackendFailure, "GetCursorPos failed",
                       "This usually means the process is running in session 0 or on a locked "
                       "desktop, where there is no cursor to report.");
        }
        // With per-monitor DPI awareness these are physical pixels.
        return displays_->convert(
            Point{static_cast<double>(p.x), static_cast<double>(p.y), Space::Physical},
            Space::Logical);
    }

    Status move(const Point& to, const MotionOptions& opts) override {
        auto cur = cursor_position();
        if (!cur) return cur.error();
        const auto samples = motion::line(cur.value(), to, opts);
        for (const auto& s : samples) {
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

        const int n = std::clamp(opts.count, 1, 3);
        for (int i = 0; i < n; ++i) {
            if (auto st = press(opts.button, true, opts.modifiers); !st) return st;
            sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.press_duration));
            if (auto st = press(opts.button, false, opts.modifiers); !st) return st;
            if (i + 1 < n) {
                // Must stay under GetDoubleClickTime() or the OS sees separate
                // clicks; the user can raise that value, so respect it.
                const auto limit =
                    std::chrono::milliseconds{std::max<DWORD>(60, ::GetDoubleClickTime() / 3)};
                sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::min(opts.inter_click, limit)));
            }
        }
        return ok();
    }

    Status click_here(const ClickOptions& opts) override {
        ClickOptions o = opts;
        o.move_first = false;
        auto cur = cursor_position();
        if (!cur) return cur.error();
        return click(cur.value(), o);
    }

    Status button_down(MouseButton b, Modifier mods) override {
        if (auto st = press(b, true, mods); !st) return st;
        std::lock_guard<std::mutex> lk(mu_);
        held_buttons_.insert(b);
        return ok();
    }

    Status button_up(MouseButton b, Modifier mods) override {
        if (auto st = press(b, false, mods); !st) return st;
        std::lock_guard<std::mutex> lk(mu_);
        held_buttons_.erase(b);
        return ok();
    }

    Status scroll(const Point& at, const ScrollOptions& opts) override {
        MotionOptions m;
        m.duration = std::chrono::milliseconds{60};
        if (auto st = move(at, m); !st) return st;

        const int per = opts.pixel_units ? opts.pixels_per_click : WHEEL_DELTA;
        int amount = per;
        DWORD flag = MOUSEEVENTF_WHEEL;
        switch (opts.direction) {
            case ScrollDirection::Up: amount = per; break;
            case ScrollDirection::Down: amount = -per; break;
            case ScrollDirection::Left:
                amount = -per;
                flag = MOUSEEVENTF_HWHEEL;
                break;
            case ScrollDirection::Right:
                amount = per;
                flag = MOUSEEVENTF_HWHEEL;
                break;
        }

        auto mods = scoped_modifiers(opts.modifiers);
        for (int i = 0; i < std::max(1, opts.clicks); ++i) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = flag;
            in.mi.mouseData = static_cast<DWORD>(amount);
            if (::SendInput(1, &in, sizeof(in)) != 1) return send_failed();
            sleep_us(std::chrono::microseconds{10000});
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

        if (auto st = press(opts.button, true, opts.modifiers); !st) return st;
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.insert(opts.button);
        }
        // Explorer and most WPF/WinUI drag sources arm only after the pointer
        // has moved past the system drag threshold while held, so the press
        // needs a moment to register before motion starts.
        sleep_us(std::chrono::microseconds{40000});

        Status result = ok();
        for (const auto& s : motion::polyline(path, opts.motion)) {
            if (auto st = warp(s.at); !st) {
                result = st;
                break;
            }
            sleep_us(s.delay);
        }

        sleep_us(std::chrono::duration_cast<std::chrono::microseconds>(opts.settle_before_release));
        if (auto st = press(opts.button, false, opts.modifiers); !st && result.ok()) result = st;
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
        auto mods = scoped_modifiers(c.modifiers);
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
        auto mods = scoped_modifiers(c.modifiers);
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
        // KEYEVENTF_UNICODE bypasses the keyboard layout entirely, so the text
        // arrives the same whether the user is on QWERTY, AZERTY or a Chinese
        // IME. Surrogate pairs are sent as two events, which is what Windows
        // expects for anything outside the BMP.
        std::vector<wchar_t> utf16;
        utf16.reserve(utf8.size());
        {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                                static_cast<int>(utf8.size()), nullptr, 0);
            if (n > 0) {
                utf16.resize(static_cast<std::size_t>(n));
                ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      utf16.data(), n);
            }
        }

        const auto per_char =
            (opts.cps > 0)
                ? std::chrono::microseconds{static_cast<long long>(1'000'000.0 / opts.cps)}
                : std::chrono::microseconds{600};

        std::vector<INPUT> batch;
        batch.reserve(utf16.size() * 2);
        for (wchar_t wc : utf16) {
            if (wc == L'\n') {
                // A literal newline must become Enter; injecting U+000A does
                // nothing in most controls.
                INPUT down{}, up{};
                down.type = INPUT_KEYBOARD;
                down.ki.wVk = VK_RETURN;
                up = down;
                up.ki.dwFlags = KEYEVENTF_KEYUP;
                batch.push_back(down);
                batch.push_back(up);
                continue;
            }
            INPUT down{}, up{};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = static_cast<WORD>(wc);
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            up = down;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            batch.push_back(down);
            batch.push_back(up);
        }

        if (opts.cps <= 0) {
            // One SendInput call for the whole string: far faster and atomic
            // with respect to real user input.
            if (!batch.empty()) {
                const UINT sent =
                    ::SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT));
                if (sent != batch.size()) return send_failed();
            }
        } else {
            for (std::size_t i = 0; i + 1 < batch.size(); i += 2) {
                if (::SendInput(2, &batch[i], sizeof(INPUT)) != 2) return send_failed();
                sleep_us(per_char);
            }
        }

        if (opts.press_enter) {
            Chord c;
            c.keys.push_back(Key::Return);
            return tap_chord(c, 1);
        }
        return ok();
    }

    // --- gestures ----------------------------------------------------------

    GestureSupport gesture_support(GestureKind kind, int fingers) const override {
        GestureSupport s;
        if (!touch_ready_) {
            s.fidelity = (kind == GestureKind::Tap || kind == GestureKind::LongPress)
                             ? GestureFidelity::Emulated
                             : GestureFidelity::Unsupported;
            s.backend = "none";
            s.max_fingers = 0;
            s.note =
                "InitializeTouchInjection failed. This happens on Windows 7, in session 0, "
                "and in some remote-desktop configurations. Mouse and keyboard still work.";
            if (kind == GestureKind::Pinch) {
                s.fidelity = GestureFidelity::Emulated;
                s.backend = "ctrl-scroll";
                s.note = "Falling back to ctrl+scroll, which most applications treat as zoom.";
            }
            return s;
        }
        s.fidelity = GestureFidelity::Native;
        s.backend = "InjectTouchInput";
        s.max_fingers = kMaxContacts;
        s.note =
            "Real synthesized touch contacts; the compositor and the target application "
            "both see genuine multi-touch.";
        if (fingers > kMaxContacts) {
            s.fidelity = GestureFidelity::Unsupported;
            s.note = "At most 10 simultaneous contacts.";
        }
        return s;
    }

    Status gesture(const GestureRequest& req) override {
        const auto support = gesture_support(req.kind, req.fingers);
        if (support.fidelity == GestureFidelity::Unsupported) {
            return err(ErrorCode::Unsupported, "gesture unavailable: " + support.note);
        }
        if (support.fidelity == GestureFidelity::Emulated) {
            if (req.require_native) {
                return err(
                    ErrorCode::Unsupported,
                    "require_native was set but touch injection is unavailable: " + support.note);
            }
            return emulate(req);
        }
        return inject(req);
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
        for (Key k : keys) (void)key_event(k, false);
        if (touch_ready_ && contacts_down_) {
            // Lift any contact still down, or the desktop stays in a
            // half-gesture state until the next real touch.
            std::vector<POINTER_TOUCH_INFO> lift(static_cast<std::size_t>(contacts_down_));
            for (int i = 0; i < contacts_down_; ++i) {
                lift[static_cast<std::size_t>(i)] = make_contact(i, last_contact_[i], 1.0);
                lift[static_cast<std::size_t>(i)].pointerInfo.pointerFlags = POINTER_FLAG_UP;
            }
            ::InjectTouchInput(static_cast<UINT32>(lift.size()), lift.data());
            contacts_down_ = 0;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_buttons_.clear();
            held_keys_.clear();
        }
        return ok();
    }

private:
    Status send_failed() {
        const DWORD e = ::GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            return err(ErrorCode::PermissionDenied,
                       "SendInput was blocked by User Interface Privilege Isolation",
                       "The target window runs at a higher integrity level than this process. "
                       "Run computer-control elevated to drive elevated windows, or target a "
                       "non-elevated application.");
        }
        return err(ErrorCode::BackendFailure, "SendInput failed (error " + std::to_string(e) + ")");
    }

    Status warp(const Point& to) {
        const Point phys = displays_->convert(to, Space::Physical);

        // Absolute coordinates are 0..65535 across the *virtual* desktop.
        const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = std::max(1, ::GetSystemMetrics(SM_CXVIRTUALSCREEN));
        const int vh = std::max(1, ::GetSystemMetrics(SM_CYVIRTUALSCREEN));

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        // The +0.5 rounds to the nearest pixel; truncation biases every move
        // one pixel up and left, which is visible on small targets.
        in.mi.dx = static_cast<LONG>(((phys.x - vx) * 65535.0) / vw + 0.5);
        in.mi.dy = static_cast<LONG>(((phys.y - vy) * 65535.0) / vh + 0.5);
        if (::SendInput(1, &in, sizeof(in)) != 1) return send_failed();
        return ok();
    }

    Status press(MouseButton b, bool down, Modifier mods) {
        auto guard = scoped_modifiers(mods);
        INPUT in{};
        in.type = INPUT_MOUSE;
        switch (b) {
            case MouseButton::Left:
                in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                break;
            case MouseButton::Right:
                in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                break;
            case MouseButton::Middle:
                in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                break;
            case MouseButton::Back:
            case MouseButton::Forward:
                in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                in.mi.mouseData = (b == MouseButton::Back) ? XBUTTON1 : XBUTTON2;
                break;
        }
        if (::SendInput(1, &in, sizeof(in)) != 1) return send_failed();
        return ok();
    }

    Status key_event(Key k, bool down) {
        const WORD vk = to_vk(k);
        if (vk == 0) {
            return err(ErrorCode::Unsupported,
                       "no Windows virtual-key code for '" + to_string(k) + "'");
        }
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags =
            (down ? 0u : KEYEVENTF_KEYUP) | (is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
        if (::SendInput(1, &in, sizeof(in)) != 1) return send_failed();

        std::lock_guard<std::mutex> lk(mu_);
        if (down)
            held_keys_.insert(k);
        else
            held_keys_.erase(k);
        return ok();
    }

    // Presses the modifier keys and releases them when the guard goes out of
    // scope, including on an early return. Leaving Ctrl stuck down is the
    // worst failure mode this library has.
    class ModifierGuard {
    public:
        ModifierGuard(WinInput* self, Modifier m) : self_(self) {
            if (has(m, Modifier::Control)) add(VK_CONTROL);
            if (has(m, Modifier::Shift)) add(VK_SHIFT);
            if (has(m, Modifier::Alt)) add(VK_MENU);
            if (has(m, Modifier::Meta)) add(VK_LWIN);
        }
        ~ModifierGuard() {
            for (auto it = keys_.rbegin(); it != keys_.rend(); ++it) send(*it, false);
        }
        ModifierGuard(const ModifierGuard&) = delete;
        ModifierGuard& operator=(const ModifierGuard&) = delete;

    private:
        void add(WORD vk) {
            send(vk, true);
            keys_.push_back(vk);
        }
        static void send(WORD vk, bool down) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = vk;
            in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            in.ki.dwFlags =
                (down ? 0u : KEYEVENTF_KEYUP) | (is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
            ::SendInput(1, &in, sizeof(in));
        }
        WinInput* self_;
        std::vector<WORD> keys_;
    };

    ModifierGuard scoped_modifiers(Modifier m) { return ModifierGuard(this, m); }

    POINTER_TOUCH_INFO make_contact(int id, const Point& logical, double pressure) {
        const Point phys = displays_->convert(logical, Space::Physical);
        const LONG x = static_cast<LONG>(phys.x + 0.5);
        const LONG y = static_cast<LONG>(phys.y + 0.5);

        POINTER_TOUCH_INFO c{};
        c.pointerInfo.pointerType = PT_TOUCH;
        c.pointerInfo.pointerId = static_cast<UINT32>(id);
        c.pointerInfo.ptPixelLocation.x = x;
        c.pointerInfo.ptPixelLocation.y = y;
        c.touchFlags = TOUCH_FLAG_NONE;
        c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
        c.orientation = 0;
        // 0..1024; 512 is a normal press. Some apps ignore a zero pressure
        // contact entirely.
        c.pressure = static_cast<UINT32>(std::clamp(pressure, 0.05, 1.0) * 1024);
        // A believable finger-sized contact rectangle. A zero-area contact is
        // rejected by parts of the Windows touch stack.
        c.rcContact.left = x - 4;
        c.rcContact.right = x + 4;
        c.rcContact.top = y - 4;
        c.rcContact.bottom = y + 4;
        return c;
    }

    Status inject_frame(const std::vector<Point>& points, double pressure, UINT32 flags) {
        std::vector<POINTER_TOUCH_INFO> contacts;
        contacts.reserve(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            auto c = make_contact(static_cast<int>(i), points[i], pressure);
            c.pointerInfo.pointerFlags = flags;
            contacts.push_back(c);
            if (i < static_cast<std::size_t>(kMaxContacts)) last_contact_[i] = points[i];
        }
        if (!::InjectTouchInput(static_cast<UINT32>(contacts.size()), contacts.data())) {
            const DWORD e = ::GetLastError();
            return err(ErrorCode::BackendFailure,
                       "InjectTouchInput failed (error " + std::to_string(e) + ")",
                       e == ERROR_ACCESS_DENIED
                           ? "Touch injection is blocked by UIPI for the target window. Run "
                             "elevated, or target a non-elevated application."
                           : "Contacts must stay within the virtual desktop and pointer ids "
                             "must be stable across a gesture.");
        }
        return ok();
    }

    Status inject(const GestureRequest& req) {
        const int fingers = std::clamp(req.fingers, 1, kMaxContacts);
        auto start =
            motion::finger_layout(req.kind, req.center, fingers, req.spread, req.direction);

        constexpr UINT32 kDown = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        constexpr UINT32 kMove =
            POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        constexpr UINT32 kUp = POINTER_FLAG_UP;

        if (auto st = inject_frame(start, req.pressure, kDown); !st) return st;
        contacts_down_ = fingers;

        // Windows drops a gesture whose contacts never update, so even a tap
        // emits one update frame before lifting.
        const int steps = (req.kind == GestureKind::Tap) ? 2 : motion::gesture_steps(req, 100);
        const auto per_step = std::chrono::microseconds{
            std::max<long long>(1000, req.duration.count() * 1000 / std::max(1, steps))};

        Status result = ok();
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            auto frame = motion::gesture_frame(req, start, t);
            if (auto st = inject_frame(frame, req.pressure, kMove); !st) {
                result = st;
                break;
            }
            sleep_us(per_step);
        }

        if (req.hold.count() > 0 && result.ok()) {
            // A hold still has to keep injecting, or the contacts time out.
            const auto until = std::chrono::steady_clock::now() + req.hold;
            auto frame = motion::gesture_frame(req, start, 1.0);
            while (std::chrono::steady_clock::now() < until) {
                if (auto st = inject_frame(frame, req.pressure, kMove); !st) {
                    result = st;
                    break;
                }
                sleep_us(std::chrono::microseconds{20000});
            }
        }

        // Always lift, even if a frame failed.
        auto last = motion::gesture_frame(req, start, 1.0);
        (void)inject_frame(last, req.pressure, kUp);
        contacts_down_ = 0;
        return result;
    }

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
        if (req.kind == GestureKind::Tap || req.kind == GestureKind::LongPress) {
            ClickOptions c;
            c.modifiers = req.modifiers;
            if (req.kind == GestureKind::LongPress) {
                if (auto st = move(req.center, MotionOptions{}); !st) return st;
                if (auto st = button_down(MouseButton::Left, req.modifiers); !st) return st;
                std::this_thread::sleep_for(req.hold.count() > 0 ? req.hold
                                                                 : std::chrono::milliseconds{700});
                return button_up(MouseButton::Left, req.modifiers);
            }
            return click(req.center, c);
        }
        return err(ErrorCode::Unsupported,
                   "this gesture needs touch injection, which is unavailable on this host");
    }

    bool touch_ready_ = false;
    int contacts_down_ = 0;
    Point last_contact_[kMaxContacts]{};
    std::mutex mu_;
    std::set<MouseButton> held_buttons_;
    std::set<Key> held_keys_;
};

}  // namespace

Result<std::unique_ptr<InputBackend>> InputBackend::create(std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<WinInput>(std::move(displays));
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
