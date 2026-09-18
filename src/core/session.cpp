// SPDX-License-Identifier: MIT
#include "cc/session.hpp"

#include <sstream>

#include "core/json.hpp"

namespace cc {

Result<std::shared_ptr<Session>> Session::create(SessionConfig cfg) {
    auto s = std::shared_ptr<Session>(new Session());
    s->cfg_ = cfg;

    auto dg = DisplayGraph::create();
    if (!dg) return dg.error();
    s->displays_ = dg.value();

    if (cfg.eager_init) {
        // Order matters: input's permission check is the one most likely to
        // fail, and failing it first gives the clearest message.
        if (auto r = InputBackend::create(s->displays_); r)
            s->input_ = std::move(r.value());
        else
            return r.error();
        if (auto r = ScreenBackend::create(s->displays_); r)
            s->screen_ = std::move(r.value());
        else
            return r.error();
        if (auto r = WindowBackend::create(s->displays_); r)
            s->windows_ = std::move(r.value());
        else
            return r.error();
        if (auto r = AccessibilityBackend::create(s->displays_); r) s->a11y_ = std::move(r.value());
        if (auto r = SystemBackend::create(); r) s->system_ = std::move(r.value());
    }
    return s;
}

Session::~Session() {
    // A crash or an early return must never leave a mouse button or a modifier
    // key stuck down: the desktop becomes unusable and the user has to press
    // the physical key to clear it.
    if (input_) (void)input_->release_all();
}

Result<DisplayGraph*> Session::displays() {
    if (!displays_) return err(ErrorCode::Internal, "display graph was not created");
    return displays_.get();
}

Result<InputBackend*> Session::input() {
    if (!input_) {
        auto r = InputBackend::create(displays_);
        if (!r) return r.error();
        input_ = std::move(r.value());
    }
    return input_.get();
}

Result<ScreenBackend*> Session::screen() {
    if (!screen_) {
        auto r = ScreenBackend::create(displays_);
        if (!r) return r.error();
        screen_ = std::move(r.value());
    }
    return screen_.get();
}

Result<WindowBackend*> Session::windows() {
    if (!windows_) {
        auto r = WindowBackend::create(displays_);
        if (!r) return r.error();
        windows_ = std::move(r.value());
    }
    return windows_.get();
}

Result<AccessibilityBackend*> Session::accessibility() {
    if (!a11y_) {
        auto r = AccessibilityBackend::create(displays_);
        if (!r) return r.error();
        a11y_ = std::move(r.value());
    }
    return a11y_.get();
}

Result<SystemBackend*> Session::system() {
    if (!system_) {
        auto r = SystemBackend::create();
        if (!r) return r.error();
        system_ = std::move(r.value());
    }
    return system_.get();
}

Result<DeviceManager*> Session::devices() {
    if (!devices_) {
        auto in = input();
        auto sc = screen();
        auto wi = windows();
        auto r = DeviceManager::create(displays_, in ? in.value() : nullptr,
                                       sc ? sc.value() : nullptr, wi ? wi.value() : nullptr);
        if (!r) return r.error();
        devices_ = r.value();
    }
    return devices_.get();
}

Result<Point> Session::resolve(const Point& p) {
    if (!displays_) return err(ErrorCode::Internal, "display graph was not created");

    if (p.space == Space::Image && !displays_->image_transform().valid) {
        return err(ErrorCode::InvalidArgument,
                   "a coordinate was given in image space, but no capture has been taken yet",
                   "Take a screenshot first; image-space coordinates are only meaningful "
                   "relative to a specific capture.");
    }

    const Point logical = displays_->convert(p, Space::Logical);

    // A point outside every display is almost always a unit mix-up (physical
    // pixels passed as logical points is the classic one) and produces a click
    // that silently goes nowhere. Refusing with the actual bounds is far more
    // useful than a no-op.
    const Rect vb = displays_->virtual_bounds(Space::Logical);
    const double slack = 2.0;
    if (logical.x < vb.x - slack || logical.y < vb.y - slack || logical.x > vb.right() + slack ||
        logical.y > vb.bottom() + slack) {
        std::ostringstream os;
        os << "point (" << p.x << ", " << p.y << ") in " << to_string(p.space)
           << " space resolves to (" << logical.x << ", " << logical.y
           << ") which is outside the desktop " << vb.w << "x" << vb.h << " at (" << vb.x << ", "
           << vb.y << ")";
        return err(ErrorCode::InvalidArgument, os.str(),
                   "If the coordinate came from a screenshot, pass space=\"image\". If it came "
                   "from a Retina capture read as raw pixels, pass space=\"physical\".");
    }
    return logical;
}

std::string Session::capability_report() {
    json::Value root = json::Value::object();
    const BuildInfo bi = build_info();
    root.set("version", bi.version);
    root.set("platform", bi.platform);
    root.set("compiler", bi.compiler);
    root.set("abi", 1);

    json::Value disp = json::Value::array();
    if (displays_) {
        for (const auto& d : displays_->displays()) {
            json::Value v = json::Value::object();
            v.set("index", d.index);
            v.set("name", d.name);
            v.set("primary", d.primary);
            v.set("scale", d.scale);
            v.set("dpi", d.dpi);
            json::Value lb = json::Value::object();
            lb.set("x", d.bounds_logical.x);
            lb.set("y", d.bounds_logical.y);
            lb.set("w", d.bounds_logical.w);
            lb.set("h", d.bounds_logical.h);
            v.set("bounds_logical", lb);
            json::Value pb = json::Value::object();
            pb.set("x", d.bounds_physical.x);
            pb.set("y", d.bounds_physical.y);
            pb.set("w", d.bounds_physical.w);
            pb.set("h", d.bounds_physical.h);
            v.set("bounds_physical", pb);
            disp.push_back(v);
        }
    }
    root.set("displays", disp);

    json::Value backends = json::Value::object();
    auto probe = [&](const char* key, auto factory) {
        auto r = factory();
        json::Value v = json::Value::object();
        if (r) {
            v.set("available", true);
            v.set("backend", r.value()->name());
        } else {
            v.set("available", false);
            v.set("error", r.error().message);
            if (!r.error().remedy.empty()) v.set("remedy", r.error().remedy);
        }
        backends.set(key, v);
    };
    probe("input", [&] { return input(); });
    probe("screen", [&] { return screen(); });
    probe("windows", [&] { return windows(); });
    probe("system", [&] { return system(); });

    // Accessibility is constructed unconditionally so that a caller who only
    // wants screenshots is never blocked by a missing grant. That makes
    // construction a useless signal, so report the readiness check instead -
    // otherwise the report says "available" for a backend that cannot answer
    // a single query.
    {
        json::Value v = json::Value::object();
        auto a11y = accessibility();
        if (!a11y) {
            v.set("available", false);
            v.set("error", a11y.error().message);
            if (!a11y.error().remedy.empty()) v.set("remedy", a11y.error().remedy);
        } else if (auto ready = a11y.value()->check_permission(false); !ready) {
            v.set("available", false);
            v.set("backend", a11y.value()->name());
            v.set("error", ready.error().message);
            if (!ready.error().remedy.empty()) v.set("remedy", ready.error().remedy);
        } else {
            v.set("available", true);
            v.set("backend", a11y.value()->name());
        }
        backends.set("accessibility", v);
    }
    root.set("backends", backends);

    // Gesture fidelity is the thing that differs most between platforms, so it
    // is reported per kind rather than as one flag.
    json::Value gestures = json::Value::object();
    if (auto in = input()) {
        struct Entry {
            const char* name;
            GestureKind kind;
            int fingers;
        };
        const Entry entries[] = {
            {"tap", GestureKind::Tap, 1},
            {"two_finger_tap", GestureKind::Tap, 2},
            {"long_press", GestureKind::LongPress, 1},
            {"swipe_2", GestureKind::Swipe, 2},
            {"swipe_3", GestureKind::Swipe, 3},
            {"swipe_4", GestureKind::Swipe, 4},
            {"pan", GestureKind::Pan, 2},
            {"pinch", GestureKind::Pinch, 2},
            {"rotate", GestureKind::Rotate, 2},
            {"smart_zoom", GestureKind::SmartZoom, 2},
            {"force_press", GestureKind::ForcePress, 1},
            {"edge_swipe", GestureKind::EdgeSwipe, 1},
        };
        for (const auto& e : entries) {
            const auto s = in.value()->gesture_support(e.kind, e.fingers);
            json::Value v = json::Value::object();
            v.set("fidelity", s.fidelity == GestureFidelity::Native     ? "native"
                              : s.fidelity == GestureFidelity::Emulated ? "emulated"
                                                                        : "unsupported");
            v.set("backend", s.backend);
            v.set("max_fingers", s.max_fingers);
            if (!s.note.empty()) v.set("note", s.note);
            gestures.set(e.name, v);
        }
    }
    root.set("gestures", gestures);

    if (auto dm = devices()) {
        json::Value tooling = json::Value::array();
        for (const auto& t : dm.value()->available_tooling()) tooling.push_back(t);
        root.set("device_tooling", tooling);
    }

    json::Value limits = json::Value::object();
    limits.set("allow_shell", cfg_.allow_shell);
    limits.set("allow_filesystem", cfg_.allow_filesystem);
    limits.set("allow_registry", cfg_.allow_registry);
    limits.set("allow_clipboard", cfg_.allow_clipboard);
    root.set("policy", limits);

    return root.dump(2);
}

Status Session::release_all() {
    if (!input_) return ok();
    return input_->release_all();
}

BuildInfo build_info() {
    BuildInfo b;
#ifdef CC_VERSION
    b.version = CC_VERSION;
#else
    b.version = "0.0.0";
#endif
#ifdef CC_PLATFORM_NAME
    b.platform = CC_PLATFORM_NAME;
#else
    b.platform = "unknown";
#endif
    b.commit = "";
#if defined(__clang__)
    b.compiler = "clang " __clang_version__;
#elif defined(_MSC_VER)
    b.compiler = "msvc";
#elif defined(__GNUC__)
    b.compiler = "gcc";
#else
    b.compiler = "unknown";
#endif
#if defined(_WIN32) || defined(__linux__)
    b.gestures_native = true;
#else
    b.gestures_native = false;
#endif
    return b;
}

}  // namespace cc
