/* SPDX-License-Identifier: MIT
 *
 * computer-control stable C ABI.
 *
 * This is the binary interface: every language binding (Python, Node, Rust,
 * Go) and every out-of-process front-end goes through this header, not through
 * the C++ classes. It is C99, has no C++ types in its signatures, and never
 * throws.
 *
 * ABI rules:
 *   - Enumerators are append-only; existing values never change.
 *   - Structs are versioned by a leading `size` field. Callers set it to
 *     sizeof(the struct they were compiled against); the library reads only
 *     the prefix it understands and defaults the rest. New fields go at the
 *     end.
 *   - Every string the library returns is NUL-terminated UTF-8 owned by the
 *     library and freed with cc_string_free. Every string the caller passes is
 *     borrowed for the duration of the call only.
 *   - Every entry point returns cc_status_t. Zero is success. On failure, call
 *     cc_last_error() on the same thread for detail.
 *   - Handles are opaque and thread-confined unless stated otherwise.
 */

#ifndef CC_CAPI_H
#define CC_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(CC_BUILDING_SHARED)
#define CC_API __declspec(dllexport)
#elif defined(CC_SHARED)
#define CC_API __declspec(dllimport)
#else
#define CC_API
#endif
#define CC_CALL __cdecl
#else
#if defined(CC_BUILDING_SHARED)
#define CC_API __attribute__((visibility("default")))
#else
#define CC_API
#endif
#define CC_CALL
#endif

#define CC_ABI_VERSION 1

/* ------------------------------------------------------------------ */
/* Status and errors                                                    */
/* ------------------------------------------------------------------ */

typedef int32_t cc_status_t;

enum {
    CC_OK = 0,
    CC_ERR_INVALID_ARGUMENT = 1,
    CC_ERR_PERMISSION_DENIED = 2,
    CC_ERR_UNSUPPORTED = 3,
    CC_ERR_NOT_FOUND = 4,
    CC_ERR_TIMEOUT = 5,
    CC_ERR_BUSY = 6,
    CC_ERR_BACKEND_FAILURE = 7,
    CC_ERR_DEVICE_ERROR = 8,
    CC_ERR_IO = 9,
    CC_ERR_INTERNAL = 10
};

/* Detail for the most recent failure on the calling thread. The returned
 * pointers stay valid until the next failing call on this thread. */
CC_API cc_status_t CC_CALL cc_last_error_code(void);
CC_API const char* CC_CALL cc_last_error_message(void);
CC_API const char* CC_CALL cc_last_error_remedy(void);

CC_API void CC_CALL cc_string_free(char* s);
CC_API void CC_CALL cc_buffer_free(uint8_t* p);

CC_API int32_t CC_CALL cc_abi_version(void);
CC_API const char* CC_CALL cc_version(void);
CC_API const char* CC_CALL cc_platform(void);

/* ------------------------------------------------------------------ */
/* Core value types                                                     */
/* ------------------------------------------------------------------ */

typedef enum { CC_SPACE_LOGICAL = 0, CC_SPACE_PHYSICAL = 1, CC_SPACE_IMAGE = 2 } cc_space_t;

typedef struct {
    double x, y;
    cc_space_t space;
} cc_point_t;
typedef struct {
    double w, h;
    cc_space_t space;
} cc_size_t;
typedef struct {
    double x, y, w, h;
    cc_space_t space;
} cc_rect_t;

typedef enum {
    CC_BUTTON_LEFT = 0,
    CC_BUTTON_RIGHT = 1,
    CC_BUTTON_MIDDLE = 2,
    CC_BUTTON_BACK = 3,
    CC_BUTTON_FORWARD = 4
} cc_button_t;

typedef enum {
    CC_MOD_NONE = 0,
    CC_MOD_SHIFT = 1 << 0,
    CC_MOD_CONTROL = 1 << 1,
    CC_MOD_ALT = 1 << 2,
    CC_MOD_META = 1 << 3,
    CC_MOD_CAPSLOCK = 1 << 4,
    CC_MOD_NUMLOCK = 1 << 5,
    CC_MOD_FN = 1 << 6
} cc_modifier_t;

typedef enum {
    CC_MOTION_INSTANT = 0,
    CC_MOTION_LINEAR = 1,
    CC_MOTION_EASE_IN_OUT = 2,
    CC_MOTION_HUMAN = 3
} cc_motion_profile_t;

typedef enum { CC_SCROLL_VERTICAL = 0, CC_SCROLL_HORIZONTAL = 1 } cc_scroll_axis_t;
typedef enum { CC_DIR_UP = 0, CC_DIR_DOWN = 1, CC_DIR_LEFT = 2, CC_DIR_RIGHT = 3 } cc_direction_t;

typedef enum {
    CC_GESTURE_TAP = 0,
    CC_GESTURE_SWIPE,
    CC_GESTURE_PAN,
    CC_GESTURE_PINCH,
    CC_GESTURE_ROTATE,
    CC_GESTURE_SMART_ZOOM,
    CC_GESTURE_FORCE_PRESS,
    CC_GESTURE_EDGE_SWIPE,
    CC_GESTURE_LONG_PRESS
} cc_gesture_kind_t;

typedef enum {
    CC_FIDELITY_UNSUPPORTED = 0,
    CC_FIDELITY_EMULATED = 1,
    CC_FIDELITY_NATIVE = 2
} cc_gesture_fidelity_t;

typedef enum {
    CC_IMAGE_PNG = 0,
    CC_IMAGE_JPEG = 1,
    CC_IMAGE_WEBP = 2,
    CC_IMAGE_RAW = 3
} cc_image_format_t;

/* ------------------------------------------------------------------ */
/* Session                                                              */
/* ------------------------------------------------------------------ */

typedef struct cc_session cc_session_t;

typedef struct {
    size_t size; /* = sizeof(cc_session_config_t) */
    int32_t eager_init;
    int32_t prompt_for_permissions;
    int32_t allow_shell;
    int32_t allow_filesystem;
    int32_t allow_registry;
    int32_t allow_clipboard;
    int32_t block_when_locked;
    int32_t default_max_capture_dimension;
} cc_session_config_t;

CC_API void CC_CALL cc_session_config_default(cc_session_config_t* out);
CC_API cc_status_t CC_CALL cc_session_create(const cc_session_config_t* cfg, cc_session_t** out);
CC_API void CC_CALL cc_session_destroy(cc_session_t* s);
/* Releases every held button, key and touch contact. Idempotent. */
CC_API cc_status_t CC_CALL cc_session_release_all(cc_session_t* s);
/* JSON capability report; free with cc_string_free. */
CC_API cc_status_t CC_CALL cc_session_capabilities(cc_session_t* s, char** out_json);

/* ------------------------------------------------------------------ */
/* Displays and coordinate conversion                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t size;
    int32_t index;
    uint64_t id;
    char name[128];
    cc_rect_t bounds_logical;
    cc_rect_t bounds_physical;
    cc_rect_t work_area_logical;
    double scale;
    double dpi;
    double refresh_hz;
    int32_t orientation;
    int32_t primary;
} cc_display_t;

CC_API cc_status_t CC_CALL cc_display_refresh(cc_session_t* s);
CC_API cc_status_t CC_CALL cc_display_count(cc_session_t* s, int32_t* out_count);
CC_API cc_status_t CC_CALL cc_display_get(cc_session_t* s, int32_t index, cc_display_t* out);
CC_API cc_status_t CC_CALL cc_display_virtual_bounds(cc_session_t* s, cc_space_t space,
                                                     cc_rect_t* out);
/* Converts between spaces using the display that contains the point, so mixed
 * DPI setups convert correctly. */
CC_API cc_status_t CC_CALL cc_convert_point(cc_session_t* s, cc_point_t in, cc_space_t to,
                                            cc_point_t* out);
CC_API cc_status_t CC_CALL cc_convert_rect(cc_session_t* s, cc_rect_t in, cc_space_t to,
                                           cc_rect_t* out);

/* ------------------------------------------------------------------ */
/* Pointer input                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t size;
    cc_motion_profile_t profile;
    int32_t duration_ms;
    int32_t rate_hz;
    int32_t max_steps;
    double jitter_px;
    double overshoot_px;
    uint64_t seed;
} cc_motion_options_t;

typedef struct {
    size_t size;
    cc_button_t button;
    int32_t count; /* 0 = hover, 1, 2, 3 */
    uint32_t modifiers;
    int32_t press_duration_ms;
    int32_t inter_click_ms;
    int32_t move_first;
} cc_click_options_t;

typedef struct {
    size_t size;
    cc_scroll_axis_t axis;
    cc_direction_t direction;
    int32_t clicks;
    uint32_t modifiers;
    int32_t pixel_units;
    int32_t pixels_per_click;
    int32_t phased;
} cc_scroll_options_t;

typedef struct {
    size_t size;
    cc_point_t at;
    double pressure;
    double tilt_x, tilt_y;
    int32_t dwell_ms;
} cc_path_point_t;

typedef struct {
    size_t size;
    cc_button_t button;
    uint32_t modifiers;
    cc_motion_options_t motion;
    int32_t smooth;
    double smooth_tension;
    int32_t settle_before_release_ms;
    int32_t use_pen;
} cc_stroke_options_t;

CC_API void CC_CALL cc_motion_options_default(cc_motion_options_t* o);
CC_API void CC_CALL cc_click_options_default(cc_click_options_t* o);
CC_API void CC_CALL cc_scroll_options_default(cc_scroll_options_t* o);
CC_API void CC_CALL cc_stroke_options_default(cc_stroke_options_t* o);

CC_API cc_status_t CC_CALL cc_cursor_position(cc_session_t* s, cc_point_t* out);
CC_API cc_status_t CC_CALL cc_mouse_move(cc_session_t* s, cc_point_t to,
                                         const cc_motion_options_t* opts);
CC_API cc_status_t CC_CALL cc_mouse_click(cc_session_t* s, cc_point_t at,
                                          const cc_click_options_t* opts);
CC_API cc_status_t CC_CALL cc_mouse_click_here(cc_session_t* s, const cc_click_options_t* opts);
CC_API cc_status_t CC_CALL cc_mouse_down(cc_session_t* s, cc_button_t b, uint32_t modifiers);
CC_API cc_status_t CC_CALL cc_mouse_up(cc_session_t* s, cc_button_t b, uint32_t modifiers);
CC_API cc_status_t CC_CALL cc_scroll(cc_session_t* s, cc_point_t at,
                                     const cc_scroll_options_t* opts);
CC_API cc_status_t CC_CALL cc_drag(cc_session_t* s, cc_point_t from, cc_point_t to,
                                   const cc_stroke_options_t* opts);
CC_API cc_status_t CC_CALL cc_stroke(cc_session_t* s, const cc_path_point_t* points, size_t count,
                                     const cc_stroke_options_t* opts);

/* ------------------------------------------------------------------ */
/* Keyboard                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t size;
    double cps;
    int32_t allow_clipboard_fast_path;
    int32_t clipboard_threshold;
    int32_t restore_clipboard;
    int32_t press_enter;
    uint32_t modifiers;
} cc_type_options_t;

CC_API void CC_CALL cc_type_options_default(cc_type_options_t* o);

/* `chord` accepts "cmd+shift+a", "ctrl-c", "F5", and multi-chord sequences
 * separated by spaces ("cmd+k cmd+s"). */
CC_API cc_status_t CC_CALL cc_key_tap(cc_session_t* s, const char* chord, int32_t repeat);
CC_API cc_status_t CC_CALL cc_key_hold(cc_session_t* s, const char* chord, int32_t duration_ms);
CC_API cc_status_t CC_CALL cc_key_down(cc_session_t* s, const char* key);
CC_API cc_status_t CC_CALL cc_key_up(cc_session_t* s, const char* key);
CC_API cc_status_t CC_CALL cc_type_text(cc_session_t* s, const char* utf8,
                                        const cc_type_options_t* opts);

/* ------------------------------------------------------------------ */
/* Gestures                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t size;
    cc_gesture_kind_t kind;
    cc_point_t center;
    int32_t fingers;
    cc_direction_t direction;
    double distance;
    double scale;
    double rotation_degrees;
    double spread;
    double pressure;
    int32_t duration_ms;
    int32_t hold_ms;
    uint32_t modifiers;
    int32_t require_native;
    const cc_point_t* path; /* CC_GESTURE_PAN only */
    size_t path_count;
} cc_gesture_t;

typedef struct {
    size_t size;
    cc_gesture_fidelity_t fidelity;
    int32_t max_fingers;
    char backend[64];
    char note[256];
} cc_gesture_support_t;

CC_API void CC_CALL cc_gesture_default(cc_gesture_t* g);
CC_API cc_status_t CC_CALL cc_gesture_support(cc_session_t* s, cc_gesture_kind_t kind,
                                              int32_t fingers, cc_gesture_support_t* out);
CC_API cc_status_t CC_CALL cc_gesture_perform(cc_session_t* s, const cc_gesture_t* g);

/* ------------------------------------------------------------------ */
/* Screen capture                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t size;
    int32_t use_display; /* 1 to honour display_index */
    int32_t display_index;
    int32_t use_window;
    uint64_t window_id;
    int32_t use_region;
    cc_rect_t region;
    int32_t max_dimension;
    double scale;
    int32_t include_cursor;
} cc_capture_options_t;

CC_API void CC_CALL cc_capture_options_default(cc_capture_options_t* o);

/* Encodes in-process and hands back a heap buffer; free with cc_buffer_free.
 * `out_width`/`out_height` are the encoded image's pixel dimensions, which is
 * what Space::Image coordinates refer to. */
CC_API cc_status_t CC_CALL cc_capture(cc_session_t* s, const cc_capture_options_t* opts,
                                      cc_image_format_t format, int32_t quality,
                                      uint8_t** out_bytes, size_t* out_len, int32_t* out_width,
                                      int32_t* out_height);
CC_API cc_status_t CC_CALL cc_capture_to_file(cc_session_t* s, const cc_capture_options_t* opts,
                                              const char* path);
CC_API cc_status_t CC_CALL cc_pixel_color(cc_session_t* s, cc_point_t at, uint32_t* out_rgba);

/* ------------------------------------------------------------------ */
/* Windows, apps, accessibility, system, devices                        */
/* ------------------------------------------------------------------ */
/* These return JSON because their shapes are open-ended and versioning a
 * struct per query would be worse than versioning one schema. Free the result
 * with cc_string_free. The schema is documented in docs/json-schema.md and is
 * additive-only. */

CC_API cc_status_t CC_CALL cc_windows_list(cc_session_t* s, int32_t include_offscreen,
                                           char** out_json);
CC_API cc_status_t CC_CALL cc_window_focused(cc_session_t* s, char** out_json);
CC_API cc_status_t CC_CALL cc_window_activate(cc_session_t* s, uint64_t window_id);
CC_API cc_status_t CC_CALL cc_window_set_bounds(cc_session_t* s, uint64_t window_id,
                                                cc_rect_t bounds);
CC_API cc_status_t CC_CALL cc_window_set_state(cc_session_t* s, uint64_t window_id, int32_t state);
CC_API cc_status_t CC_CALL cc_window_close(cc_session_t* s, uint64_t window_id);

CC_API cc_status_t CC_CALL cc_apps_list(cc_session_t* s, char** out_json);
CC_API cc_status_t CC_CALL cc_app_launch(cc_session_t* s, const char* request_json,
                                         char** out_json);
CC_API cc_status_t CC_CALL cc_app_activate(cc_session_t* s, const char* name_or_bundle);

CC_API cc_status_t CC_CALL cc_a11y_snapshot(cc_session_t* s, const char* options_json,
                                            char** out_json);
CC_API cc_status_t CC_CALL cc_a11y_element_at(cc_session_t* s, cc_point_t p, char** out_json);
CC_API cc_status_t CC_CALL cc_a11y_focused(cc_session_t* s, char** out_json);

CC_API cc_status_t CC_CALL cc_clipboard_get_text(cc_session_t* s, char** out_text);
CC_API cc_status_t CC_CALL cc_clipboard_set_text(cc_session_t* s, const char* text);
CC_API cc_status_t CC_CALL cc_shell_run(cc_session_t* s, const char* request_json, char** out_json);
CC_API cc_status_t CC_CALL cc_process_list(cc_session_t* s, char** out_json);
CC_API cc_status_t CC_CALL cc_process_kill(cc_session_t* s, int64_t pid, int32_t force);
CC_API cc_status_t CC_CALL cc_notify(cc_session_t* s, const char* request_json);

CC_API cc_status_t CC_CALL cc_devices_list(cc_session_t* s, int32_t booted_only, char** out_json);
/* Opens a device and returns an opaque id used by the cc_device_* calls. */
CC_API cc_status_t CC_CALL cc_device_open(cc_session_t* s, const char* id_or_name,
                                          int32_t transport, char** out_handle);
CC_API cc_status_t CC_CALL cc_device_close(cc_session_t* s, const char* handle);
CC_API cc_status_t CC_CALL cc_device_tap(cc_session_t* s, const char* handle,
                                         cc_point_t device_point, int32_t count, int32_t hold_ms);
CC_API cc_status_t CC_CALL cc_device_swipe(cc_session_t* s, const char* handle, cc_point_t from,
                                           cc_point_t to, int32_t duration_ms);
CC_API cc_status_t CC_CALL cc_device_gesture(cc_session_t* s, const char* handle,
                                             const cc_gesture_t* g);
CC_API cc_status_t CC_CALL cc_device_type(cc_session_t* s, const char* handle, const char* utf8);
CC_API cc_status_t CC_CALL cc_device_button(cc_session_t* s, const char* handle, const char* name);
CC_API cc_status_t CC_CALL cc_device_screenshot(cc_session_t* s, const char* handle,
                                                cc_image_format_t format, int32_t quality,
                                                uint8_t** out_bytes, size_t* out_len,
                                                int32_t* out_w, int32_t* out_h);
CC_API cc_status_t CC_CALL cc_device_info(cc_session_t* s, const char* handle, char** out_json);

/* ------------------------------------------------------------------ */
/* Batch                                                                */
/* ------------------------------------------------------------------ */
/* Executes a JSON array of actions in one call. This is the latency win that
 * matters for an agent driving a UI: each round trip costs far more than the
 * actions themselves. Stops at the first failure and reports the index. */
CC_API cc_status_t CC_CALL cc_batch(cc_session_t* s, const char* actions_json, char** out_json);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CC_CAPI_H */
