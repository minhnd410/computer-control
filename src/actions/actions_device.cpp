// SPDX-License-Identifier: MIT
//
// The `device` action: everything mobile, in one tool.
//
// Kept separate from actions.cpp only for size; it is the same dispatcher.

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "actions/actions.hpp"

#include "core/text.hpp"

namespace cc::actions {
namespace {

using json::Value;

ActionResult fail_(ErrorCode c, std::string msg, std::string remedy = {}) {
    ActionResult r;
    r.ok = false;
    r.error = Error{c, std::move(msg), std::move(remedy)};
    return r;
}

ActionResult ok_(std::string text, Value v = Value::object()) {
    ActionResult r;
    r.ok = true;
    r.text = std::move(text);
    r.value = std::move(v);
    return r;
}

Value device_json(const DeviceInfo& d) {
    Value v = Value::object();
    v.set("id", d.id);
    v.set("name", d.name);
    v.set("platform", to_string(d.platform));
    v.set("kind", to_string(d.kind));
    v.set("os_version", d.os_version);
    v.set("booted", d.booted);
    if (d.screen_points.w > 0) {
        Value s = Value::object();
        s.set("width", d.screen_points.w);
        s.set("height", d.screen_points.h);
        s.set("scale", d.device_scale);
        v.set("screen_points", s);
    } else {
        v.set("screen_points", Value());
        v.set("screen_note",
              "Device metrics are unknown, so device-point coordinates cannot be translated "
              "precisely. Use mode=screenshot and work from the image, or file an issue with "
              "the device name.");
    }
    Value t = Value::array();
    for (const auto& s : d.available_transports) t.push_back(s);
    v.set("transports", t);
    return v;
}

DeviceTransport transport_from(const Value& v) {
    const std::string s = v.as_string("auto");
    if (s == "bridge") return DeviceTransport::Bridge;
    if (s == "onscreen") return DeviceTransport::Onscreen;
    return DeviceTransport::Auto;
}

std::size_t device_limit(const Value& args, const char* field, std::size_t fallback,
                         std::size_t ceiling) {
    const long long raw = args.contains(field) ? args[field].as_int(static_cast<long long>(fallback))
                                               : static_cast<long long>(fallback);
    return static_cast<std::size_t>(std::clamp<long long>(raw, 1, static_cast<long long>(ceiling)));
}

}  // namespace

ActionResult act_device(Session& s, const Value& args) {
    auto dm = s.devices();
    if (!dm) return fail_(dm.error().code, dm.error().message, dm.error().remedy);

    const std::string mode = args["mode"].as_string("list");

    if (mode == "list") {
        auto list = dm.value()->list(args["booted_only"].as_bool(false));
        if (!list) return fail_(list.error().code, list.error().message, list.error().remedy);

        Value arr = Value::array();
        std::string text;
        const std::size_t total = list.value().size();
        const std::size_t limit = device_limit(args, "limit", 50, 500);
        for (std::size_t i = 0; i < std::min(total, limit); ++i) {
            const auto& d = list.value()[i];
            arr.push_back(device_json(d));
            char buf[160];
            text += text::pad_utf8(d.name, 26);
            std::snprintf(buf, sizeof(buf), " %-9s %-8s %s  [%s]\n", to_string(d.platform),
                          d.booted ? "booted" : "shutdown", d.os_version.c_str(), d.id.c_str());
            text += buf;
        }
        Value out = Value::object();
        out.set("devices", arr);
        out.set("returned", static_cast<long long>(arr.size()));
        out.set("total", static_cast<long long>(total));
        if (total > limit) out.set("truncated", true);
        Value tooling = Value::array();
        for (const auto& t : dm.value()->available_tooling()) tooling.push_back(t);
        out.set("tooling", tooling);
        if (list.value().empty()) {
            text = "No devices found. Tooling on PATH: ";
            for (const auto& t : dm.value()->available_tooling()) text += t + " ";
            if (dm.value()->available_tooling().empty()) {
                text +=
                    "(none). Install Xcode for iOS simulators (xcrun simctl) or Android "
                    "platform-tools for adb.";
            }
        }
        return ok_(text, out);
    }

    if (mode == "boot" || mode == "shutdown") {
        const std::string id = args["device"].as_string();
        if (id.empty()) return fail_(ErrorCode::InvalidArgument, "provide device");
        auto st = (mode == "boot") ? dm.value()->boot(id) : dm.value()->shutdown(id);
        if (!st) return fail_(st.error().code, st.error().message, st.error().remedy);
        return ok_("Device " + mode + " requested.");
    }

    const std::string id = args["device"].as_string();
    if (id.empty()) {
        return fail_(ErrorCode::InvalidArgument, "provide device (UDID, adb serial, or a name)",
                     "Call device(mode=\"list\") to see what is attached.");
    }

    auto opened = dm.value()->open(id, transport_from(args["transport"]));
    if (!opened) return fail_(opened.error().code, opened.error().message, opened.error().remedy);
    auto dev = opened.value();

    auto device_point = [&](const Value& v, const char* field) -> Result<Point> {
        return parse_point(v, field);
    };

    if (mode == "info") {
        Value v = device_json(dev->info());
        (void)dev->refresh();
        if (dev->viewport().valid()) {
            Value vp = Value::object();
            vp.set("host_rect", Value::object());
            const Rect& hr = dev->viewport().host_rect();
            Value r = Value::object();
            r.set("x", hr.x);
            r.set("y", hr.y);
            r.set("w", hr.w);
            r.set("h", hr.h);
            vp.set("host_rect", r);
            vp.set("host_px_per_device_pt", dev->viewport().effective_scale());
            if (dev->viewport().effective_scale() < 0.75) {
                vp.set("warning",
                       "The device window is displayed well below its logical size; small "
                       "targets may be missed. Use Simulator > Window > Physical Size, or "
                       "transport=\"bridge\".");
            }
            v.set("viewport", vp);
        }
        v.set("active_transport",
              dev->active_transport() == DeviceTransport::Bridge ? "bridge" : "onscreen");
        return ok_(dev->info().name, v);
    }

    if (mode == "tap") {
        auto p = device_point(args["at"], "at");
        if (!p) return fail_(p.error().code, p.error().message, p.error().remedy);
        DeviceTapOptions o;
        o.count = static_cast<int>(args["count"].as_int(1));
        o.hold = std::chrono::milliseconds{args["hold_ms"].as_int(0)};
        if (auto st = dev->tap(p.value(), o); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        Value v = Value::object();
        v.set("transport",
              dev->active_transport() == DeviceTransport::Bridge ? "bridge" : "onscreen");
        return ok_("Tapped device.", v);
    }

    if (mode == "swipe") {
        auto a = device_point(args["from"], "from");
        if (!a) return fail_(a.error().code, a.error().message, a.error().remedy);
        auto b = device_point(args["to"], "to");
        if (!b) return fail_(b.error().code, b.error().message, b.error().remedy);
        DeviceSwipeOptions o;
        o.duration = std::chrono::milliseconds{args["duration_ms"].as_int(300)};
        o.press_delay = std::chrono::milliseconds{args["hold_ms"].as_int(0)};
        if (auto st = dev->swipe(a.value(), b.value(), o); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Swiped.");
    }

    if (mode == "stroke") {
        std::vector<PathPoint> path;
        for (const auto& p : args["points"].as_array()) {
            auto pt = parse_point(p, "points[]");
            if (!pt) return fail_(pt.error().code, pt.error().message, pt.error().remedy);
            PathPoint pp;
            pp.at = pt.value();
            if (p.is_object() && p.contains("pressure")) pp.pressure = p["pressure"].as_double(1.0);
            path.push_back(pp);
        }
        if (path.size() < 2) return fail_(ErrorCode::InvalidArgument, "need at least two points");
        StrokeOptions so;
        so.motion.duration = std::chrono::milliseconds{args["duration_ms"].as_int(500)};
        so.smooth = args["smooth"].as_bool(false);
        if (auto st = dev->stroke(path, so); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Drew stroke on device.");
    }

    if (mode == "gesture") {
        GestureRequest g;
        const std::string kind = args["kind"].as_string("pinch");
        if (kind == "pinch" || kind == "zoom")
            g.kind = GestureKind::Pinch;
        else if (kind == "rotate")
            g.kind = GestureKind::Rotate;
        else if (kind == "swipe")
            g.kind = GestureKind::Swipe;
        else if (kind == "pan")
            g.kind = GestureKind::Pan;
        else if (kind == "long_press")
            g.kind = GestureKind::LongPress;
        else if (kind == "tap")
            g.kind = GestureKind::Tap;
        else
            return fail_(ErrorCode::InvalidArgument, "unsupported device gesture '" + kind + "'");

        if (args.contains("at")) {
            auto p = device_point(args["at"], "at");
            if (!p) return fail_(p.error().code, p.error().message, p.error().remedy);
            g.center = p.value();
        } else if (dev->info().screen_points.w > 0) {
            g.center = Point{dev->info().screen_points.w / 2, dev->info().screen_points.h / 2,
                             Space::Logical};
        }
        g.fingers = static_cast<int>(args["fingers"].as_int(2));
        g.scale = args["scale"].as_double(2.0);
        g.rotation_degrees = args["degrees"].as_double(0);
        g.distance = args["distance"].as_double(200);
        g.spread = args["spread"].as_double(120);
        g.duration = std::chrono::milliseconds{args["duration_ms"].as_int(400)};
        g.hold = std::chrono::milliseconds{args["hold_ms"].as_int(0)};
        const std::string dir = args["direction"].as_string("up");
        if (dir == "down")
            g.direction = SwipeDirection::Down;
        else if (dir == "left")
            g.direction = SwipeDirection::Left;
        else if (dir == "right")
            g.direction = SwipeDirection::Right;
        else
            g.direction = SwipeDirection::Up;

        if (auto st = dev->gesture(g); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Performed " + kind + " on device.");
    }

    if (mode == "type") {
        if (auto st = dev->type_text(args["text"].as_string()); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Typed on device.");
    }

    if (mode == "button") {
        if (auto st = dev->press_button(args["button"].as_string()); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Pressed " + args["button"].as_string() + ".");
    }

    if (mode == "screenshot") {
        auto f = dev->screenshot();
        if (!f) return fail_(f.error().code, f.error().message, f.error().remedy);

        EncodeOptions eo;
        const std::string fmt = args["format"].as_string("png");
        if (fmt == "jpeg" || fmt == "jpg") eo.format = ImageFormat::JPEG;
        eo.quality = static_cast<int>(args["quality"].as_int(80));

        Frame frame = f.value();
        const auto max_dim = static_cast<int>(args["max_dimension"].as_int(1400));
        if (max_dim > 0 && std::max(frame.width, frame.height) > max_dim) {
            const double factor =
                static_cast<double>(max_dim) / std::max(frame.width, frame.height);
            auto r = resize(frame, std::max(1, static_cast<int>(frame.width * factor)),
                            std::max(1, static_cast<int>(frame.height * factor)));
            if (r) frame = std::move(r.value());
        }

        auto bytes = encode(frame, eo);
        if (!bytes) return fail_(bytes.error().code, bytes.error().message, bytes.error().remedy);

        ActionResult r;
        r.ok = true;
        r.image = bytes.value();
        r.image_mime = (eo.format == ImageFormat::JPEG) ? "image/jpeg" : "image/png";
        Value v = Value::object();
        v.set("width", frame.width);
        v.set("height", frame.height);
        const auto& dev_size = dev->info().screen_points;
        if (dev_size.w > 0) {
            v.set("device_points_per_image_px", dev_size.w / std::max(1, frame.width));
            v.set("coordinate_note",
                  "This image is the device screen. Device-point coordinates for tap/swipe are "
                  "image_px * " +
                      std::to_string(dev_size.w / std::max(1, frame.width)) + ".");
        }
        r.value = v;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Device screenshot %dx%d.", frame.width, frame.height);
        r.text = buf;
        return r;
    }

    if (mode == "shell") {
        auto out = dev->shell(args["command"].as_string());
        if (!out) return fail_(out.error().code, out.error().message, out.error().remedy);
        const std::size_t limit = device_limit(args, "max_output_bytes", 12000, 262144);
        const bool truncated = out.value().size() > limit;
        Value v = Value::object();
        v.set("output", text::truncate_utf8(out.value(), limit));
        v.set("output_bytes", static_cast<long long>(out.value().size()));
        if (truncated) v.set("output_truncated", true);
        return ok_("Device shell output returned.", v);
    }

    if (mode == "install") {
        if (auto st = dev->install_app(args["path"].as_string()); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Installed.");
    }
    if (mode == "launch") {
        if (auto st = dev->launch_app(args["bundle"].as_string()); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Launched " + args["bundle"].as_string() + ".");
    }
    if (mode == "terminate") {
        if (auto st = dev->terminate_app(args["bundle"].as_string()); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Terminated " + args["bundle"].as_string() + ".");
    }
    if (mode == "open_url") {
        if (auto st = dev->open_url(args["url"].as_string()); !st) {
            return fail_(st.error().code, st.error().message, st.error().remedy);
        }
        return ok_("Opened URL on device.");
    }

    if (mode == "tree") {
        TreeOptions to;
        to.max_nodes = static_cast<int>(args["max_nodes"].as_int(1500));
        auto tree = dev->ui_tree(to);
        if (!tree) return fail_(tree.error().code, tree.error().message, tree.error().remedy);

        Value arr = Value::array();
        std::string text;
        for (const Node* n : tree.value().interactive) {
            Value v = Value::object();
            v.set("label", n->label);
            v.set("role", to_string(n->role));
            v.set("name", n->name);
            if (!n->automation_id.empty()) v.set("id", n->automation_id);
            Value b = Value::object();
            b.set("x", n->bounds.x);
            b.set("y", n->bounds.y);
            b.set("w", n->bounds.w);
            b.set("h", n->bounds.h);
            v.set("bounds", b);
            Value c = Value::object();
            c.set("x", n->bounds.center().x);
            c.set("y", n->bounds.center().y);
            v.set("center", c);
            arr.push_back(v);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%3d %-12s ", n->label, to_string(n->role));
            text += buf;
            text += text::pad_utf8(n->name, 30);
            std::snprintf(buf, sizeof(buf), " (%.0f,%.0f)\n", n->bounds.center().x,
                          n->bounds.center().y);
            text += buf;
        }
        Value out = Value::object();
        out.set("elements", arr);
        out.set("element_count", static_cast<long long>(arr.size()));
        out.set("returned", static_cast<long long>(arr.size()));
        out.set("nodes_walked", tree.value().node_count);
        if (tree.value().truncated) {
            out.set("truncated", true);
            out.set("truncation_reason", tree.value().truncation_reason);
        }
        out.set("note",
                "Coordinates are in device pixels/points; pass them straight to "
                "mode=\"tap\".");
        return ok_(text, out);
    }

    return fail_(ErrorCode::InvalidArgument, "unknown device mode '" + mode + "'");
}

}  // namespace cc::actions
