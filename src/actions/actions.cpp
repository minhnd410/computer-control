// SPDX-License-Identifier: MIT
#include "actions/actions.hpp"

#include "core/text.hpp"

#include "cc/permissions.hpp"
#include "cc/system_ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

namespace cc::actions {
namespace {

using json::Value;

// --- small helpers ---------------------------------------------------------

ActionResult fail(Error e) {
    ActionResult r;
    r.ok = false;
    r.error = std::move(e);
    return r;
}

ActionResult fail(ErrorCode c, std::string msg, std::string remedy = {}) {
    return fail(Error{c, std::move(msg), std::move(remedy)});
}

ActionResult succeed(std::string text, Value v = Value::object()) {
    ActionResult r;
    r.ok = true;
    r.text = std::move(text);
    r.value = std::move(v);
    return r;
}

Value rect_json(const Rect& r) {
    Value v = Value::object();
    v.set("x", r.x);
    v.set("y", r.y);
    v.set("w", r.w);
    v.set("h", r.h);
    v.set("space", to_string(r.space));
    return v;
}

Value point_json(const Point& p) {
    Value v = Value::object();
    v.set("x", p.x);
    v.set("y", p.y);
    v.set("space", to_string(p.space));
    return v;
}

std::chrono::milliseconds ms(const Value& v, long long fallback) {
    return std::chrono::milliseconds{v.is_null() ? fallback : v.as_int(fallback)};
}

}  // namespace

Modifier parse_modifiers(const Value& v) {
    Modifier m = Modifier::None;
    auto apply = [&](const std::string& s) {
        auto c = parse_chord(s);
        if (c) m |= c.value().modifiers;
    };
    if (v.is_string()) {
        // Accept "cmd+shift" as one string.
        auto c = parse_chord(v.as_string() + "+a");
        if (c)
            m |= c.value().modifiers;
        else
            apply(v.as_string());
    } else if (v.is_array()) {
        for (const auto& e : v.as_array()) {
            if (!e.is_string()) continue;
            auto c = parse_chord(e.as_string() + "+a");
            if (c) m |= c.value().modifiers;
        }
    }
    return m;
}

Result<Point> parse_point(const Value& v, const char* field) {
    Space space = Space::Logical;

    if (v.is_array() && v.size() >= 2) {
        return Point{v[0].as_double(), v[1].as_double(), space};
    }
    if (v.is_object()) {
        if (v.contains("space")) {
            auto s = space_from_string(v["space"].as_string());
            if (!s) {
                return err(ErrorCode::InvalidArgument,
                           std::string(field) + ".space must be logical, physical or image");
            }
            space = *s;
        }
        if (!v.contains("x") || !v.contains("y")) {
            return err(ErrorCode::InvalidArgument, std::string(field) + " needs both x and y");
        }
        return Point{v["x"].as_double(), v["y"].as_double(), space};
    }
    if (v.is_string()) {
        // "100,200" and "100,200@image" are both accepted; the CLI passes
        // coordinates this way and hand-written JSON often does too.
        std::string s = v.as_string();
        const auto at = s.find('@');
        if (at != std::string::npos) {
            auto sp = space_from_string(s.substr(at + 1));
            if (sp) space = *sp;
            s = s.substr(0, at);
        }
        const auto comma = s.find(',');
        if (comma == std::string::npos) {
            return err(ErrorCode::InvalidArgument,
                       std::string(field) + " string form must be \"x,y\" or \"x,y@space\"");
        }
        try {
            return Point{std::stod(s.substr(0, comma)), std::stod(s.substr(comma + 1)), space};
        } catch (...) {
            return err(ErrorCode::InvalidArgument, std::string(field) + " has non-numeric parts");
        }
    }
    return err(ErrorCode::InvalidArgument,
               std::string(field) + " must be [x,y], {x,y,space} or \"x,y\"");
}

Result<Rect> parse_rect(const Value& v, const char* field) {
    Space space = Space::Logical;
    if (v.is_array() && v.size() >= 4) {
        return Rect{v[0].as_double(), v[1].as_double(), v[2].as_double(), v[3].as_double(), space};
    }
    if (v.is_object()) {
        if (v.contains("space")) {
            auto s = space_from_string(v["space"].as_string());
            if (s) space = *s;
        }
        return Rect{v["x"].as_double(), v["y"].as_double(), v["w"].as_double(), v["h"].as_double(),
                    space};
    }
    if (v.is_string()) {
        // "x,y,w,h" and "x,y,w,h@physical", matching the string form points
        // accept. The CLI turns a bare --region 0,0,800,600 into a string, so
        // without this the documented shorthand does not work there.
        std::string s = v.as_string();
        const auto at = s.find('@');
        if (at != std::string::npos) {
            auto sp = space_from_string(s.substr(at + 1));
            if (!sp) {
                return err(ErrorCode::InvalidArgument,
                           std::string(field) + " has an unknown space suffix");
            }
            space = *sp;
            s = s.substr(0, at);
        }
        double parts[4] = {0, 0, 0, 0};
        std::size_t start = 0;
        int found = 0;
        for (; found < 4; ++found) {
            const auto comma = s.find(',', start);
            const std::string piece =
                s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            try {
                parts[found] = std::stod(piece);
            } catch (...) {
                break;
            }
            if (comma == std::string::npos) {
                ++found;
                break;
            }
            start = comma + 1;
        }
        if (found != 4) {
            return err(ErrorCode::InvalidArgument,
                       std::string(field) + " string form must be \"x,y,w,h\"");
        }
        return Rect{parts[0], parts[1], parts[2], parts[3], space};
    }
    return err(ErrorCode::InvalidArgument,
               std::string(field) + " must be [x,y,w,h], {x,y,w,h} or \"x,y,w,h\"");
}

namespace {

// --- option builders -------------------------------------------------------

MotionOptions motion_from(const Value& v) {
    MotionOptions m;
    const std::string profile = v["profile"].as_string("ease");
    if (profile == "instant")
        m.profile = MotionProfile::Instant;
    else if (profile == "linear")
        m.profile = MotionProfile::Linear;
    else if (profile == "human")
        m.profile = MotionProfile::Human;
    else
        m.profile = MotionProfile::EaseInOut;
    if (v.contains("duration_ms")) m.duration = ms(v["duration_ms"], 180);
    if (v.contains("rate_hz")) m.rate_hz = static_cast<int>(v["rate_hz"].as_int(120));
    if (v.contains("seed")) m.seed = static_cast<std::uint64_t>(v["seed"].as_int(0));
    if (v.contains("jitter_px")) m.jitter_px = v["jitter_px"].as_double(1.2);
    if (v.contains("overshoot_px")) m.overshoot_px = v["overshoot_px"].as_double(6.0);
    return m;
}

Result<StrokeOptions> stroke_from(const Value& v) {
    StrokeOptions s;
    if (v.contains("button")) {
        auto b = button_from_string(v["button"].as_string("left"));
        if (!b) return b.error();
        s.button = b.value();
    }
    s.modifiers = parse_modifiers(v["modifiers"]);
    s.motion = motion_from(v);
    if (!v.contains("duration_ms")) s.motion.duration = std::chrono::milliseconds{450};
    s.smooth = v["smooth"].as_bool(false);
    s.smooth_tension = v.contains("tension") ? v["tension"].as_double(0.5) : 0.5;
    if (v.contains("settle_ms")) s.settle_before_release = ms(v["settle_ms"], 40);
    s.use_pen = v["pen"].as_bool(false);
    return s;
}

// --- capture ---------------------------------------------------------------

Result<CaptureOptions> capture_from(Session& s, const Value& v) {
    CaptureOptions o;
    if (v.contains("display")) o.display_index = static_cast<std::int32_t>(v["display"].as_int(0));
    if (v.contains("window_id")) o.window_id = static_cast<std::uint64_t>(v["window_id"].as_int(0));
    if (v.contains("region") && !v["region"].is_null()) {
        auto r = parse_rect(v["region"], "region");
        if (!r) return r.error();
        o.region = r.value();
    }
    o.max_dimension = v.contains("max_dimension")
                          ? static_cast<std::int32_t>(v["max_dimension"].as_int(0))
                          : s.config().default_max_capture_dimension;
    if (v.contains("scale")) o.scale = v["scale"].as_double(1.0);
    o.include_cursor = v["cursor"].as_bool(true);
    return o;
}

ActionResult do_capture(Session& s, const Value& args, bool as_image) {
    auto screen = s.screen();
    if (!screen) return fail(screen.error());
    auto disp = s.displays();
    if (disp) (void)disp.value()->refresh();

    auto opts = capture_from(s, args);
    if (!opts) return fail(opts.error());

    auto frame = screen.value()->capture(opts.value());
    if (!frame) return fail(frame.error());

    EncodeOptions eo;
    const std::string fmt = args["format"].as_string("png");
    if (fmt == "jpeg" || fmt == "jpg")
        eo.format = ImageFormat::JPEG;
    else if (fmt == "webp")
        eo.format = ImageFormat::WEBP;
    eo.quality = static_cast<int>(args["quality"].as_int(80));

    auto bytes = encode(frame.value(), eo);
    if (!bytes) return fail(bytes.error());

    ActionResult r;
    r.ok = true;
    if (as_image) {
        r.image = bytes.value();
        r.image_mime = (eo.format == ImageFormat::JPEG) ? "image/jpeg" : "image/png";
    }

    Value v = Value::object();
    v.set("width", frame.value().width);
    v.set("height", frame.value().height);
    v.set("bytes", static_cast<long long>(bytes.value().size()));
    v.set("source_physical", rect_json(frame.value().source_physical));
    v.set("image_scale", frame.value().scale);
    // Spelling out the mapping is what stops the classic Retina bug: an agent
    // reading pixel (400,300) off this image must know what that is on screen.
    v.set("coordinate_note",
          "Coordinates read from this image are in image space. Pass them back as "
          "{\"x\":...,\"y\":...,\"space\":\"image\"} and they will be converted automatically.");
    r.value = v;

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Captured %dx%d px (%zu bytes %s).", frame.value().width,
                  frame.value().height, bytes.value().size(), fmt.c_str());
    r.text = buf;
    return r;
}

// --- accessibility ---------------------------------------------------------

TreeOptions tree_from(const Value& v) {
    TreeOptions o;
    if (v.contains("max_depth")) o.max_depth = static_cast<int>(v["max_depth"].as_int(60));
    if (v.contains("max_nodes")) o.max_nodes = static_cast<int>(v["max_nodes"].as_int(4000));
    if (v.contains("budget_ms")) o.budget = ms(v["budget_ms"], 1200);
    o.interactive_only = v["interactive_only"].as_bool(true);
    o.include_offscreen = v["include_offscreen"].as_bool(false);
    o.include_static_text = v["include_text"].as_bool(true);
    if (v.contains("pid")) o.pid = v["pid"].as_int(0);
    if (v.contains("window_id")) o.window_id = static_cast<std::uint64_t>(v["window_id"].as_int(0));
    return o;
}

Value node_json(const Node& n, bool with_children) {
    Value v = Value::object();
    if (n.label >= 0) v.set("label", n.label);
    v.set("role", to_string(n.role));
    if (!n.raw_role.empty()) v.set("raw_role", n.raw_role);
    if (!n.name.empty()) v.set("name", n.name);
    if (!n.value.empty() && n.value != n.name) v.set("value", n.value);
    if (!n.automation_id.empty()) v.set("id", n.automation_id);
    v.set("bounds", rect_json(n.bounds));
    v.set("center", point_json(n.bounds.center()));
    if (!n.enabled) v.set("enabled", false);
    if (n.focused) v.set("focused", true);
    if (n.scrollable) v.set("scrollable", true);
    if (n.checked.has_value()) v.set("checked", *n.checked);
    if (n.selected.has_value() && *n.selected) v.set("selected", true);
    if (!n.app_name.empty()) v.set("app", n.app_name);
    if (!n.actions.empty()) {
        Value a = Value::array();
        for (const auto& s : n.actions) a.push_back(s);
        v.set("actions", a);
    }
    if (with_children && !n.children.empty()) {
        Value c = Value::array();
        for (const auto& ch : n.children) c.push_back(node_json(ch, true));
        v.set("children", c);
    }
    return v;
}

// Resolves either an explicit point or an element label to a screen point.
// Labels come from the most recent snapshot, which the dispatcher caches so a
// click can follow a snapshot without re-walking the tree.
struct LabelCache {
    std::vector<Node> nodes;
    std::chrono::steady_clock::time_point taken{};
};
LabelCache& label_cache() {
    static LabelCache c;
    return c;
}

Result<Point> resolve_target(Session& s, const Value& args) {
    if (args.contains("label") && !args["label"].is_null()) {
        const auto label = static_cast<std::int32_t>(args["label"].as_int(-1));
        auto& cache = label_cache();
        const auto age = std::chrono::steady_clock::now() - cache.taken;
        if (cache.nodes.empty()) {
            return err(
                ErrorCode::NotFound,
                "no snapshot has been taken, so label " + std::to_string(label) + " means nothing",
                "Call snapshot first; labels are assigned by it.");
        }
        if (age > std::chrono::seconds{60}) {
            return err(ErrorCode::NotFound,
                       "the labelled snapshot is over a minute old and is probably stale",
                       "Take a fresh snapshot before clicking by label.");
        }
        for (const auto& n : cache.nodes) {
            if (n.label == label) return s.resolve(n.bounds.center());
        }
        return err(ErrorCode::NotFound, "no element with label " + std::to_string(label));
    }
    if (args.contains("at") && !args["at"].is_null()) {
        auto p = parse_point(args["at"], "at");
        if (!p) return p.error();
        return s.resolve(p.value());
    }
    return err(ErrorCode::InvalidArgument, "provide either `at` (coordinates) or `label`");
}

// --- gesture ---------------------------------------------------------------

Result<GestureRequest> gesture_from(Session& s, const Value& args) {
    GestureRequest g;
    const std::string kind = args["kind"].as_string("swipe");
    if (kind == "tap")
        g.kind = GestureKind::Tap;
    else if (kind == "swipe")
        g.kind = GestureKind::Swipe;
    else if (kind == "pan")
        g.kind = GestureKind::Pan;
    else if (kind == "pinch" || kind == "zoom")
        g.kind = GestureKind::Pinch;
    else if (kind == "rotate")
        g.kind = GestureKind::Rotate;
    else if (kind == "smart_zoom")
        g.kind = GestureKind::SmartZoom;
    else if (kind == "force_press")
        g.kind = GestureKind::ForcePress;
    else if (kind == "edge_swipe")
        g.kind = GestureKind::EdgeSwipe;
    else if (kind == "long_press")
        g.kind = GestureKind::LongPress;
    else
        return err(ErrorCode::InvalidArgument, "unknown gesture kind '" + kind + "'");

    if (args.contains("at") && !args["at"].is_null()) {
        auto p = parse_point(args["at"], "at");
        if (!p) return p.error();
        auto resolved = s.resolve(p.value());
        if (!resolved) return resolved.error();
        g.center = resolved.value();
    } else {
        auto in = s.input();
        if (!in) return in.error();
        auto cur = in.value()->cursor_position();
        if (!cur) return cur.error();
        g.center = cur.value();
    }

    // A sensible default depends on the gesture: a tap or long press is one
    // finger, a pinch or rotate is inherently two, and a swipe is two on a
    // trackpad. Defaulting everything to two made `long_press` report itself
    // as emulated when the one-finger form is native.
    const int default_fingers =
        (g.kind == GestureKind::Tap || g.kind == GestureKind::LongPress ||
         g.kind == GestureKind::ForcePress || g.kind == GestureKind::EdgeSwipe)
            ? 1
            : 2;
    g.fingers = static_cast<int>(args["fingers"].as_int(default_fingers));
    const std::string dir = args["direction"].as_string("left");
    if (dir == "up")
        g.direction = SwipeDirection::Up;
    else if (dir == "down")
        g.direction = SwipeDirection::Down;
    else if (dir == "right")
        g.direction = SwipeDirection::Right;
    else
        g.direction = SwipeDirection::Left;

    if (args.contains("distance")) g.distance = args["distance"].as_double(200);
    if (args.contains("scale")) g.scale = args["scale"].as_double(2.0);
    if (args.contains("degrees")) g.rotation_degrees = args["degrees"].as_double(0);
    if (args.contains("spread")) g.spread = args["spread"].as_double(120);
    if (args.contains("pressure")) g.pressure = args["pressure"].as_double(1.0);
    g.duration = ms(args["duration_ms"], 300);
    g.hold = ms(args["hold_ms"], 0);
    g.modifiers = parse_modifiers(args["modifiers"]);
    g.require_native = args["require_native"].as_bool(false);

    for (const auto& p : args["path"].as_array()) {
        auto pt = parse_point(p, "path");
        if (!pt) return pt.error();
        auto resolved = s.resolve(pt.value());
        if (!resolved) return resolved.error();
        g.path.push_back(resolved.value());
    }
    return g;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// Argument shapes that recur across the registry. Raw string literals do not
// expand macros, so these are ordinary literals joined by adjacent-literal
// concatenation: R"(..."at":{)" CC_POINT R"(})". They are body-only, with no
// outer braces, so each use can add its own description.
//
// Every one names a concrete type. An untyped property tells a model nothing
// about the shape to send, and an untyped array element makes GitHub Copilot
// reject the whole tool ("tool parameters array type must have items"). The
// parsers still accept the [x,y] and "x,y@space" shorthands; the schema
// advertises the object form because it is the only one that carries a
// coordinate space, which is the thing most worth getting right.
#define CC_SPACE                                                                    \
    "\"space\":{\"type\":\"string\",\"enum\":[\"logical\",\"physical\",\"image\"]," \
    "\"default\":\"logical\"}"

#define CC_POINT                                                      \
    "\"type\":\"object\",\"required\":[\"x\",\"y\"],\"properties\":{" \
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}," CC_SPACE "}"

#define CC_RECT                                                                   \
    "\"type\":\"object\",\"required\":[\"x\",\"y\",\"w\",\"h\"],\"properties\":{" \
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},"                    \
    "\"w\":{\"type\":\"number\"},\"h\":{\"type\":\"number\"}," CC_SPACE "}"

#define CC_POINT_PRESSURE                                               \
    "\"type\":\"object\",\"required\":[\"x\",\"y\"],\"properties\":{"   \
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}," CC_SPACE \
    ","                                                                 \
    "\"pressure\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"   \
    "\"dwell_ms\":{\"type\":\"integer\",\"minimum\":0}}"

#define CC_MODIFIERS                                                               \
    "\"type\":\"array\",\"description\":\"Held for the duration of the action.\"," \
    "\"items\":{\"type\":\"string\",\"enum\":[\"cmd\",\"ctrl\",\"alt\",\"shift\"," \
    "\"option\",\"meta\",\"win\",\"super\"]}"

const std::vector<ActionSpec>& registry() {
    static const std::vector<ActionSpec> specs = {
        {"permissions", "Permissions",
         "Check, and optionally request, the OS permissions this tool needs. On macOS this is "
         "the one to run when the element tree comes back empty: it distinguishes a real "
         "denial from the inherited-grant case, where the process is trusted but still "
         "refused, and prints the exact binary path to add in System Settings.",
         R"({"type":"object","properties":{
            "request":{"type":"boolean","default":false,"description":"Prompt for anything missing and open the relevant settings page."},
            "permission":{"type":"string","enum":["accessibility","screen_recording","input_monitoring","touch_injection"],"description":"Limit to one; default is all."}}})",
         false, false},

        {"system", "System",
         "Shell-level actions every desktop has but none exposes as an API: open the launcher "
         "or search, switch virtual desktops, show the overview, lock the screen. Use this "
         "rather than trying to synthesize the trackpad gesture - a four-finger swipe is "
         "interpreted by the touchpad driver from HID reports and cannot be produced by touch "
         "injection, whereas these shortcuts are exactly what that driver ends up invoking. "
         "They are also instant, with no gesture-recogniser timing to lose. Note that success "
         "means the shortcut was delivered, not that the OS acted on it: these are all "
         "user-remappable, and a disabled or rebound shortcut fails silently. Take a "
         "screenshot if you need to confirm the effect.",
         R"({"type":"object","properties":{
            "action":{"type":"string","description":"search, launcher, app_switcher, overview, show_desktop, next_desktop, previous_desktop, notifications, screenshot_ui, emoji, run_dialog, settings, file_manager, lock_screen. Aliases like spotlight, start, task_view, mission_control and explorer are accepted. Omit to list what this host supports."}}})",
         false, true},

        {"capabilities", "Capabilities",
         "Report what this host can do: displays and their DPI scale, which backends came up, "
         "which permissions are missing, gesture fidelity per gesture type, and which mobile "
         "device tooling is installed. Call this first on an unfamiliar machine.",
         R"({"type":"object","properties":{},"additionalProperties":false})", true, false},

        {"displays", "Displays",
         "List every display with logical bounds, physical pixel bounds, scale factor and DPI. "
         "Use this to understand a multi-monitor or Retina layout before working with "
         "coordinates.",
         R"({"type":"object","properties":{},"additionalProperties":false})", true, false},

        {"screenshot", "Screenshot",
         "Capture the screen, a single display, a window, or a region. Returns an image plus the "
         "mapping needed to turn image pixels back into screen coordinates. Downscales to "
         "max_dimension so the payload stays small on high-resolution displays.",
         R"({"type":"object","properties":{
            "display":{"type":"integer","description":"Zero-based display index."},
            "window_id":{"description":"Capture just this window. From `windows`.","type":"integer"},
            "region":{"description":"Capture just this rectangle instead of a whole display.",)" CC_RECT
         R"(},
            "max_dimension":{"type":"integer","description":"Longest side in pixels; 0 = no limit."},
            "scale":{"description":"Multiply the captured size; 0.5 halves it. Ignored when max_dimension applies.","type":"number"},
            "format":{"description":"png keeps text crisp; jpeg is smaller for photographic content.","type":"string","enum":["png","jpeg"],"default":"png"},
            "quality":{"description":"JPEG quality 1-100. Ignored for png.","type":"integer","default":80},
            "cursor":{"description":"Draw the mouse pointer into the image.","type":"boolean","default":true}}})",
         true, false},

        {"snapshot", "Snapshot",
         "Screenshot plus the accessibility tree, with every interactive element assigned a "
         "numbered label. Click, type and scroll accept those labels, which is more reliable "
         "than pixel coordinates because it survives scrolling and window movement.",
         R"({"type":"object","properties":{
            "vision":{"type":"boolean","default":true,"description":"Include the screenshot."},
            "interactive_only":{"description":"Only elements that can be clicked, typed into or scrolled. Turn off to see static text too, at the cost of a much larger tree.","type":"boolean","default":true},
            "include_offscreen":{"description":"Include elements scrolled out of view or on another Space.","type":"boolean","default":false},
            "pid":{"type":"integer","description":"Restrict to one process (much faster)."},
            "max_nodes":{"description":"Stop after this many elements and report the tree as truncated.","type":"integer","default":4000},
            "budget_ms":{"description":"Wall-clock budget for the walk. An unresponsive app cannot stall the call past this.","type":"integer","default":1200},
            "max_dimension":{"description":"Longest side of the screenshot in pixels; 0 = no limit.","type":"integer"},
            "format":{"description":"png keeps text crisp; jpeg is smaller.","type":"string","enum":["png","jpeg"]}}})",
         true, false},

        {"zoom", "Zoom",
         "Re-capture a region of the screen at full resolution. Use it to read small text that "
         "is illegible in a downscaled screenshot. Read-only.",
         R"({"type":"object","required":["region"],"properties":{
            "region":{"description":"The rectangle to re-capture at full resolution.",)" CC_RECT
         R"(},
            "format":{"description":"png keeps text crisp; jpeg is smaller.","type":"string","enum":["png","jpeg"]}}})",
         true, false},

        {"cursor_position", "CursorPosition", "Where the mouse pointer currently is.",
         R"({"type":"object","properties":{},"additionalProperties":false})", true, false},

        {"move", "Move",
         "Move the pointer. Use profile=\"human\" for motion that passes hover and drag "
         "heuristics, or \"instant\" when only the final position matters.",
         R"({"type":"object","properties":{
            "at":{"description":"Where to move the pointer. Omit when using label.",)" CC_POINT
         R"(},
            "label":{"type":"integer","description":"Element label from the last snapshot."},
            "profile":{"description":"Motion shape. human adds jitter and an overshoot-and-settle so hover and drag heuristics fire.","type":"string","enum":["instant","linear","ease","human"],"default":"ease"},
            "duration_ms":{"description":"How long the movement takes. Ignored by the instant profile.","type":"integer","default":180},
            "seed":{"type":"integer","description":"Makes a human path reproducible."}}})",
         false, false},

        {"click", "Click",
         "Click at coordinates or on a labelled element. count=0 hovers without clicking, 2 is a "
         "double click and 3 a triple click, emitted as one stream with the OS click-count field "
         "set so applications see a real multi-click.",
         R"({"type":"object","properties":{
            "at":{"description":"Where to click. Omit when using label.",)" CC_POINT R"(},
            "label":{"description":"Element label from the last snapshot. Preferred over coordinates: it survives scrolling and window movement.","type":"integer"},
            "button":{"description":"Which button. back and forward are the side buttons browsers use for history.","type":"string","enum":["left","right","middle","back","forward"],"default":"left"},
            "count":{"description":"0 hovers without clicking, 1 single, 2 double, 3 triple.","type":"integer","minimum":0,"maximum":3,"default":1},
            "modifiers":{)" CC_MODIFIERS R"(},
            "press_ms":{"description":"Hold the button down this long before releasing.","type":"integer","default":12}}})",
         false, true},

        {"scroll", "Scroll",
         "Scroll at a point. Set pixel_units for smooth trackpad-style scrolling, and phased to "
         "emit begin/change/end markers so momentum-aware applications treat it as one gesture.",
         R"({"type":"object","properties":{
            "at":{"description":"Defaults to the current pointer position.",)" CC_POINT R"(},
            "label":{"description":"Scroll the element with this label from the last snapshot.","type":"integer"},
            "direction":{"description":"Which way the content moves.","type":"string","enum":["up","down","left","right"],"default":"down"},
            "clicks":{"description":"Number of wheel detents. Ignored when pixel_units is set.","type":"integer","default":3},
            "pixel_units":{"description":"Scroll by exact pixels instead of wheel detents, as a trackpad does.","type":"boolean","default":false},
            "pixels_per_click":{"description":"Pixels each detent is worth when pixel_units is set.","type":"integer","default":40},
            "phased":{"description":"Emit begin/change/end phases so momentum-aware apps treat it as a trackpad gesture rather than a wheel.","type":"boolean","default":false},
            "modifiers":{)" CC_MODIFIERS R"(}}})",
         false, false},

        {"drag", "Drag",
         "Press at one point, travel, release at another, as one uninterrupted event stream with "
         "a dwell after the press and a settle before the release. That timing is what makes "
         "drag-and-drop actually register in most toolkits.",
         R"({"type":"object","required":["from","to"],"properties":{
            "from":{"description":"Where the press happens.",)" CC_POINT R"(},
            "to":{"description":"Where the release happens.",)" CC_POINT R"(},
            "button":{"description":"Which button to hold during the drag.","type":"string","default":"left"},
            "duration_ms":{"description":"Travel time between the two points.","type":"integer","default":450},
            "profile":{"description":"Motion shape for the travel.","type":"string","enum":["instant","linear","ease","human"]},
            "modifiers":{)" CC_MODIFIERS R"(},
            "settle_ms":{"description":"Pause after pressing, before moving. Drop targets often need this to register the drag at all.","type":"integer","default":40}}})",
         false, true},

        {"stroke", "Stroke",
         "Draw a freehand path with the button held: signatures, canvas drawing, lasso "
         "selections, gesture passwords. Set smooth to spline through the points, and give each "
         "point a pressure for pen-capable backends.",
         R"({"type":"object","required":["points"],"properties":{
            "points":{"description":"The path, in order. At least two.","type":"array","minItems":2,"items":{)" CC_POINT_PRESSURE
         R"(}},
            "button":{"description":"Which button is held for the whole stroke.","type":"string","default":"left"},
            "smooth":{"description":"Spline through the points instead of joining them with straight lines.","type":"boolean","default":false},
            "tension":{"description":"Curve tightness when smooth is set. 0 is angular, 1 is loose.","type":"number","default":0.5},
            "duration_ms":{"description":"Total time for the whole path.","type":"integer","default":450},
            "pen":{"description":"Route as pen input where the platform supports it, so pressure is honoured.","type":"boolean","default":false},
            "modifiers":{)" CC_MODIFIERS R"(}}})",
         false, true},

        {"gesture", "Gesture",
         "Multi-touch: pinch/zoom, rotate, n-finger tap, swipe and pan, long press, force press, "
         "edge swipe. Fidelity differs by platform (Windows and Linux inject real touch "
         "contacts; macOS emulates most of these) - call capabilities to see which you will get, "
         "or set require_native to refuse emulation.",
         R"({"type":"object","required":["kind"],"properties":{
            "kind":{"description":"Which gesture to perform. Check `capabilities` first: several are emulated on some platforms.","type":"string","enum":["tap","swipe","pan","pinch","rotate","smart_zoom","force_press","edge_swipe","long_press"]},
            "at":{"description":"Gesture centre; defaults to the pointer position.",)" CC_POINT
         R"(},
            "fingers":{"type":"integer","minimum":1,"maximum":5,"description":"Defaults to 1 for tap/long_press/force_press/edge_swipe, 2 otherwise."},
            "direction":{"description":"Direction for swipe, pan and edge_swipe.","type":"string","enum":["up","down","left","right"]},
            "distance":{"description":"Travel distance in points for swipe and pan.","type":"number","default":200},
            "scale":{"type":"number","default":2,"description":"Pinch: >1 zooms in, <1 out."},
            "degrees":{"type":"number","description":"Rotate: positive is counter-clockwise."},
            "spread":{"description":"Starting distance between contacts for pinch and rotate.","type":"number","default":120},
            "duration_ms":{"description":"How long the gesture takes.","type":"integer","default":300},
            "hold_ms":{"description":"Hold at the end before releasing the contacts.","type":"integer","default":0},
            "path":{"type":"array","description":"Pan only.",
                    "items":{)" CC_POINT R"(}},
            "require_native":{"description":"Fail rather than substitute an emulation. Use when a real multi-touch event is the point.","type":"boolean","default":false},
            "modifiers":{)" CC_MODIFIERS R"(}}})",
         false, true},

        {"key", "Key",
         "Press a key or chord: \"cmd+shift+a\", \"ctrl-c\", \"F5\", \"escape\". Space-separated "
         "chords run in sequence (\"cmd+k cmd+s\").",
         R"({"type":"object","required":["keys"],"properties":{
            "keys":{"description":"A chord such as \"cmd+shift+a\", or several separated by spaces to run in sequence (\"cmd+k cmd+s\").","type":"string"},
            "repeat":{"description":"Send the chord this many times.","type":"integer","default":1}}})",
         false, true},

        {"key_hold", "KeyHold", "Hold a key or chord down for a duration, then release it.",
         R"({"type":"object","required":["keys"],"properties":{
            "keys":{"description":"The chord to hold.","type":"string"},
            "duration_ms":{"description":"How long to hold it before releasing.","type":"integer","default":500}}})",
         false, true},

        {"key_down", "KeyDown",
         "Press a key and leave it held. Pair with key_up. Prefer key or key_hold unless you "
         "genuinely need the key held across other actions.",
         R"({"type":"object","required":["key"],"properties":{"key":{"description":"The key to press and leave held. Pair with key_up.","type":"string"}}})",
         false, true},

        {"key_up", "KeyUp", "Release a key held by key_down.",
         R"({"type":"object","required":["key"],"properties":{"key":{"description":"The key to release.","type":"string"}}})",
         false, true},

        {"type", "Type",
         "Type text into whatever has keyboard focus, or into a labelled/clicked field first. "
         "Unicode is injected directly, so emoji and non-Latin scripts work regardless of the "
         "active keyboard layout.",
         R"({"type":"object","required":["text"],"properties":{
            "text":{"description":"The text to type. Unicode is injected directly, so emoji and CJK do not depend on the keyboard layout.","type":"string"},
            "at":{"description":"Click here first.",)" CC_POINT R"(},
            "label":{"type":"integer","description":"Click this element first."},
            "clear":{"type":"boolean","default":false,"description":"Select-all and delete first."},
            "enter":{"type":"boolean","default":false,"description":"Press Return afterwards."},
            "cps":{"type":"number","description":"Characters per second; 0 = as fast as possible."}}})",
         false, true},

        {"wait", "Wait", "Pause for a number of milliseconds.",
         R"({"type":"object","required":["ms"],"properties":{"ms":{"description":"How long to pause.","type":"integer"}}})",
         true, false},

        {"wait_for", "WaitFor",
         "Poll until a UI condition holds, inside one call. Much cheaper than a snapshot loop "
         "from the client.",
         R"({"type":"object","required":["condition"],"properties":{
            "condition":{"description":"What to wait for.","type":"string","enum":["text_exists","element_exists","window_exists","window_focused","element_enabled"]},
            "text":{"description":"Text to look for, for the text condition.","type":"string"},
            "window":{"description":"Window title to match, for the window conditions.","type":"string"},
            "timeout_ms":{"description":"Give up after this long and report that the condition was not met.","type":"integer","default":10000},
            "interval_ms":{"description":"How often to re-check.","type":"integer","default":250}}})",
         true, false},

        {"windows", "Windows",
         "List, activate, move, resize, change the state of, or close windows.",
         R"({"type":"object","properties":{
            "mode":{"description":"What to do. list is read-only; the rest act on one window.","type":"string","enum":["list","activate","bounds","state","close","focused"],"default":"list"},
            "window_id":{"description":"Which window to act on. From mode=list.","type":"integer"},
            "title":{"type":"string","description":"Fuzzy-matched alternative to window_id."},
            "bounds":{"description":"Required for mode=bounds.",)" CC_RECT R"(},
            "state":{"description":"Target state for mode=state.","type":"string","enum":["normal","minimized","maximized","fullscreen","hidden"]},
            "include_offscreen":{"description":"Include windows on other Spaces or minimised.","type":"boolean","default":false}}})",
         false, true},

        {"app", "App", "List, launch, activate or quit applications.",
         R"({"type":"object","properties":{
            "mode":{"description":"What to do.","type":"string","enum":["list","launch","activate","quit"],"default":"list"},
            "name":{"type":"string","description":"Display name or bundle id."},
            "executable":{"description":"Application name, bundle id, or path, for launch.","type":"string"},
            "args":{"description":"Arguments passed to the executable on launch.","type":"array","items":{"type":"string"}},
            "cwd":{"description":"Working directory for launch.","type":"string"},
            "pid":{"description":"Which process to act on, instead of matching by name.","type":"integer"},
            "force":{"description":"Kill rather than asking the app to quit. Unsaved work is lost.","type":"boolean","default":false},
            "timeout_ms":{"description":"How long to wait for the app to appear or exit.","type":"integer","default":8000}}})",
         false, true},

        {"elements", "Elements",
         "Query the accessibility tree without taking a screenshot: the full tree, the element "
         "under a point, or the focused element.",
         R"({"type":"object","properties":{
            "mode":{"description":"tree returns the whole tree, at returns the element under a point, focused returns the focused one.","type":"string","enum":["tree","at","focused"],"default":"tree"},
            "at":{"description":"Required for mode=at.",)" CC_POINT R"(},
            "pid":{"description":"Restrict to one process. Much faster than walking every application.","type":"integer"},
            "interactive_only":{"description":"Only elements that can be interacted with.","type":"boolean","default":true},
            "max_nodes":{"description":"Stop after this many elements and report the tree as truncated.","type":"integer"},
            "budget_ms":{"description":"Wall-clock budget for the walk.","type":"integer"}}})",
         true, false},

        {"clipboard", "Clipboard", "Read or write the clipboard.",
         R"({"type":"object","properties":{
            "mode":{"description":"read returns the current contents; write replaces them.","type":"string","enum":["get","set"],"default":"get"},
            "text":{"description":"What to put on the clipboard, for mode=write.","type":"string"}}})",
         false, false},

        {"shell", "Shell",
         "Run a command. Disabled when the server is started with --no-shell. The command runs "
         "with the same privileges as this process.",
         R"({"type":"object","required":["command"],"properties":{
            "command":{"description":"The command line to run. On Windows this goes to PowerShell unless interpreter says otherwise.","type":"string"},
            "shell":{"type":"string","description":"Override the interpreter; \"osascript\" on macOS."},
            "cwd":{"description":"Working directory for the command.","type":"string"},
            "timeout_ms":{"description":"Kill the command after this long.","type":"integer","default":30000}}})",
         false, true},

        {"process", "Process", "List or terminate processes.",
         R"({"type":"object","properties":{
            "mode":{"description":"list is read-only; kill terminates.","type":"string","enum":["list","kill"],"default":"list"},
            "pid":{"description":"Kill exactly this process.","type":"integer"},
            "name":{"description":"Match processes whose name contains this.","type":"string"},
            "force":{"description":"Send an uncatchable kill. The process cannot save state or clean up.","type":"boolean","default":false},
            "limit":{"description":"Return at most this many processes.","type":"integer","default":30},
            "sort":{"description":"How to order the list.","type":"string","enum":["memory","name","pid"],"default":"memory"}}})",
         false, true},

        {"notify", "Notify", "Show a desktop notification.",
         R"({"type":"object","required":["message"],"properties":{
            "message":{"description":"Body text.","type":"string"},
            "title":{"description":"Notification title.","type":"string"},
            "subtitle":{"description":"Secondary line, where the platform shows one.","type":"string"},
            "sound":{"description":"Play the default notification sound.","type":"string"}}})",
         false, false},

        {"registry", "Registry",
         "Read or write the Windows registry. Returns unsupported on macOS and Linux, and is "
         "disabled unless the server is started with --allow-registry.",
         R"({"type":"object","required":["mode","path"],"properties":{
            "mode":{"description":"read, write, delete or list. Windows only.","type":"string","enum":["get","set","delete","list"]},
            "path":{"description":"Key path, for example HKCU\\\\Software\\\\Example.","type":"string"},
            "name":{"description":"Value name within the key.","type":"string"},
            "value":{"description":"Value to write, for mode=write.","type":"string"},
            "type":{"description":"Registry value type, for mode=write.","type":"string","default":"String"}}})",
         false, true},

        {"device", "Device",
         "Drive an iOS simulator, Android emulator, or a phone mirrored on screen. Coordinates "
         "are in the device's own points, translated automatically. Modes cover listing, "
         "lifecycle, input, screenshots and the device UI tree.",
         R"({"type":"object","properties":{
            "mode":{"description":"What to do with the device.","type":"string","enum":["list","info","boot","shutdown","tap","swipe","stroke","gesture","type","button","screenshot","shell","install","launch","terminate","open_url","tree"],"default":"list"},
            "device":{"type":"string","description":"UDID, adb serial, or a fuzzy name."},
            "transport":{"description":"bridge uses the device's own tooling and is exact; onscreen locates the device window and works for anything visible, including mirrored phones. auto prefers bridge.","type":"string","enum":["auto","bridge","onscreen"],"default":"auto"},
            "at":{"description":"Device-space point for tap.",)" CC_POINT R"(},
            "from":{"description":"Start point for swipe, in device points.",)" CC_POINT
         R"(},"to":{"description":"End point for swipe, in device points.",)" CC_POINT R"(},
            "points":{"type":"array","description":"Device points, for stroke and gesture modes.",
                      "items":{"type":"object","required":["x","y"],"properties":{"x":{"type":"number"},"y":{"type":"number"}}}},
            "text":{"description":"Text to type, or the bundle id / package name for install, launch and terminate.","type":"string"},
            "button":{"type":"string","description":"home, back, power, enter, volumeup, ..."},
            "duration_ms":{"description":"How long the action takes.","type":"integer"},
            "hold_ms":{"description":"Hold at the end before releasing.","type":"integer"},
            "count":{"description":"Contact count for gestures that take one.","type":"integer","default":1},
            "path":{"type":"string","description":"App bundle/apk path for install."},
            "bundle":{"type":"string","description":"Bundle id or package name."},
            "url":{"description":"URL to open, for mode=open_url.","type":"string"},
            "command":{"description":"Command to run on the device, for mode=shell.","type":"string"},
            "booted_only":{"description":"Only list devices that are running.","type":"boolean","default":false},
            "kind":{"type":"string","description":"Gesture kind for mode=gesture."},
            "format":{"description":"Screenshot format.","type":"string","enum":["png","jpeg"]}}})",
         false, true},

        {"release_all", "ReleaseAll",
         "Release every held mouse button, key and touch contact. Use it to recover after an "
         "interrupted drag leaves the desktop in a stuck state.",
         R"({"type":"object","properties":{},"additionalProperties":false})", false, false},

        {"batch", "Batch",
         "Run several actions in one call. Each round trip to this server costs far more than "
         "the actions themselves, so batching a predictable sequence (click, type, press Return) "
         "is dramatically faster. Stops at the first failure and reports which index failed.",
         R"({"type":"object","required":["actions"],"properties":{
            "actions":{"description":"The actions to run in order, each an object with an \"action\" key naming the tool and the rest of its arguments alongside. Stops at the first failure and reports which index failed.","type":"array","items":{"type":"object","required":["action"]}}}})",
         false, true},
    };
    return specs;
}

const ActionSpec* find_spec(std::string_view name) {
    for (const auto& s : registry()) {
        if (name == s.name) return &s;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace {

Result<Permission> permission_from(std::string_view name) {
    if (name == "accessibility") return Permission::Accessibility;
    if (name == "screen_recording") return Permission::ScreenRecording;
    if (name == "input_monitoring") return Permission::InputMonitoring;
    if (name == "touch_injection") return Permission::TouchInjection;
    return err(ErrorCode::InvalidArgument, "unknown permission '" + std::string(name) + "'");
}

Value permission_json(const PermissionStatus& p) {
    Value v = Value::object();
    v.set("permission", to_string(p.permission));
    v.set("state", to_string(p.state));
    if (!p.detail.empty()) v.set("detail", p.detail);
    if (!p.remedy.empty()) v.set("remedy", p.remedy);
    v.set("can_prompt", p.can_prompt);
    if (!p.affects.empty()) {
        Value a = Value::array();
        for (const auto& x : p.affects) a.push_back(x);
        v.set("affects", a);
    }
    return v;
}

ActionResult act_permissions(Session&, const Value& args) {
    const bool request = args["request"].as_bool(false);

    std::vector<PermissionStatus> statuses;
    if (args.contains("permission") && !args["permission"].is_null()) {
        auto which = permission_from(args["permission"].as_string());
        if (!which) return fail(which.error());
        statuses.push_back(request ? request_permission(which.value())
                                   : check_permission(which.value()));
    } else if (request) {
        // Only prompt for what is actually missing: asking for something
        // already granted is a no-op that still opens a settings pane at the
        // user, which is worse than useless.
        for (const auto& current : check_permissions()) {
            if (current.state == PermissionState::Denied ||
                current.state == PermissionState::NotDetermined) {
                statuses.push_back(request_permission(current.permission));
            } else {
                statuses.push_back(current);
            }
        }
    } else {
        statuses = check_permissions();
    }

    Value list = Value::array();
    std::string text;
    bool all_good = true;
    for (const auto& p : statuses) {
        list.push_back(permission_json(p));
        const bool good =
            (p.state == PermissionState::Granted || p.state == PermissionState::NotRequired);
        all_good = all_good && good;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%-18s %s\n", to_string(p.permission), to_string(p.state));
        text += buf;
        if (!good) {
            if (!p.detail.empty()) text += "  " + p.detail + "\n";
            if (!p.remedy.empty()) text += "  " + p.remedy + "\n";
            text += "\n";
        }
    }

    Value out = Value::object();
    out.set("permissions", list);
    out.set("all_granted", all_good);
    out.set("executable", executable_path());
    const std::string bundle = bundle_path();
    if (!bundle.empty()) out.set("bundle", bundle);
    out.set("own_tcc_identity", has_own_tcc_identity());

    // Naming the process that actually owns the grant is the difference
    // between "permissions look fine but nothing works" and an obvious fix.
    if (const std::string owner = permission_owner(); !owner.empty()) {
        out.set("permissions_attributed_to", owner);
        text += "\nPermissions for this process are attributed to " + owner +
                ", not to the binary\nitself, which is why\n  " + executable_path() +
                "\ndoes not appear in System Settings. That is normal for a command-line "
                "tool started\nfrom a terminal. If element queries come back empty anyway, "
                "add the binary there\nwith the + button, or build the app bundle "
                "(`cmake --build build --target macos_bundle`)\nand launch it with `open` so "
                "it holds its own grant.\n";
    }
    return succeed(text, out);
}

ActionResult act_system(Session& s, const Value& args) {
    if (!args.contains("action") || args["action"].is_null()) {
        Value list = Value::array();
        std::string text;
        for (const auto& info : system_actions()) {
            Value v = Value::object();
            v.set("action", to_string(info.action));
            v.set("supported", info.supported);
            v.set("mechanism", info.mechanism);
            if (!info.note.empty()) v.set("note", info.note);
            list.push_back(v);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "%-18s %-3s ", to_string(info.action),
                          info.supported ? "ok" : "--");
            text += buf;
            text += info.mechanism.empty() ? "(none)" : info.mechanism;
            text += "\n";
            if (!info.note.empty()) text += "                      " + info.note + "\n";
        }
        Value out = Value::object();
        out.set("actions", list);
        return succeed(text, out);
    }

    auto which = system_action_from_string(args["action"].as_string());
    if (!which) return fail(which.error());

    const SystemActionInfo info = describe_system_action(which.value());
    if (!info.supported) {
        return fail(ErrorCode::Unsupported,
                    std::string("'") + to_string(which.value()) + "' is not available here",
                    info.note);
    }

    auto in = s.input();
    if (!in) return fail(in.error());
    auto sys = s.system();

    if (auto st = perform_system_action(which.value(), *in.value(), sys ? sys.value() : nullptr);
        !st) {
        return fail(st.error());
    }

    Value v = Value::object();
    v.set("action", to_string(which.value()));
    v.set("mechanism", info.mechanism);
    if (!info.note.empty()) v.set("note", info.note);
    v.set("delivered", true);
    v.set("verified", false);
    return succeed(std::string("Sent ") + info.mechanism + " for " + to_string(which.value()) +
                       ". The shortcut was delivered; whether the OS acted on it is not "
                       "observable from a key event, so take a screenshot if it matters.",
                   v);
}

ActionResult act_capabilities(Session& s, const Value&) {
    json::ParseError pe;
    Value v = json::parse(s.capability_report(), &pe);

    // The JSON is for programs; a human running `--doctor` wants to
    // read it. Previously this returned the single word "Capability report.",
    // which told the caller nothing at all.
    std::string text;
    text += std::string("computer-control ") + v["version"].as_string() + " on " +
            v["platform"].as_string() + "\n\n";

    text += "displays\n";
    for (const auto& d : v["displays"].as_array()) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "  [%lld] %-24.24s %.0fx%.0f pt @%gx = %.0fx%.0f px%s\n",
                      static_cast<long long>(d["index"].as_int()), d["name"].as_string().c_str(),
                      d["bounds_logical"]["w"].as_double(), d["bounds_logical"]["h"].as_double(),
                      d["scale"].as_double(), d["bounds_physical"]["w"].as_double(),
                      d["bounds_physical"]["h"].as_double(),
                      d["primary"].as_bool() ? "  (primary)" : "");
        text += buf;
    }

    text += "\nbackends\n";
    for (const auto& [name, info] : v["backends"].as_object()) {
        text += "  " + text::pad_utf8(name, 16);
        if (info["available"].as_bool()) {
            text += "ok    " + info["backend"].as_string() + "\n";
        } else {
            text += "FAIL  " + info["error"].as_string() + "\n";
        }
    }

    text += "\ngestures\n";
    for (const auto& [name, info] : v["gestures"].as_object()) {
        const std::string fidelity = info["fidelity"].as_string();
        text += "  " + text::pad_utf8(name, 16) + text::pad_utf8(fidelity, 14) +
                info["backend"].as_string() + "\n";
    }

    const auto& tooling = v["device_tooling"].as_array();
    text += "\ndevice tooling\n  ";
    if (tooling.empty()) {
        text += "none found\n";
    } else {
        for (std::size_t i = 0; i < tooling.size(); ++i) {
            if (i) text += ", ";
            text += tooling[i].as_string();
        }
        text += "\n";
    }

    return succeed(text, v);
}

ActionResult act_displays(Session& s, const Value&) {
    auto d = s.displays();
    if (!d) return fail(d.error());
    (void)d.value()->refresh();

    Value arr = Value::array();
    std::string text;
    for (const auto& disp : d.value()->displays()) {
        Value v = Value::object();
        v.set("index", disp.index);
        v.set("name", disp.name);
        v.set("primary", disp.primary);
        v.set("scale", disp.scale);
        v.set("dpi", disp.dpi);
        v.set("refresh_hz", disp.refresh_hz);
        v.set("bounds_logical", rect_json(disp.bounds_logical));
        v.set("bounds_physical", rect_json(disp.bounds_physical));
        v.set("work_area", rect_json(disp.work_area_logical));
        arr.push_back(v);

        char buf[256];
        std::snprintf(buf, sizeof(buf), "[%d] %s%s %gx%g pt @%gx (%gx%g px), %g dpi\n", disp.index,
                      disp.name.c_str(), disp.primary ? " (primary)" : "", disp.bounds_logical.w,
                      disp.bounds_logical.h, disp.scale, disp.bounds_physical.w,
                      disp.bounds_physical.h, disp.dpi);
        text += buf;
    }
    Value out = Value::object();
    out.set("displays", arr);
    out.set("virtual_bounds", rect_json(d.value()->virtual_bounds(Space::Logical)));
    return succeed(text, out);
}

ActionResult act_snapshot(Session& s, const Value& args) {
    ActionResult r;
    Value out = Value::object();

    if (args["vision"].as_bool(true)) {
        auto img = do_capture(s, args, true);
        if (!img.ok) return img;
        r.image = std::move(img.image);
        r.image_mime = img.image_mime;
        out.set("screenshot", img.value);
    }

    const bool have_image = !r.image.empty();

    // A missing accessibility grant should not throw away a perfectly good
    // screenshot, so a vision snapshot degrades to image-only. With vision off
    // there is nothing left to return, and reporting success would hide a real
    // permissions problem behind an empty element list.
    auto degraded = [&](const Error& e) {
        out.set("tree_error", e.message);
        if (!e.remedy.empty()) out.set("tree_remedy", e.remedy);
        r.value = out;
        if (have_image) {
            r.ok = true;
            r.text =
                "Screenshot captured, but the accessibility tree is unavailable: " + e.message +
                (e.remedy.empty() ? "" : "\n\n" + e.remedy);
        } else {
            r.ok = false;
            r.error = e;
        }
        return r;
    };

    auto a11y = s.accessibility();
    if (!a11y) return degraded(a11y.error());

    auto tree = a11y.value()->snapshot(tree_from(args));
    if (!tree) return degraded(tree.error());

    // Cache the flattened nodes so subsequent actions can address them by
    // label instead of by coordinate.
    auto& cache = label_cache();
    cache.nodes.clear();
    cache.taken = std::chrono::steady_clock::now();
    for (const Node* n : tree.value().interactive) cache.nodes.push_back(*n);

    Value elements = Value::array();
    std::string text;
    for (const Node* n : tree.value().interactive) {
        elements.push_back(node_json(*n, false));
        const std::string label = n->name.empty() ? n->value : n->name;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%3d %-14s ", n->label, to_string(n->role));
        text += buf;
        text += text::pad_utf8(label, 28);
        std::snprintf(buf, sizeof(buf), " (%.0f,%.0f)\n", n->bounds.center().x,
                      n->bounds.center().y);
        text += buf;
    }
    out.set("elements", elements);
    out.set("element_count", static_cast<long long>(tree.value().interactive.size()));
    out.set("nodes_walked", tree.value().node_count);
    out.set("elapsed_ms", static_cast<long long>(tree.value().elapsed.count()));
    if (tree.value().truncated) {
        out.set("truncated", true);
        out.set("truncation_reason", tree.value().truncation_reason);
    }

    auto win = s.windows();
    if (win) {
        if (auto f = win.value()->focused_window()) {
            Value fw = Value::object();
            fw.set("id", static_cast<long long>(f.value().id));
            fw.set("title", f.value().title);
            fw.set("app", f.value().app_name);
            fw.set("bounds", rect_json(f.value().bounds));
            out.set("focused_window", fw);
        }
    }

    r.ok = true;
    r.value = out;
    r.text = std::to_string(tree.value().interactive.size()) + " interactive elements in " +
             std::to_string(tree.value().elapsed.count()) + "ms:\n" + text;
    return r;
}

ActionResult act_move(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto target = resolve_target(s, args);
    if (!target) return fail(target.error());
    if (auto st = in.value()->move(target.value(), motion_from(args)); !st) return fail(st.error());
    Value v = Value::object();
    v.set("at", point_json(target.value()));
    return succeed("Moved pointer.", v);
}

ActionResult act_click(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto target = resolve_target(s, args);
    if (!target) return fail(target.error());

    ClickOptions o;
    auto b = button_from_string(args["button"].as_string("left"));
    if (!b) return fail(b.error());
    o.button = b.value();
    o.count = static_cast<int>(args["count"].as_int(1));
    if (o.count < 0 || o.count > 3) {
        return fail(ErrorCode::InvalidArgument, "count must be 0 (hover), 1, 2 or 3");
    }
    o.modifiers = parse_modifiers(args["modifiers"]);
    if (args.contains("press_ms")) o.press_duration = ms(args["press_ms"], 12);

    if (auto st = in.value()->click(target.value(), o); !st) return fail(st.error());

    Value v = Value::object();
    v.set("at", point_json(target.value()));
    v.set("count", o.count);
    return succeed(o.count == 0 ? "Hovered." : "Clicked.", v);
}

ActionResult act_scroll(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());

    Point at;
    if (args.contains("at") || args.contains("label")) {
        auto t = resolve_target(s, args);
        if (!t) return fail(t.error());
        at = t.value();
    } else {
        auto cur = in.value()->cursor_position();
        if (!cur) return fail(cur.error());
        at = cur.value();
    }

    ScrollOptions o;
    const std::string dir = args["direction"].as_string("down");
    if (dir == "up") {
        o.direction = ScrollDirection::Up;
        o.axis = ScrollAxis::Vertical;
    } else if (dir == "down") {
        o.direction = ScrollDirection::Down;
        o.axis = ScrollAxis::Vertical;
    } else if (dir == "left") {
        o.direction = ScrollDirection::Left;
        o.axis = ScrollAxis::Horizontal;
    } else if (dir == "right") {
        o.direction = ScrollDirection::Right;
        o.axis = ScrollAxis::Horizontal;
    } else
        return fail(ErrorCode::InvalidArgument, "direction must be up, down, left or right");

    o.clicks = static_cast<int>(args["clicks"].as_int(3));
    o.pixel_units = args["pixel_units"].as_bool(false);
    o.pixels_per_click = static_cast<int>(args["pixels_per_click"].as_int(40));
    o.phased = args["phased"].as_bool(false);
    o.modifiers = parse_modifiers(args["modifiers"]);

    if (auto st = in.value()->scroll(at, o); !st) return fail(st.error());
    Value v = Value::object();
    v.set("at", point_json(at));
    v.set("direction", dir);
    v.set("clicks", o.clicks);
    return succeed("Scrolled " + dir + ".", v);
}

ActionResult act_drag(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto from = parse_point(args["from"], "from");
    if (!from) return fail(from.error());
    auto to = parse_point(args["to"], "to");
    if (!to) return fail(to.error());
    auto rf = s.resolve(from.value());
    if (!rf) return fail(rf.error());
    auto rt = s.resolve(to.value());
    if (!rt) return fail(rt.error());

    auto opts = stroke_from(args);
    if (!opts) return fail(opts.error());
    if (auto st = in.value()->drag(rf.value(), rt.value(), opts.value()); !st)
        return fail(st.error());

    Value v = Value::object();
    v.set("from", point_json(rf.value()));
    v.set("to", point_json(rt.value()));
    return succeed("Dragged.", v);
}

ActionResult act_stroke(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());

    std::vector<PathPoint> path;
    for (const auto& p : args["points"].as_array()) {
        PathPoint pp;
        auto pt = parse_point(p, "points[]");
        if (!pt) return fail(pt.error());
        auto resolved = s.resolve(pt.value());
        if (!resolved) return fail(resolved.error());
        pp.at = resolved.value();
        if (p.is_object()) {
            pp.pressure = p.contains("pressure") ? p["pressure"].as_double(1.0) : 1.0;
            pp.dwell = ms(p["dwell_ms"], 0);
        }
        path.push_back(pp);
    }
    if (path.size() < 2) {
        return fail(ErrorCode::InvalidArgument, "a stroke needs at least two points");
    }

    auto opts = stroke_from(args);
    if (!opts) return fail(opts.error());
    if (auto st = in.value()->stroke(path, opts.value()); !st) return fail(st.error());

    Value v = Value::object();
    v.set("points", static_cast<long long>(path.size()));
    return succeed("Drew a " + std::to_string(path.size()) + "-point stroke.", v);
}

ActionResult act_gesture(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto g = gesture_from(s, args);
    if (!g) return fail(g.error());

    const auto support = in.value()->gesture_support(g.value().kind, g.value().fingers);
    if (auto st = in.value()->gesture(g.value()); !st) return fail(st.error());

    Value v = Value::object();
    v.set("kind", args["kind"].as_string());
    v.set("fingers", g.value().fingers);
    v.set("at", point_json(g.value().center));
    v.set("fidelity", support.fidelity == GestureFidelity::Native ? "native" : "emulated");
    v.set("backend", support.backend);
    if (!support.note.empty()) v.set("note", support.note);

    std::string text = "Performed " + args["kind"].as_string() + " gesture";
    if (support.fidelity == GestureFidelity::Emulated) {
        text += " (emulated via " + support.backend + ")";
    }
    return succeed(text + ".", v);
}

ActionResult act_key(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto chords = parse_chord_sequence(args["keys"].as_string());
    if (!chords) return fail(chords.error());
    const int repeat = static_cast<int>(args["repeat"].as_int(1));
    for (const auto& c : chords.value()) {
        if (auto st = in.value()->tap_chord(c, repeat); !st) return fail(st.error());
    }
    return succeed("Pressed " + args["keys"].as_string() + ".");
}

ActionResult act_key_hold(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto c = parse_chord(args["keys"].as_string());
    if (!c) return fail(c.error());
    if (auto st = in.value()->hold_chord(c.value(), ms(args["duration_ms"], 500)); !st) {
        return fail(st.error());
    }
    return succeed("Held " + args["keys"].as_string() + ".");
}

ActionResult act_key_down(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto c = parse_chord(args["key"].as_string());
    if (!c) return fail(c.error());
    for (Key k : c.value().keys) {
        if (auto st = in.value()->key_down(k); !st) return fail(st.error());
    }
    return succeed("Holding " + args["key"].as_string() + ". Remember to call key_up.");
}

ActionResult act_key_up(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());
    auto c = parse_chord(args["key"].as_string());
    if (!c) return fail(c.error());
    for (Key k : c.value().keys) {
        if (auto st = in.value()->key_up(k); !st) return fail(st.error());
    }
    return succeed("Released " + args["key"].as_string() + ".");
}

ActionResult act_type(Session& s, const Value& args) {
    auto in = s.input();
    if (!in) return fail(in.error());

    if (args.contains("at") || args.contains("label")) {
        auto t = resolve_target(s, args);
        if (!t) return fail(t.error());
        ClickOptions c;
        if (auto st = in.value()->click(t.value(), c); !st) return fail(st.error());
        std::this_thread::sleep_for(std::chrono::milliseconds{60});
    }

    if (args["clear"].as_bool(false)) {
        Chord select_all;
#if defined(__APPLE__)
        select_all.modifiers = Modifier::Meta;
#else
        select_all.modifiers = Modifier::Control;
#endif
        select_all.keys.push_back(Key::A);
        if (auto st = in.value()->tap_chord(select_all, 1); !st) return fail(st.error());
        Chord del;
        del.keys.push_back(Key::Backspace);
        if (auto st = in.value()->tap_chord(del, 1); !st) return fail(st.error());
    }

    TypeOptions o;
    o.press_enter = args["enter"].as_bool(false);
    if (args.contains("cps")) o.cps = args["cps"].as_double(0);
    o.allow_clipboard_fast_path = s.config().allow_clipboard;

    const std::string text = args["text"].as_string();
    if (auto st = in.value()->type_text(text, o); !st) return fail(st.error());

    Value v = Value::object();
    v.set("characters", static_cast<long long>(text.size()));
    return succeed("Typed " + std::to_string(text.size()) + " characters.", v);
}

ActionResult act_wait(Session&, const Value& args) {
    const auto d = ms(args["ms"], 0);
    if (d.count() < 0 || d.count() > 120000) {
        return fail(ErrorCode::InvalidArgument, "ms must be between 0 and 120000");
    }
    std::this_thread::sleep_for(d);
    return succeed("Waited " + std::to_string(d.count()) + "ms.");
}

ActionResult act_wait_for(Session& s, const Value& args) {
    const std::string condition = args["condition"].as_string();
    const std::string needle = args["text"].as_string();
    const std::string window = args["window"].as_string();
    const auto timeout = ms(args["timeout_ms"], 10000);
    const auto interval = ms(args["interval_ms"], 250);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int polls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ++polls;
        bool satisfied = false;

        if (condition == "window_exists" || condition == "window_focused") {
            auto win = s.windows();
            if (win) {
                if (condition == "window_focused") {
                    auto f = win.value()->focused_window();
                    satisfied = f && fuzzy_score(window, f.value().title) >= 60;
                } else {
                    auto all = win.value()->list_windows(false);
                    if (all) {
                        for (const auto& w : all.value()) {
                            if (fuzzy_score(window, w.title) >= 60) {
                                satisfied = true;
                                break;
                            }
                        }
                    }
                }
            }
        } else {
            auto a11y = s.accessibility();
            if (!a11y) return fail(a11y.error());
            TreeOptions to;
            to.budget = std::chrono::milliseconds{800};
            to.interactive_only = false;
            auto tree = a11y.value()->snapshot(to);
            if (tree) {
                std::function<bool(const Node&)> check = [&](const Node& n) -> bool {
                    const bool text_hit =
                        !needle.empty() && (n.name.find(needle) != std::string::npos ||
                                            n.value.find(needle) != std::string::npos);
                    if (condition == "text_exists" && text_hit) return true;
                    if (condition == "element_exists" && text_hit && n.interactive) return true;
                    if (condition == "element_enabled" && text_hit && n.enabled && n.interactive)
                        return true;
                    for (const auto& c : n.children)
                        if (check(c)) return true;
                    return false;
                };
                for (const auto& root : tree.value().roots) {
                    if (check(root)) {
                        satisfied = true;
                        break;
                    }
                }
            }
        }

        if (satisfied) {
            Value v = Value::object();
            v.set("satisfied", true);
            v.set("polls", polls);
            return succeed(
                "Condition '" + condition + "' met after " + std::to_string(polls) + " polls.", v);
        }
        std::this_thread::sleep_for(interval);
    }

    Value v = Value::object();
    v.set("satisfied", false);
    v.set("polls", polls);
    ActionResult r = fail(ErrorCode::Timeout,
                          "condition '" + condition + "' was not met within " +
                              std::to_string(timeout.count()) + "ms",
                          "Take a snapshot to see the current state; the text may differ from "
                          "what was expected.");
    r.value = v;
    return r;
}

ActionResult act_windows(Session& s, const Value& args) {
    auto win = s.windows();
    if (!win) return fail(win.error());
    const std::string mode = args["mode"].as_string("list");

    auto resolve_id = [&]() -> Result<std::uint64_t> {
        if (args.contains("window_id"))
            return static_cast<std::uint64_t>(args["window_id"].as_int(0));
        const std::string title = args["title"].as_string();
        if (title.empty()) {
            return err(ErrorCode::InvalidArgument, "provide window_id or title");
        }
        auto all = win.value()->list_windows(true);
        if (!all) return all.error();
        const WindowInfo* best = nullptr;
        int best_score = 0;
        for (const auto& w : all.value()) {
            const int sc = std::max(fuzzy_score(title, w.title), fuzzy_score(title, w.app_name));
            if (sc > best_score) {
                best_score = sc;
                best = &w;
            }
        }
        if (!best || best_score < 55) {
            return err(ErrorCode::NotFound, "no window matches '" + title + "'");
        }
        return best->id;
    };

    if (mode == "list" || mode == "focused") {
        if (mode == "focused") {
            auto f = win.value()->focused_window();
            if (!f) return fail(f.error());
            Value v = Value::object();
            v.set("id", static_cast<long long>(f.value().id));
            v.set("title", f.value().title);
            v.set("app", f.value().app_name);
            v.set("pid", f.value().pid);
            v.set("bounds", rect_json(f.value().bounds));
            return succeed(f.value().app_name + " - " + f.value().title, v);
        }
        auto all = win.value()->list_windows(args["include_offscreen"].as_bool(false));
        if (!all) return fail(all.error());
        Value arr = Value::array();
        std::string text;
        for (const auto& w : all.value()) {
            Value v = Value::object();
            v.set("id", static_cast<long long>(w.id));
            v.set("title", w.title);
            v.set("app", w.app_name);
            v.set("pid", w.pid);
            v.set("bounds", rect_json(w.bounds));
            v.set("focused", w.focused);
            v.set("display", w.display_index);
            arr.push_back(v);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%-10llu ", static_cast<unsigned long long>(w.id));
            text += buf;
            text += text::pad_utf8(w.app_name, 18) + " " + text::pad_utf8(w.title, 34);
            std::snprintf(buf, sizeof(buf), " %.0fx%.0f at (%.0f,%.0f)%s\n", w.bounds.w, w.bounds.h,
                          w.bounds.x, w.bounds.y, w.focused ? "  *focused" : "");
            text += buf;
        }
        Value out = Value::object();
        out.set("windows", arr);
        return succeed(text, out);
    }

    auto id = resolve_id();
    if (!id) return fail(id.error());

    if (mode == "activate") {
        if (auto st = win.value()->activate(id.value()); !st) return fail(st.error());
        return succeed("Activated window " + std::to_string(id.value()) + ".");
    }
    if (mode == "bounds") {
        auto r = parse_rect(args["bounds"], "bounds");
        if (!r) return fail(r.error());
        if (auto st = win.value()->set_bounds(id.value(), r.value()); !st) return fail(st.error());
        return succeed("Set window bounds.");
    }
    if (mode == "state") {
        const std::string st_name = args["state"].as_string("normal");
        WindowState ws = WindowState::Normal;
        if (st_name == "minimized")
            ws = WindowState::Minimized;
        else if (st_name == "maximized")
            ws = WindowState::Maximized;
        else if (st_name == "fullscreen")
            ws = WindowState::Fullscreen;
        else if (st_name == "hidden")
            ws = WindowState::Hidden;
        else if (st_name != "normal") {
            return fail(ErrorCode::InvalidArgument, "unknown window state '" + st_name + "'");
        }
        if (auto st = win.value()->set_state(id.value(), ws); !st) return fail(st.error());
        return succeed("Set window state to " + st_name + ".");
    }
    if (mode == "close") {
        if (auto st = win.value()->close_window(id.value()); !st) return fail(st.error());
        return succeed("Closed window.");
    }
    return fail(ErrorCode::InvalidArgument, "unknown windows mode '" + mode + "'");
}

ActionResult act_app(Session& s, const Value& args) {
    auto win = s.windows();
    if (!win) return fail(win.error());
    const std::string mode = args["mode"].as_string("list");

    if (mode == "list") {
        auto apps = win.value()->list_apps();
        if (!apps) return fail(apps.error());
        Value arr = Value::array();
        std::string text;
        for (const auto& a : apps.value()) {
            Value v = Value::object();
            v.set("pid", a.pid);
            v.set("name", a.name);
            v.set("bundle_id", a.bundle_id);
            v.set("active", a.active);
            v.set("windows", a.window_count);
            arr.push_back(v);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%-7lld ", static_cast<long long>(a.pid));
            text += buf;
            text += text::pad_utf8(a.name, 28);
            std::snprintf(buf, sizeof(buf), " %d window(s)%s\n", a.window_count,
                          a.active ? "  *active" : "");
            text += buf;
        }
        Value out = Value::object();
        out.set("apps", arr);
        return succeed(text, out);
    }
    if (mode == "launch") {
        LaunchRequest req;
        req.name = args["name"].as_string();
        req.executable = args["executable"].as_string();
        req.cwd = args["cwd"].as_string();
        for (const auto& a : args["args"].as_array()) req.args.push_back(a.as_string());
        req.timeout = ms(args["timeout_ms"], 8000);
        if (req.name.empty() && req.executable.empty()) {
            return fail(ErrorCode::InvalidArgument, "provide name or executable");
        }
        auto info = win.value()->launch(req);
        if (!info) return fail(info.error());
        Value v = Value::object();
        v.set("pid", info.value().pid);
        v.set("name", info.value().name);
        v.set("bundle_id", info.value().bundle_id);
        v.set("windows", info.value().window_count);
        return succeed(
            "Launched " + info.value().name + " (pid " + std::to_string(info.value().pid) + ").",
            v);
    }
    if (mode == "activate") {
        const std::string name = args["name"].as_string();
        if (name.empty()) return fail(ErrorCode::InvalidArgument, "provide name");
        if (auto st = win.value()->activate_app(name); !st) return fail(st.error());
        return succeed("Activated " + name + ".");
    }
    if (mode == "quit") {
        const auto pid = args["pid"].as_int(0);
        if (pid <= 0) return fail(ErrorCode::InvalidArgument, "provide pid");
        if (auto st = win.value()->quit_app(pid, args["force"].as_bool(false)); !st) {
            return fail(st.error());
        }
        return succeed("Quit pid " + std::to_string(pid) + ".");
    }
    return fail(ErrorCode::InvalidArgument, "unknown app mode '" + mode + "'");
}

ActionResult act_elements(Session& s, const Value& args) {
    auto a11y = s.accessibility();
    if (!a11y) return fail(a11y.error());
    const std::string mode = args["mode"].as_string("tree");

    if (mode == "at") {
        auto p = parse_point(args["at"], "at");
        if (!p) return fail(p.error());
        auto resolved = s.resolve(p.value());
        if (!resolved) return fail(resolved.error());
        auto n = a11y.value()->element_at(resolved.value());
        if (!n) return fail(n.error());
        return succeed(std::string(to_string(n.value().role)) + " \"" + n.value().name + "\"",
                       node_json(n.value(), false));
    }
    if (mode == "focused") {
        auto n = a11y.value()->focused_element();
        if (!n) return fail(n.error());
        return succeed(std::string(to_string(n.value().role)) + " \"" + n.value().name + "\"",
                       node_json(n.value(), false));
    }

    auto tree = a11y.value()->snapshot(tree_from(args));
    if (!tree) return fail(tree.error());

    auto& cache = label_cache();
    cache.nodes.clear();
    cache.taken = std::chrono::steady_clock::now();
    for (const Node* n : tree.value().interactive) cache.nodes.push_back(*n);

    Value arr = Value::array();
    for (const auto& root : tree.value().roots) arr.push_back(node_json(root, true));
    Value out = Value::object();
    out.set("roots", arr);
    out.set("element_count", static_cast<long long>(tree.value().interactive.size()));
    out.set("elapsed_ms", static_cast<long long>(tree.value().elapsed.count()));
    return succeed(std::to_string(tree.value().interactive.size()) + " interactive elements.", out);
}

ActionResult act_clipboard(Session& s, const Value& args) {
    if (!s.config().allow_clipboard) {
        return fail(ErrorCode::PermissionDenied, "clipboard access is disabled for this session",
                    "Restart without --no-clipboard.");
    }
    auto sys = s.system();
    if (!sys) return fail(sys.error());
    const std::string mode = args["mode"].as_string("get");

    if (mode == "get") {
        auto c = sys.value()->clipboard_get();
        if (!c) return fail(c.error());
        Value v = Value::object();
        v.set("text", c.value().text);
        v.set("has_image", c.value().has_image);
        Value files = Value::array();
        for (const auto& f : c.value().file_paths) files.push_back(f);
        v.set("files", files);
        return succeed(c.value().text, v);
    }
    if (mode == "set") {
        ClipboardContent c;
        c.text = args["text"].as_string();
        c.has_text = true;
        if (auto st = sys.value()->clipboard_set(c); !st) return fail(st.error());
        return succeed("Clipboard set.");
    }
    return fail(ErrorCode::InvalidArgument, "clipboard mode must be get or set");
}

ActionResult act_shell(Session& s, const Value& args) {
    if (!s.config().allow_shell) {
        return fail(ErrorCode::PermissionDenied, "shell execution is disabled for this session",
                    "Restart the server without --no-shell if you intend to allow it.");
    }
    auto sys = s.system();
    if (!sys) return fail(sys.error());

    ShellRequest req;
    req.command = args["command"].as_string();
    req.shell = args["shell"].as_string();
    req.cwd = args["cwd"].as_string();
    req.timeout = ms(args["timeout_ms"], 30000);
    if (req.command.empty()) return fail(ErrorCode::InvalidArgument, "command is required");

    auto r = sys.value()->run_shell(req);
    if (!r) return fail(r.error());

    Value v = Value::object();
    v.set("exit_code", r.value().exit_code);
    v.set("stdout", r.value().stdout_text);
    v.set("stderr", r.value().stderr_text);
    v.set("timed_out", r.value().timed_out);
    v.set("elapsed_ms", static_cast<long long>(r.value().elapsed.count()));

    std::string text = r.value().stdout_text;
    if (!r.value().stderr_text.empty()) text += "\n[stderr]\n" + r.value().stderr_text;
    if (r.value().timed_out)
        text += "\n[timed out after " + std::to_string(req.timeout.count()) + "ms]";

    ActionResult out = succeed(text, v);
    // A non-zero exit is reported faithfully but is not a tool failure: the
    // caller asked to run a command and the command ran.
    return out;
}

ActionResult act_process(Session& s, const Value& args) {
    auto sys = s.system();
    if (!sys) return fail(sys.error());
    const std::string mode = args["mode"].as_string("list");

    if (mode == "list") {
        auto procs = sys.value()->list_processes();
        if (!procs) return fail(procs.error());
        auto list = procs.value();
        const std::string filter = args["name"].as_string();
        if (!filter.empty()) {
            list.erase(std::remove_if(
                           list.begin(), list.end(),
                           [&](const ProcessInfo& p) { return fuzzy_score(filter, p.name) < 60; }),
                       list.end());
        }
        const std::string sort = args["sort"].as_string("memory");
        std::sort(list.begin(), list.end(), [&](const ProcessInfo& a, const ProcessInfo& b) {
            if (sort == "name") return a.name < b.name;
            if (sort == "pid") return a.pid < b.pid;
            return a.memory_bytes > b.memory_bytes;
        });
        const auto limit =
            static_cast<std::size_t>(std::max<long long>(1, args["limit"].as_int(30)));
        if (list.size() > limit) list.resize(limit);

        Value arr = Value::array();
        std::string text;
        for (const auto& p : list) {
            Value v = Value::object();
            v.set("pid", p.pid);
            v.set("name", p.name);
            v.set("memory_mb", p.memory_bytes / 1048576.0);
            arr.push_back(v);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%-7lld ", static_cast<long long>(p.pid));
            text += buf;
            text += text::pad_utf8(p.name, 32);
            std::snprintf(buf, sizeof(buf), " %8.1f MB\n", p.memory_bytes / 1048576.0);
            text += buf;
        }
        Value out = Value::object();
        out.set("processes", arr);
        return succeed(text, out);
    }
    if (mode == "kill") {
        const auto pid = args["pid"].as_int(0);
        if (pid <= 0) return fail(ErrorCode::InvalidArgument, "provide pid");
        if (auto st = sys.value()->kill_process(pid, args["force"].as_bool(false)); !st) {
            return fail(st.error());
        }
        return succeed("Signalled pid " + std::to_string(pid) + ".");
    }
    return fail(ErrorCode::InvalidArgument, "process mode must be list or kill");
}

ActionResult act_notify(Session& s, const Value& args) {
    auto sys = s.system();
    if (!sys) return fail(sys.error());
    NotificationRequest req;
    req.message = args["message"].as_string();
    req.title = args["title"].as_string("computer-control");
    req.subtitle = args["subtitle"].as_string();
    req.sound = args["sound"].as_string();
    if (auto st = sys.value()->notify(req); !st) return fail(st.error());
    return succeed("Notification sent.");
}

ActionResult act_registry(Session& s, const Value& args) {
    if (!s.config().allow_registry) {
        return fail(ErrorCode::PermissionDenied, "registry access is disabled for this session",
                    "Restart the server with --allow-registry.");
    }
    auto sys = s.system();
    if (!sys) return fail(sys.error());

    const std::string mode = args["mode"].as_string();
    const std::string path = args["path"].as_string();
    const std::string name = args["name"].as_string();

    if (mode == "get") {
        auto v = sys.value()->registry_get(path, name);
        if (!v) return fail(v.error());
        Value out = Value::object();
        out.set("value", v.value());
        return succeed(v.value(), out);
    }
    if (mode == "set") {
        auto st = sys.value()->registry_set(path, name, args["value"].as_string(),
                                            args["type"].as_string("String"));
        if (!st) return fail(st.error());
        return succeed("Registry value set.");
    }
    if (mode == "delete") {
        auto st = sys.value()->registry_delete(path, name);
        if (!st) return fail(st.error());
        return succeed("Registry value deleted.");
    }
    if (mode == "list") {
        auto v = sys.value()->registry_list(path);
        if (!v) return fail(v.error());
        Value arr = Value::array();
        std::string text;
        for (const auto& e : v.value()) {
            arr.push_back(e);
            text += e + "\n";
        }
        Value out = Value::object();
        out.set("entries", arr);
        return succeed(text, out);
    }
    return fail(ErrorCode::InvalidArgument, "registry mode must be get, set, delete or list");
}

ActionResult act_release_all(Session& s, const Value&) {
    if (auto st = s.release_all(); !st) return fail(st.error());
    return succeed("Released all held buttons and keys.");
}

}  // namespace

// Device actions live in their own function for size.
ActionResult act_device(Session& s, const Value& args);

ActionResult run(Session& session, std::string_view name, const Value& args) {
    const ActionSpec* spec = find_spec(name);
    if (!spec) {
        std::string known;
        for (const auto& sp : registry()) known += std::string(sp.name) + " ";
        return fail(ErrorCode::NotFound, "unknown action '" + std::string(name) + "'",
                    "Known actions: " + known);
    }

    // Exceptions must never cross this boundary. A throw reaching the stdio
    // loop takes the MCP session down with no diagnostic the client can show,
    // and a bad JSON shape should be an error response, not a dead server.
    try {
        if (name == "system") return act_system(session, args);
        if (name == "permissions") return act_permissions(session, args);
        if (name == "capabilities") return act_capabilities(session, args);
        if (name == "displays") return act_displays(session, args);
        if (name == "screenshot") return do_capture(session, args, true);
        if (name == "snapshot") return act_snapshot(session, args);
        if (name == "zoom") {
            Value a = args;
            a.set("max_dimension", 0);  // full resolution, that is the point
            return do_capture(session, a, true);
        }
        if (name == "cursor_position") {
            auto in = session.input();
            if (!in) return fail(in.error());
            auto p = in.value()->cursor_position();
            if (!p) return fail(p.error());
            char buf[96];
            std::snprintf(buf, sizeof(buf), "(%.0f, %.0f)", p.value().x, p.value().y);
            return succeed(buf, point_json(p.value()));
        }
        if (name == "move") return act_move(session, args);
        if (name == "click") return act_click(session, args);
        if (name == "scroll") return act_scroll(session, args);
        if (name == "drag") return act_drag(session, args);
        if (name == "stroke") return act_stroke(session, args);
        if (name == "gesture") return act_gesture(session, args);
        if (name == "key") return act_key(session, args);
        if (name == "key_hold") return act_key_hold(session, args);
        if (name == "key_down") return act_key_down(session, args);
        if (name == "key_up") return act_key_up(session, args);
        if (name == "type") return act_type(session, args);
        if (name == "wait") return act_wait(session, args);
        if (name == "wait_for") return act_wait_for(session, args);
        if (name == "windows") return act_windows(session, args);
        if (name == "app") return act_app(session, args);
        if (name == "elements") return act_elements(session, args);
        if (name == "clipboard") return act_clipboard(session, args);
        if (name == "shell") return act_shell(session, args);
        if (name == "process") return act_process(session, args);
        if (name == "notify") return act_notify(session, args);
        if (name == "registry") return act_registry(session, args);
        if (name == "device") return act_device(session, args);
        if (name == "release_all") return act_release_all(session, args);
        if (name == "batch") return run_batch(session, args["actions"]);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal,
                    std::string("action '") + std::string(name) + "' threw: " + e.what());
    } catch (...) {
        return fail(ErrorCode::Internal,
                    std::string("action '") + std::string(name) + "' threw an unknown exception");
    }
    return fail(ErrorCode::Internal, "action '" + std::string(name) + "' has no implementation");
}

ActionResult run_batch(Session& session, const Value& actions) {
    if (!actions.is_array()) {
        return fail(ErrorCode::InvalidArgument, "batch actions must be an array");
    }

    Value results = Value::array();
    std::string text;
    ActionResult last_image;

    const auto& list = actions.as_array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Value& step = list[i];
        const std::string action = step["action"].as_string();
        if (action.empty()) {
            ActionResult r = fail(ErrorCode::InvalidArgument,
                                  "batch step " + std::to_string(i) + " has no \"action\" field");
            r.value = results;
            return r;
        }

        ActionResult one = run(session, action, step);
        Value entry = Value::object();
        entry.set("index", static_cast<long long>(i));
        entry.set("action", action);
        entry.set("ok", one.ok);
        if (!one.text.empty()) entry.set("text", one.text);
        if (!one.value.is_null()) entry.set("result", one.value);
        if (!one.ok) {
            entry.set("error", one.error.message);
            if (!one.error.remedy.empty()) entry.set("remedy", one.error.remedy);
        }
        results.push_back(entry);
        text += "[" + std::to_string(i) + "] " + action + ": " +
                (one.ok ? one.text : ("FAILED - " + one.error.message)) + "\n";

        // Keep the last image so a batch ending in a screenshot still returns
        // one; an agent batching "click, wait, screenshot" wants the picture.
        if (!one.image.empty()) {
            last_image.image = std::move(one.image);
            last_image.image_mime = one.image_mime;
        }

        if (!one.ok) {
            ActionResult r;
            r.ok = false;
            r.error = Error{one.error.code,
                            "batch stopped at step " + std::to_string(i) + " (" + action +
                                "): " + one.error.message,
                            one.error.remedy};
            Value out = Value::object();
            out.set("steps", results);
            out.set("failed_at", static_cast<long long>(i));
            r.value = out;
            r.text = text;
            r.image = std::move(last_image.image);
            r.image_mime = last_image.image_mime;
            return r;
        }
    }

    ActionResult r;
    r.ok = true;
    Value out = Value::object();
    out.set("steps", results);
    out.set("completed", static_cast<long long>(list.size()));
    r.value = out;
    r.text = text;
    r.image = std::move(last_image.image);
    r.image_mime = last_image.image_mime;
    return r;
}

}  // namespace cc::actions
