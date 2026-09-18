// SPDX-License-Identifier: MIT
//
// The C ABI. Every language binding goes through here.
//
// Two invariants make this safe to call from a garbage-collected runtime:
//   1. Nothing throws. Every entry point is wrapped, and a C++ exception
//      escaping into a Python or Node frame is undefined behaviour.
//   2. Error detail is thread-local. A binding may drive several sessions from
//      several threads, and a shared error slot would race.

#include "cc/capi.h"

#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>

#include "capi/actions.hpp"
#include "cc/session.hpp"
#include "core/json.hpp"

using namespace cc;

namespace {

struct ThreadError {
    cc_status_t code = CC_OK;
    std::string message;
    std::string remedy;
};

ThreadError& tls_error() {
    thread_local ThreadError e;
    return e;
}

cc_status_t record(const Error& e) {
    ThreadError& t = tls_error();
    t.code = static_cast<cc_status_t>(e.code);
    t.message = e.message;
    t.remedy = e.remedy;
    return t.code;
}

cc_status_t clear_error() {
    ThreadError& t = tls_error();
    t.code = CC_OK;
    t.message.clear();
    t.remedy.clear();
    return CC_OK;
}

cc_status_t record(cc_status_t code, std::string message, std::string remedy = {}) {
    ThreadError& t = tls_error();
    t.code = code;
    t.message = std::move(message);
    t.remedy = std::move(remedy);
    return code;
}

char* dup_string(const std::string& s) {
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

std::uint8_t* dup_bytes(const std::vector<std::uint8_t>& b) {
    if (b.empty()) return nullptr;
    auto* p = static_cast<std::uint8_t*>(std::malloc(b.size()));
    if (!p) return nullptr;
    std::memcpy(p, b.data(), b.size());
    return p;
}

// Reads a versioned struct safely: a caller compiled against an older header
// passes a smaller `size`, and everything past it keeps the default.
template <typename T>
bool field_present(const T* s, std::size_t offset_end) {
    return s && s->size >= offset_end;
}

}  // namespace

struct cc_session {
    std::shared_ptr<Session> session;
    std::mutex mu;
    std::map<std::string, std::shared_ptr<DeviceSession>> devices;
    int next_handle = 1;
};

// Guard for every entry point. Turns any escaping exception into an error code
// instead of terminating the host process.
#define CC_GUARD_BEGIN try {
#define CC_GUARD_END                                                           \
    }                                                                          \
    catch (const std::bad_alloc&) {                                            \
        return record(CC_ERR_INTERNAL, "out of memory");                       \
    }                                                                          \
    catch (const std::exception& e) {                                          \
        return record(CC_ERR_INTERNAL, std::string("exception: ") + e.what()); \
    }                                                                          \
    catch (...) {                                                              \
        return record(CC_ERR_INTERNAL, "unknown exception");                   \
    }

namespace {

cc_rect_t to_c(const Rect& r) {
    return cc_rect_t{r.x, r.y, r.w, r.h, static_cast<cc_space_t>(r.space)};
}
Point from_c(const cc_point_t& p) {
    return Point{p.x, p.y, static_cast<Space>(p.space)};
}
Rect from_c(const cc_rect_t& r) {
    return Rect{r.x, r.y, r.w, r.h, static_cast<Space>(r.space)};
}
MotionOptions from_c(const cc_motion_options_t* o) {
    MotionOptions m;
    if (!o) return m;
    m.profile = static_cast<MotionProfile>(o->profile);
    m.duration = std::chrono::milliseconds{o->duration_ms};
    if (o->rate_hz > 0) m.rate_hz = o->rate_hz;
    if (o->max_steps > 0) m.max_steps = o->max_steps;
    m.jitter_px = o->jitter_px;
    m.overshoot_px = o->overshoot_px;
    m.seed = o->seed;
    return m;
}

ClickOptions from_c(const cc_click_options_t* o) {
    ClickOptions c;
    if (!o) return c;
    c.button = static_cast<MouseButton>(o->button);
    c.count = o->count;
    c.modifiers = static_cast<Modifier>(o->modifiers);
    if (o->press_duration_ms > 0)
        c.press_duration = std::chrono::milliseconds{o->press_duration_ms};
    if (o->inter_click_ms > 0) c.inter_click = std::chrono::milliseconds{o->inter_click_ms};
    c.move_first = o->move_first != 0;
    return c;
}

ScrollOptions from_c(const cc_scroll_options_t* o) {
    ScrollOptions s;
    if (!o) return s;
    s.axis = static_cast<ScrollAxis>(o->axis);
    s.direction = static_cast<ScrollDirection>(o->direction);
    s.clicks = o->clicks;
    s.modifiers = static_cast<Modifier>(o->modifiers);
    s.pixel_units = o->pixel_units != 0;
    if (o->pixels_per_click > 0) s.pixels_per_click = o->pixels_per_click;
    s.phased = o->phased != 0;
    return s;
}

StrokeOptions from_c(const cc_stroke_options_t* o) {
    StrokeOptions s;
    if (!o) return s;
    s.button = static_cast<MouseButton>(o->button);
    s.modifiers = static_cast<Modifier>(o->modifiers);
    s.motion = from_c(&o->motion);
    s.smooth = o->smooth != 0;
    s.smooth_tension = o->smooth_tension;
    s.settle_before_release = std::chrono::milliseconds{o->settle_before_release_ms};
    s.use_pen = o->use_pen != 0;
    return s;
}
CaptureOptions from_c(const cc_capture_options_t* o) {
    CaptureOptions c;
    if (!o) return c;
    if (o->use_display) c.display_index = o->display_index;
    if (o->use_window) c.window_id = o->window_id;
    if (o->use_region) c.region = from_c(o->region);
    c.max_dimension = o->max_dimension;
    c.scale = (o->scale > 0) ? o->scale : 1.0;
    c.include_cursor = o->include_cursor != 0;
    return c;
}
cc_status_t run_action_json(cc_session_t* s, const char* action, const std::string& args_json,
                            char** out_json) {
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    json::ParseError pe;
    json::Value args = args_json.empty() ? json::Value::object() : json::parse(args_json, &pe);
    if (!pe.ok)
        return record(CC_ERR_INVALID_ARGUMENT, "arguments are not valid JSON: " + pe.message);

    auto result = actions::run(*s->session, action, args);
    if (!result.ok) return record(result.error);
    if (out_json) {
        json::Value out = json::Value::object();
        out.set("ok", true);
        out.set("text", result.text);
        out.set("result", result.value);
        *out_json = dup_string(out.dump());
        if (!*out_json) return record(CC_ERR_INTERNAL, "out of memory");
    }
    return clear_error();
}

std::string json_obj(std::initializer_list<std::pair<const char*, json::Value>> fields) {
    json::Value v = json::Value::object();
    for (const auto& [k, val] : fields) v.set(k, val);
    return v.dump();
}
std::shared_ptr<DeviceSession> lookup(cc_session_t* s, const char* handle) {
    if (!s || !handle) return nullptr;
    std::lock_guard<std::mutex> lk(s->mu);
    auto it = s->devices.find(handle);
    return it == s->devices.end() ? nullptr : it->second;
}

}  // namespace

extern "C" {

// --- diagnostics -----------------------------------------------------------

cc_status_t cc_last_error_code(void) {
    return tls_error().code;
}
const char* cc_last_error_message(void) {
    return tls_error().message.c_str();
}
const char* cc_last_error_remedy(void) {
    return tls_error().remedy.c_str();
}

void cc_string_free(char* s) {
    std::free(s);
}
void cc_buffer_free(std::uint8_t* p) {
    std::free(p);
}

std::int32_t cc_abi_version(void) {
    return CC_ABI_VERSION;
}
const char* cc_version(void) {
    return build_info().version;
}
const char* cc_platform(void) {
    return build_info().platform;
}

// --- session ---------------------------------------------------------------

void cc_session_config_default(cc_session_config_t* out) {
    if (!out) return;
    const SessionConfig d{};
    out->size = sizeof(cc_session_config_t);
    out->eager_init = d.eager_init;
    out->prompt_for_permissions = d.prompt_for_permissions;
    out->allow_shell = d.allow_shell;
    out->allow_filesystem = d.allow_filesystem;
    out->allow_registry = d.allow_registry;
    out->allow_clipboard = d.allow_clipboard;
    out->block_when_locked = d.block_when_locked;
    out->default_max_capture_dimension = d.default_max_capture_dimension;
}

cc_status_t cc_session_create(const cc_session_config_t* cfg, cc_session_t** out) {
    CC_GUARD_BEGIN
    if (!out) return record(CC_ERR_INVALID_ARGUMENT, "out must not be null");
    *out = nullptr;

    SessionConfig sc;
    if (cfg) {
        if (field_present(cfg, offsetof(cc_session_config_t, eager_init) + sizeof(std::int32_t)))
            sc.eager_init = cfg->eager_init != 0;
        if (field_present(
                cfg, offsetof(cc_session_config_t, prompt_for_permissions) + sizeof(std::int32_t)))
            sc.prompt_for_permissions = cfg->prompt_for_permissions != 0;
        if (field_present(cfg, offsetof(cc_session_config_t, allow_shell) + sizeof(std::int32_t)))
            sc.allow_shell = cfg->allow_shell != 0;
        if (field_present(cfg,
                          offsetof(cc_session_config_t, allow_filesystem) + sizeof(std::int32_t)))
            sc.allow_filesystem = cfg->allow_filesystem != 0;
        if (field_present(cfg,
                          offsetof(cc_session_config_t, allow_registry) + sizeof(std::int32_t)))
            sc.allow_registry = cfg->allow_registry != 0;
        if (field_present(cfg,
                          offsetof(cc_session_config_t, allow_clipboard) + sizeof(std::int32_t)))
            sc.allow_clipboard = cfg->allow_clipboard != 0;
        if (field_present(cfg,
                          offsetof(cc_session_config_t, block_when_locked) + sizeof(std::int32_t)))
            sc.block_when_locked = cfg->block_when_locked != 0;
        if (field_present(cfg, offsetof(cc_session_config_t, default_max_capture_dimension) +
                                   sizeof(std::int32_t)))
            sc.default_max_capture_dimension = cfg->default_max_capture_dimension;
    }

    auto s = Session::create(sc);
    if (!s) return record(s.error());

    auto* handle = new cc_session();
    handle->session = s.value();
    *out = handle;
    return clear_error();
    CC_GUARD_END
}

void cc_session_destroy(cc_session_t* s) {
    if (!s) return;
    // The Session destructor releases held buttons and keys, so this is the
    // cleanup path a binding's __del__ or FinalizationRegistry relies on.
    delete s;
}

cc_status_t cc_session_release_all(cc_session_t* s) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto st = s->session->release_all();
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_session_capabilities(cc_session_t* s, char** out_json) {
    CC_GUARD_BEGIN
    if (!s || !out_json) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    *out_json = dup_string(s->session->capability_report());
    return *out_json ? clear_error() : record(CC_ERR_INTERNAL, "out of memory");
    CC_GUARD_END
}

// --- displays --------------------------------------------------------------

cc_status_t cc_display_refresh(cc_session_t* s) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto d = s->session->displays();
    if (!d) return record(d.error());
    auto st = d.value()->refresh();
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_display_count(cc_session_t* s, std::int32_t* out_count) {
    CC_GUARD_BEGIN
    if (!s || !out_count) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto d = s->session->displays();
    if (!d) return record(d.error());
    *out_count = static_cast<std::int32_t>(d.value()->displays().size());
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_display_get(cc_session_t* s, std::int32_t index, cc_display_t* out) {
    CC_GUARD_BEGIN
    if (!s || !out) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto d = s->session->displays();
    if (!d) return record(d.error());
    const Display* disp = d.value()->by_index(index);
    if (!disp) return record(CC_ERR_NOT_FOUND, "no display at index " + std::to_string(index));

    out->size = sizeof(cc_display_t);
    out->index = disp->index;
    out->id = disp->id;
    std::snprintf(out->name, sizeof(out->name), "%s", disp->name.c_str());
    out->bounds_logical = to_c(disp->bounds_logical);
    out->bounds_physical = to_c(disp->bounds_physical);
    out->work_area_logical = to_c(disp->work_area_logical);
    out->scale = disp->scale;
    out->dpi = disp->dpi;
    out->refresh_hz = disp->refresh_hz;
    out->orientation = static_cast<std::int32_t>(disp->orientation);
    out->primary = disp->primary ? 1 : 0;
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_display_virtual_bounds(cc_session_t* s, cc_space_t space, cc_rect_t* out) {
    CC_GUARD_BEGIN
    if (!s || !out) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto d = s->session->displays();
    if (!d) return record(d.error());
    *out = to_c(d.value()->virtual_bounds(static_cast<Space>(space)));
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_convert_point(cc_session_t* s, cc_point_t in, cc_space_t to, cc_point_t* out) {
    CC_GUARD_BEGIN
    if (!s || !out) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto d = s->session->displays();
    if (!d) return record(d.error());
    const Point p = d.value()->convert(from_c(in), static_cast<Space>(to));
    *out = cc_point_t{p.x, p.y, static_cast<cc_space_t>(p.space)};
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_convert_rect(cc_session_t* s, cc_rect_t in, cc_space_t to, cc_rect_t* out) {
    CC_GUARD_BEGIN
    if (!s || !out) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto d = s->session->displays();
    if (!d) return record(d.error());
    *out = to_c(d.value()->convert(from_c(in), static_cast<Space>(to)));
    return clear_error();
    CC_GUARD_END
}

// --- option defaults -------------------------------------------------------

void cc_motion_options_default(cc_motion_options_t* o) {
    if (!o) return;
    const MotionOptions d{};
    o->size = sizeof(*o);
    o->profile = static_cast<cc_motion_profile_t>(d.profile);
    o->duration_ms = static_cast<std::int32_t>(d.duration.count());
    o->rate_hz = d.rate_hz;
    o->max_steps = d.max_steps;
    o->jitter_px = d.jitter_px;
    o->overshoot_px = d.overshoot_px;
    o->seed = d.seed;
}

void cc_click_options_default(cc_click_options_t* o) {
    if (!o) return;
    const ClickOptions d{};
    o->size = sizeof(*o);
    o->button = static_cast<cc_button_t>(d.button);
    o->count = d.count;
    o->modifiers = static_cast<std::uint32_t>(d.modifiers);
    o->press_duration_ms = static_cast<std::int32_t>(d.press_duration.count());
    o->inter_click_ms = static_cast<std::int32_t>(d.inter_click.count());
    o->move_first = d.move_first;
}

void cc_scroll_options_default(cc_scroll_options_t* o) {
    if (!o) return;
    const ScrollOptions d{};
    o->size = sizeof(*o);
    o->axis = static_cast<cc_scroll_axis_t>(d.axis);
    o->direction = static_cast<cc_direction_t>(d.direction);
    o->clicks = d.clicks;
    o->modifiers = static_cast<std::uint32_t>(d.modifiers);
    o->pixel_units = d.pixel_units;
    o->pixels_per_click = d.pixels_per_click;
    o->phased = d.phased;
}

void cc_stroke_options_default(cc_stroke_options_t* o) {
    if (!o) return;
    const StrokeOptions d{};
    o->size = sizeof(*o);
    o->button = static_cast<cc_button_t>(d.button);
    o->modifiers = static_cast<std::uint32_t>(d.modifiers);
    cc_motion_options_default(&o->motion);
    o->smooth = d.smooth;
    o->smooth_tension = d.smooth_tension;
    o->settle_before_release_ms = static_cast<std::int32_t>(d.settle_before_release.count());
    o->use_pen = d.use_pen;
}

void cc_type_options_default(cc_type_options_t* o) {
    if (!o) return;
    const TypeOptions d{};
    o->size = sizeof(*o);
    o->cps = d.cps;
    o->allow_clipboard_fast_path = d.allow_clipboard_fast_path;
    o->clipboard_threshold = d.clipboard_threshold;
    o->restore_clipboard = d.restore_clipboard;
    o->press_enter = d.press_enter;
    o->modifiers = static_cast<std::uint32_t>(d.modifiers);
}

void cc_gesture_default(cc_gesture_t* g) {
    if (!g) return;
    const GestureRequest d{};
    std::memset(g, 0, sizeof(*g));
    g->size = sizeof(*g);
    g->kind = static_cast<cc_gesture_kind_t>(d.kind);
    g->fingers = d.fingers;
    g->direction = CC_DIR_LEFT;
    g->distance = d.distance;
    g->scale = d.scale;
    g->spread = d.spread;
    g->pressure = d.pressure;
    g->duration_ms = static_cast<std::int32_t>(d.duration.count());
}

// --- pointer ---------------------------------------------------------------

cc_status_t cc_cursor_position(cc_session_t* s, cc_point_t* out) {
    CC_GUARD_BEGIN
    if (!s || !out) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto p = in.value()->cursor_position();
    if (!p) return record(p.error());
    *out = cc_point_t{p.value().x, p.value().y, CC_SPACE_LOGICAL};
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_mouse_move(cc_session_t* s, cc_point_t to, const cc_motion_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto resolved = s->session->resolve(from_c(to));
    if (!resolved) return record(resolved.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->move(resolved.value(), from_c(opts));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_mouse_click(cc_session_t* s, cc_point_t at, const cc_click_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto resolved = s->session->resolve(from_c(at));
    if (!resolved) return record(resolved.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->click(resolved.value(), from_c(opts));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_mouse_click_here(cc_session_t* s, const cc_click_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->click_here(from_c(opts));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_mouse_down(cc_session_t* s, cc_button_t b, std::uint32_t modifiers) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st =
        in.value()->button_down(static_cast<MouseButton>(b), static_cast<Modifier>(modifiers));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_mouse_up(cc_session_t* s, cc_button_t b, std::uint32_t modifiers) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->button_up(static_cast<MouseButton>(b), static_cast<Modifier>(modifiers));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_scroll(cc_session_t* s, cc_point_t at, const cc_scroll_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto resolved = s->session->resolve(from_c(at));
    if (!resolved) return record(resolved.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->scroll(resolved.value(), from_c(opts));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_drag(cc_session_t* s, cc_point_t from, cc_point_t to,
                    const cc_stroke_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s) return record(CC_ERR_INVALID_ARGUMENT, "session is null");
    auto a = s->session->resolve(from_c(from));
    if (!a) return record(a.error());
    auto b = s->session->resolve(from_c(to));
    if (!b) return record(b.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->drag(a.value(), b.value(), from_c(opts));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_stroke(cc_session_t* s, const cc_path_point_t* points, std::size_t count,
                      const cc_stroke_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s || !points) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    if (count < 2) return record(CC_ERR_INVALID_ARGUMENT, "a stroke needs at least two points");

    std::vector<PathPoint> path;
    path.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto resolved = s->session->resolve(from_c(points[i].at));
        if (!resolved) return record(resolved.error());
        PathPoint p;
        p.at = resolved.value();
        p.pressure = points[i].pressure;
        p.tilt_x = points[i].tilt_x;
        p.tilt_y = points[i].tilt_y;
        p.dwell = std::chrono::milliseconds{points[i].dwell_ms};
        path.push_back(p);
    }
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->stroke(path, from_c(opts));
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

// --- keyboard --------------------------------------------------------------

cc_status_t cc_key_tap(cc_session_t* s, const char* chord, std::int32_t repeat) {
    CC_GUARD_BEGIN
    if (!s || !chord) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto chords = parse_chord_sequence(chord);
    if (!chords) return record(chords.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    for (const auto& c : chords.value()) {
        auto st = in.value()->tap_chord(c, repeat > 0 ? repeat : 1);
        if (!st) return record(st.error());
    }
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_key_hold(cc_session_t* s, const char* chord, std::int32_t duration_ms) {
    CC_GUARD_BEGIN
    if (!s || !chord) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto c = parse_chord(chord);
    if (!c) return record(c.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->hold_chord(c.value(), std::chrono::milliseconds{duration_ms});
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_key_down(cc_session_t* s, const char* key) {
    CC_GUARD_BEGIN
    if (!s || !key) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto c = parse_chord(key);
    if (!c) return record(c.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    for (Key k : c.value().keys) {
        auto st = in.value()->key_down(k);
        if (!st) return record(st.error());
    }
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_key_up(cc_session_t* s, const char* key) {
    CC_GUARD_BEGIN
    if (!s || !key) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto c = parse_chord(key);
    if (!c) return record(c.error());
    auto in = s->session->input();
    if (!in) return record(in.error());
    for (Key k : c.value().keys) {
        auto st = in.value()->key_up(k);
        if (!st) return record(st.error());
    }
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_type_text(cc_session_t* s, const char* utf8, const cc_type_options_t* opts) {
    CC_GUARD_BEGIN
    if (!s || !utf8) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    TypeOptions o;
    if (opts) {
        o.cps = opts->cps;
        o.allow_clipboard_fast_path = opts->allow_clipboard_fast_path != 0;
        if (opts->clipboard_threshold > 0) o.clipboard_threshold = opts->clipboard_threshold;
        o.restore_clipboard = opts->restore_clipboard != 0;
        o.press_enter = opts->press_enter != 0;
        o.modifiers = static_cast<Modifier>(opts->modifiers);
    }
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->type_text(utf8, o);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

// --- gestures --------------------------------------------------------------

cc_status_t cc_gesture_support(cc_session_t* s, cc_gesture_kind_t kind, std::int32_t fingers,
                               cc_gesture_support_t* out) {
    CC_GUARD_BEGIN
    if (!s || !out) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto in = s->session->input();
    if (!in) return record(in.error());
    const auto sup = in.value()->gesture_support(static_cast<GestureKind>(kind), fingers);
    out->size = sizeof(*out);
    out->fidelity = static_cast<cc_gesture_fidelity_t>(sup.fidelity);
    out->max_fingers = sup.max_fingers;
    std::snprintf(out->backend, sizeof(out->backend), "%s", sup.backend.c_str());
    std::snprintf(out->note, sizeof(out->note), "%s", sup.note.c_str());
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_gesture_perform(cc_session_t* s, const cc_gesture_t* g) {
    CC_GUARD_BEGIN
    if (!s || !g) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    GestureRequest req;
    req.kind = static_cast<GestureKind>(g->kind);
    auto center = s->session->resolve(from_c(g->center));
    if (!center) return record(center.error());
    req.center = center.value();
    req.fingers = g->fingers > 0 ? g->fingers : 2;
    req.direction = static_cast<SwipeDirection>(g->direction);
    req.distance = g->distance;
    req.scale = g->scale;
    req.rotation_degrees = g->rotation_degrees;
    req.spread = g->spread;
    req.pressure = g->pressure;
    req.duration = std::chrono::milliseconds{g->duration_ms};
    req.hold = std::chrono::milliseconds{g->hold_ms};
    req.modifiers = static_cast<Modifier>(g->modifiers);
    req.require_native = g->require_native != 0;
    if (g->path && g->path_count) {
        for (std::size_t i = 0; i < g->path_count; ++i) {
            auto p = s->session->resolve(from_c(g->path[i]));
            if (!p) return record(p.error());
            req.path.push_back(p.value());
        }
    }
    auto in = s->session->input();
    if (!in) return record(in.error());
    auto st = in.value()->gesture(req);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

// --- capture ---------------------------------------------------------------

void cc_capture_options_default(cc_capture_options_t* o) {
    if (!o) return;
    std::memset(o, 0, sizeof(*o));
    o->size = sizeof(*o);
    o->scale = 1.0;
    o->include_cursor = 1;
    o->max_dimension = 0;
}

cc_status_t cc_capture(cc_session_t* s, const cc_capture_options_t* opts, cc_image_format_t format,
                       std::int32_t quality, std::uint8_t** out_bytes, std::size_t* out_len,
                       std::int32_t* out_width, std::int32_t* out_height) {
    CC_GUARD_BEGIN
    if (!s || !out_bytes || !out_len) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    *out_bytes = nullptr;
    *out_len = 0;

    auto screen = s->session->screen();
    if (!screen) return record(screen.error());
    auto frame = screen.value()->capture(from_c(opts));
    if (!frame) return record(frame.error());

    EncodeOptions eo;
    eo.format = static_cast<ImageFormat>(format);
    eo.quality = quality > 0 ? quality : 80;
    auto bytes = encode(frame.value(), eo);
    if (!bytes) return record(bytes.error());

    *out_bytes = dup_bytes(bytes.value());
    if (!*out_bytes && !bytes.value().empty()) return record(CC_ERR_INTERNAL, "out of memory");
    *out_len = bytes.value().size();
    if (out_width) *out_width = frame.value().width;
    if (out_height) *out_height = frame.value().height;
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_capture_to_file(cc_session_t* s, const cc_capture_options_t* opts,
                               const char* path) {
    CC_GUARD_BEGIN
    if (!s || !path) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    const std::string p(path);
    cc_image_format_t fmt = CC_IMAGE_PNG;
    if (p.size() > 4) {
        const std::string ext =
            p.substr(p.rfind('.') == std::string::npos ? p.size() : p.rfind('.'));
        if (ext == ".jpg" || ext == ".jpeg") fmt = CC_IMAGE_JPEG;
    }
    std::uint8_t* bytes = nullptr;
    std::size_t len = 0;
    const cc_status_t rc = cc_capture(s, opts, fmt, 85, &bytes, &len, nullptr, nullptr);
    if (rc != CC_OK) return rc;

    FILE* f = std::fopen(path, "wb");
    if (!f) {
        cc_buffer_free(bytes);
        return record(CC_ERR_IO, std::string("cannot open ") + path + " for writing");
    }
    const std::size_t written = std::fwrite(bytes, 1, len, f);
    std::fclose(f);
    cc_buffer_free(bytes);
    if (written != len) return record(CC_ERR_IO, "short write");
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_pixel_color(cc_session_t* s, cc_point_t at, std::uint32_t* out_rgba) {
    CC_GUARD_BEGIN
    if (!s || !out_rgba) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto screen = s->session->screen();
    if (!screen) return record(screen.error());
    auto c = screen.value()->pixel(from_c(at));
    if (!c) return record(c.error());
    *out_rgba = c.value();
    return clear_error();
    CC_GUARD_END
}

// --- JSON-shaped surface ---------------------------------------------------
//
// These all funnel through the same action dispatcher the MCP server and CLI
// use, so behaviour cannot drift between front-ends.

cc_status_t cc_windows_list(cc_session_t* s, std::int32_t include_offscreen, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(
        s, "windows", json_obj({{"mode", "list"}, {"include_offscreen", include_offscreen != 0}}),
        out_json);
    CC_GUARD_END
}

cc_status_t cc_window_focused(cc_session_t* s, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "windows", json_obj({{"mode", "focused"}}), out_json);
    CC_GUARD_END
}

cc_status_t cc_window_activate(cc_session_t* s, std::uint64_t window_id) {
    CC_GUARD_BEGIN
    return run_action_json(
        s, "windows",
        json_obj({{"mode", "activate"}, {"window_id", static_cast<long long>(window_id)}}),
        nullptr);
    CC_GUARD_END
}

cc_status_t cc_window_set_bounds(cc_session_t* s, std::uint64_t window_id, cc_rect_t bounds) {
    CC_GUARD_BEGIN
    json::Value b = json::Value::array();
    b.push_back(bounds.x);
    b.push_back(bounds.y);
    b.push_back(bounds.w);
    b.push_back(bounds.h);
    return run_action_json(
        s, "windows",
        json_obj(
            {{"mode", "bounds"}, {"window_id", static_cast<long long>(window_id)}, {"bounds", b}}),
        nullptr);
    CC_GUARD_END
}

cc_status_t cc_window_set_state(cc_session_t* s, std::uint64_t window_id, std::int32_t state) {
    CC_GUARD_BEGIN
    static const char* kNames[] = {"normal", "minimized", "maximized", "fullscreen", "hidden"};
    if (state < 0 || state > 4) return record(CC_ERR_INVALID_ARGUMENT, "state out of range");
    return run_action_json(s, "windows",
                           json_obj({{"mode", "state"},
                                     {"window_id", static_cast<long long>(window_id)},
                                     {"state", kNames[state]}}),
                           nullptr);
    CC_GUARD_END
}

cc_status_t cc_window_close(cc_session_t* s, std::uint64_t window_id) {
    CC_GUARD_BEGIN
    return run_action_json(
        s, "windows",
        json_obj({{"mode", "close"}, {"window_id", static_cast<long long>(window_id)}}), nullptr);
    CC_GUARD_END
}

cc_status_t cc_apps_list(cc_session_t* s, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "app", json_obj({{"mode", "list"}}), out_json);
    CC_GUARD_END
}

cc_status_t cc_app_launch(cc_session_t* s, const char* request_json, char** out_json) {
    CC_GUARD_BEGIN
    json::ParseError pe;
    json::Value v = json::parse(request_json ? request_json : "{}", &pe);
    if (!pe.ok) return record(CC_ERR_INVALID_ARGUMENT, "invalid JSON: " + pe.message);
    v.set("mode", "launch");
    return run_action_json(s, "app", v.dump(), out_json);
    CC_GUARD_END
}

cc_status_t cc_app_activate(cc_session_t* s, const char* name_or_bundle) {
    CC_GUARD_BEGIN
    if (!name_or_bundle) return record(CC_ERR_INVALID_ARGUMENT, "name is null");
    return run_action_json(s, "app", json_obj({{"mode", "activate"}, {"name", name_or_bundle}}),
                           nullptr);
    CC_GUARD_END
}

cc_status_t cc_a11y_snapshot(cc_session_t* s, const char* options_json, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "elements", options_json ? options_json : "{}", out_json);
    CC_GUARD_END
}

cc_status_t cc_a11y_element_at(cc_session_t* s, cc_point_t p, char** out_json) {
    CC_GUARD_BEGIN
    json::Value at = json::Value::object();
    at.set("x", p.x);
    at.set("y", p.y);
    at.set("space", to_string(static_cast<Space>(p.space)));
    return run_action_json(s, "elements", json_obj({{"mode", "at"}, {"at", at}}), out_json);
    CC_GUARD_END
}

cc_status_t cc_a11y_focused(cc_session_t* s, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "elements", json_obj({{"mode", "focused"}}), out_json);
    CC_GUARD_END
}

cc_status_t cc_clipboard_get_text(cc_session_t* s, char** out_text) {
    CC_GUARD_BEGIN
    if (!s || !out_text) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto sys = s->session->system();
    if (!sys) return record(sys.error());
    auto c = sys.value()->clipboard_get();
    if (!c) return record(c.error());
    *out_text = dup_string(c.value().text);
    return *out_text ? clear_error() : record(CC_ERR_INTERNAL, "out of memory");
    CC_GUARD_END
}

cc_status_t cc_clipboard_set_text(cc_session_t* s, const char* text) {
    CC_GUARD_BEGIN
    if (!s || !text) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    return run_action_json(s, "clipboard", json_obj({{"mode", "set"}, {"text", text}}), nullptr);
    CC_GUARD_END
}

cc_status_t cc_shell_run(cc_session_t* s, const char* request_json, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "shell", request_json ? request_json : "{}", out_json);
    CC_GUARD_END
}

cc_status_t cc_process_list(cc_session_t* s, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "process", json_obj({{"mode", "list"}, {"limit", 500}}), out_json);
    CC_GUARD_END
}

cc_status_t cc_process_kill(cc_session_t* s, std::int64_t pid, std::int32_t force) {
    CC_GUARD_BEGIN
    return run_action_json(
        s, "process",
        json_obj({{"mode", "kill"}, {"pid", static_cast<long long>(pid)}, {"force", force != 0}}),
        nullptr);
    CC_GUARD_END
}

cc_status_t cc_notify(cc_session_t* s, const char* request_json) {
    CC_GUARD_BEGIN
    return run_action_json(s, "notify", request_json ? request_json : "{}", nullptr);
    CC_GUARD_END
}

// --- devices ---------------------------------------------------------------

cc_status_t cc_devices_list(cc_session_t* s, std::int32_t booted_only, char** out_json) {
    CC_GUARD_BEGIN
    return run_action_json(
        s, "device", json_obj({{"mode", "list"}, {"booted_only", booted_only != 0}}), out_json);
    CC_GUARD_END
}

cc_status_t cc_device_open(cc_session_t* s, const char* id_or_name, std::int32_t transport,
                           char** out_handle) {
    CC_GUARD_BEGIN
    if (!s || !id_or_name || !out_handle) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto dm = s->session->devices();
    if (!dm) return record(dm.error());
    auto opened = dm.value()->open(id_or_name, static_cast<DeviceTransport>(transport));
    if (!opened) return record(opened.error());

    std::lock_guard<std::mutex> lk(s->mu);
    const std::string handle = "dev" + std::to_string(s->next_handle++);
    s->devices[handle] = opened.value();
    *out_handle = dup_string(handle);
    return *out_handle ? clear_error() : record(CC_ERR_INTERNAL, "out of memory");
    CC_GUARD_END
}

cc_status_t cc_device_close(cc_session_t* s, const char* handle) {
    CC_GUARD_BEGIN
    if (!s || !handle) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    std::lock_guard<std::mutex> lk(s->mu);
    s->devices.erase(handle);
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_device_tap(cc_session_t* s, const char* handle, cc_point_t p, std::int32_t count,
                          std::int32_t hold_ms) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    DeviceTapOptions o;
    o.count = count > 0 ? count : 1;
    o.hold = std::chrono::milliseconds{hold_ms};
    auto st = dev->tap(Point{p.x, p.y, Space::Logical}, o);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_device_swipe(cc_session_t* s, const char* handle, cc_point_t from, cc_point_t to,
                            std::int32_t duration_ms) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    DeviceSwipeOptions o;
    o.duration = std::chrono::milliseconds{duration_ms > 0 ? duration_ms : 300};
    auto st =
        dev->swipe(Point{from.x, from.y, Space::Logical}, Point{to.x, to.y, Space::Logical}, o);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_device_gesture(cc_session_t* s, const char* handle, const cc_gesture_t* g) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    if (!g) return record(CC_ERR_INVALID_ARGUMENT, "gesture is null");
    GestureRequest req;
    req.kind = static_cast<GestureKind>(g->kind);
    req.center = Point{g->center.x, g->center.y, Space::Logical};
    req.fingers = g->fingers > 0 ? g->fingers : 2;
    req.direction = static_cast<SwipeDirection>(g->direction);
    req.distance = g->distance;
    req.scale = g->scale;
    req.rotation_degrees = g->rotation_degrees;
    req.spread = g->spread;
    req.duration = std::chrono::milliseconds{g->duration_ms};
    req.hold = std::chrono::milliseconds{g->hold_ms};
    auto st = dev->gesture(req);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_device_type(cc_session_t* s, const char* handle, const char* utf8) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    if (!utf8) return record(CC_ERR_INVALID_ARGUMENT, "text is null");
    auto st = dev->type_text(utf8);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_device_button(cc_session_t* s, const char* handle, const char* name) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    if (!name) return record(CC_ERR_INVALID_ARGUMENT, "name is null");
    auto st = dev->press_button(name);
    return st ? clear_error() : record(st.error());
    CC_GUARD_END
}

cc_status_t cc_device_screenshot(cc_session_t* s, const char* handle, cc_image_format_t format,
                                 std::int32_t quality, std::uint8_t** out_bytes,
                                 std::size_t* out_len, std::int32_t* out_w, std::int32_t* out_h) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    if (!out_bytes || !out_len) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    auto f = dev->screenshot();
    if (!f) return record(f.error());

    EncodeOptions eo;
    eo.format = static_cast<ImageFormat>(format);
    eo.quality = quality > 0 ? quality : 80;
    auto bytes = encode(f.value(), eo);
    if (!bytes) return record(bytes.error());

    *out_bytes = dup_bytes(bytes.value());
    *out_len = bytes.value().size();
    if (out_w) *out_w = f.value().width;
    if (out_h) *out_h = f.value().height;
    return clear_error();
    CC_GUARD_END
}

cc_status_t cc_device_info(cc_session_t* s, const char* handle, char** out_json) {
    CC_GUARD_BEGIN
    auto dev = lookup(s, handle);
    if (!dev) return record(CC_ERR_NOT_FOUND, "unknown device handle");
    if (!out_json) return record(CC_ERR_INVALID_ARGUMENT, "out_json is null");
    return run_action_json(s, "device", json_obj({{"mode", "info"}, {"device", dev->info().id}}),
                           out_json);
    CC_GUARD_END
}

// --- batch -----------------------------------------------------------------

cc_status_t cc_batch(cc_session_t* s, const char* actions_json, char** out_json) {
    CC_GUARD_BEGIN
    if (!s || !actions_json) return record(CC_ERR_INVALID_ARGUMENT, "null argument");
    json::ParseError pe;
    json::Value actions = json::parse(actions_json, &pe);
    if (!pe.ok) return record(CC_ERR_INVALID_ARGUMENT, "invalid JSON: " + pe.message);

    auto result = actions::run_batch(*s->session, actions);
    if (out_json) {
        json::Value out = json::Value::object();
        out.set("ok", result.ok);
        out.set("text", result.text);
        out.set("result", result.value);
        if (!result.ok) out.set("error", result.error.message);
        *out_json = dup_string(out.dump());
    }
    return result.ok ? clear_error() : record(result.error);
    CC_GUARD_END
}

}  // extern "C"
