"""ctypes declarations for the computer-control C ABI.

Kept separate from the friendly wrapper so the binding surface is auditable in
one place, and so a signature change shows up as a diff here rather than as a
segfault somewhere else.
"""

from __future__ import annotations

import ctypes
import os
import platform
import sys
from ctypes import (
    POINTER, Structure, c_char_p, c_double, c_int32, c_int64, c_size_t,
    c_uint32, c_uint64, c_uint8, c_void_p,
)

ABI_VERSION = 1


def _candidate_names() -> list[str]:
    system = platform.system()
    if system == "Darwin":
        return ["libcomputer_control.dylib"]
    if system == "Windows":
        return ["computer_control.dll", "libcomputer_control.dll"]
    return ["libcomputer_control.so", "libcomputer_control.so.0"]


def _candidate_dirs() -> list[str]:
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = [
        # Shipped inside the wheel.
        here,
        os.path.join(here, "lib"),
        # A local CMake build tree, which is how contributors run this.
        os.path.join(here, "..", "..", "..", "build"),
        os.path.join(here, "..", "..", "..", "build", "Release"),
        "/usr/local/lib",
        "/usr/lib",
        "/opt/homebrew/lib",
    ]
    if env := os.environ.get("COMPUTER_CONTROL_LIB"):
        dirs.insert(0, os.path.dirname(env) if os.path.isfile(env) else env)
    return dirs


def load_library() -> ctypes.CDLL:
    explicit = os.environ.get("COMPUTER_CONTROL_LIB")
    if explicit and os.path.isfile(explicit):
        return ctypes.CDLL(explicit)

    tried = []
    for directory in _candidate_dirs():
        for name in _candidate_names():
            path = os.path.abspath(os.path.join(directory, name))
            tried.append(path)
            if os.path.isfile(path):
                return ctypes.CDLL(path)

    # Last resort: let the dynamic loader search its own paths.
    for name in _candidate_names():
        try:
            return ctypes.CDLL(name)
        except OSError:
            tried.append(f"{name} (loader search path)")

    raise OSError(
        "cannot find the computer-control shared library.\n"
        "Set COMPUTER_CONTROL_LIB to its full path, install the package with its "
        "bundled library, or build it with:\n"
        "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build\n\n"
        "Looked in:\n  " + "\n  ".join(tried)
    )


# --- structs ---------------------------------------------------------------


class CCPoint(Structure):
    _fields_ = [("x", c_double), ("y", c_double), ("space", c_int32)]


class CCSize(Structure):
    _fields_ = [("w", c_double), ("h", c_double), ("space", c_int32)]


class CCRect(Structure):
    _fields_ = [("x", c_double), ("y", c_double), ("w", c_double), ("h", c_double),
                ("space", c_int32)]


class CCSessionConfig(Structure):
    _fields_ = [
        ("size", c_size_t),
        ("eager_init", c_int32),
        ("prompt_for_permissions", c_int32),
        ("allow_shell", c_int32),
        ("allow_filesystem", c_int32),
        ("allow_registry", c_int32),
        ("allow_clipboard", c_int32),
        ("block_when_locked", c_int32),
        ("default_max_capture_dimension", c_int32),
    ]


class CCDisplay(Structure):
    _fields_ = [
        ("size", c_size_t),
        ("index", c_int32),
        ("id", c_uint64),
        ("name", ctypes.c_char * 128),
        ("bounds_logical", CCRect),
        ("bounds_physical", CCRect),
        ("work_area_logical", CCRect),
        ("scale", c_double),
        ("dpi", c_double),
        ("refresh_hz", c_double),
        ("orientation", c_int32),
        ("primary", c_int32),
    ]


class CCMotionOptions(Structure):
    _fields_ = [
        ("size", c_size_t), ("profile", c_int32), ("duration_ms", c_int32),
        ("rate_hz", c_int32), ("max_steps", c_int32), ("jitter_px", c_double),
        ("overshoot_px", c_double), ("seed", c_uint64),
    ]


class CCClickOptions(Structure):
    _fields_ = [
        ("size", c_size_t), ("button", c_int32), ("count", c_int32),
        ("modifiers", c_uint32), ("press_duration_ms", c_int32),
        ("inter_click_ms", c_int32), ("move_first", c_int32),
    ]


class CCScrollOptions(Structure):
    _fields_ = [
        ("size", c_size_t), ("axis", c_int32), ("direction", c_int32), ("clicks", c_int32),
        ("modifiers", c_uint32), ("pixel_units", c_int32), ("pixels_per_click", c_int32),
        ("phased", c_int32),
    ]


class CCPathPoint(Structure):
    _fields_ = [
        ("size", c_size_t), ("at", CCPoint), ("pressure", c_double),
        ("tilt_x", c_double), ("tilt_y", c_double), ("dwell_ms", c_int32),
    ]


class CCStrokeOptions(Structure):
    _fields_ = [
        ("size", c_size_t), ("button", c_int32), ("modifiers", c_uint32),
        ("motion", CCMotionOptions), ("smooth", c_int32), ("smooth_tension", c_double),
        ("settle_before_release_ms", c_int32), ("use_pen", c_int32),
    ]


class CCTypeOptions(Structure):
    _fields_ = [
        ("size", c_size_t), ("cps", c_double), ("allow_clipboard_fast_path", c_int32),
        ("clipboard_threshold", c_int32), ("restore_clipboard", c_int32),
        ("press_enter", c_int32), ("modifiers", c_uint32),
    ]


class CCGesture(Structure):
    _fields_ = [
        ("size", c_size_t), ("kind", c_int32), ("center", CCPoint), ("fingers", c_int32),
        ("direction", c_int32), ("distance", c_double), ("scale", c_double),
        ("rotation_degrees", c_double), ("spread", c_double), ("pressure", c_double),
        ("duration_ms", c_int32), ("hold_ms", c_int32), ("modifiers", c_uint32),
        ("require_native", c_int32), ("path", POINTER(CCPoint)), ("path_count", c_size_t),
    ]


class CCGestureSupport(Structure):
    _fields_ = [
        ("size", c_size_t), ("fidelity", c_int32), ("max_fingers", c_int32),
        ("backend", ctypes.c_char * 64), ("note", ctypes.c_char * 256),
    ]


class CCCaptureOptions(Structure):
    _fields_ = [
        ("size", c_size_t), ("use_display", c_int32), ("display_index", c_int32),
        ("use_window", c_int32), ("window_id", c_uint64), ("use_region", c_int32),
        ("region", CCRect), ("max_dimension", c_int32), ("scale", c_double),
        ("include_cursor", c_int32),
    ]


def bind(lib: ctypes.CDLL) -> ctypes.CDLL:
    """Declare every signature. ctypes defaults to int returns, which silently
    truncates pointers on 64-bit, so nothing may be called before this runs."""
    S = c_void_p  # opaque cc_session_t*

    sigs = {
        "cc_last_error_code": ([], c_int32),
        "cc_last_error_message": ([], c_char_p),
        "cc_last_error_remedy": ([], c_char_p),
        "cc_string_free": ([c_void_p], None),
        "cc_buffer_free": ([c_void_p], None),
        "cc_abi_version": ([], c_int32),
        "cc_version": ([], c_char_p),
        "cc_platform": ([], c_char_p),

        "cc_session_config_default": ([POINTER(CCSessionConfig)], None),
        "cc_session_create": ([POINTER(CCSessionConfig), POINTER(c_void_p)], c_int32),
        "cc_session_destroy": ([S], None),
        "cc_session_release_all": ([S], c_int32),
        "cc_session_capabilities": ([S, POINTER(c_char_p)], c_int32),

        "cc_display_refresh": ([S], c_int32),
        "cc_display_count": ([S, POINTER(c_int32)], c_int32),
        "cc_display_get": ([S, c_int32, POINTER(CCDisplay)], c_int32),
        "cc_display_virtual_bounds": ([S, c_int32, POINTER(CCRect)], c_int32),
        "cc_convert_point": ([S, CCPoint, c_int32, POINTER(CCPoint)], c_int32),
        "cc_convert_rect": ([S, CCRect, c_int32, POINTER(CCRect)], c_int32),

        "cc_motion_options_default": ([POINTER(CCMotionOptions)], None),
        "cc_click_options_default": ([POINTER(CCClickOptions)], None),
        "cc_scroll_options_default": ([POINTER(CCScrollOptions)], None),
        "cc_stroke_options_default": ([POINTER(CCStrokeOptions)], None),
        "cc_type_options_default": ([POINTER(CCTypeOptions)], None),
        "cc_gesture_default": ([POINTER(CCGesture)], None),
        "cc_capture_options_default": ([POINTER(CCCaptureOptions)], None),

        "cc_cursor_position": ([S, POINTER(CCPoint)], c_int32),
        "cc_mouse_move": ([S, CCPoint, POINTER(CCMotionOptions)], c_int32),
        "cc_mouse_click": ([S, CCPoint, POINTER(CCClickOptions)], c_int32),
        "cc_mouse_click_here": ([S, POINTER(CCClickOptions)], c_int32),
        "cc_mouse_down": ([S, c_int32, c_uint32], c_int32),
        "cc_mouse_up": ([S, c_int32, c_uint32], c_int32),
        "cc_scroll": ([S, CCPoint, POINTER(CCScrollOptions)], c_int32),
        "cc_drag": ([S, CCPoint, CCPoint, POINTER(CCStrokeOptions)], c_int32),
        "cc_stroke": ([S, POINTER(CCPathPoint), c_size_t, POINTER(CCStrokeOptions)], c_int32),

        "cc_key_tap": ([S, c_char_p, c_int32], c_int32),
        "cc_key_hold": ([S, c_char_p, c_int32], c_int32),
        "cc_key_down": ([S, c_char_p], c_int32),
        "cc_key_up": ([S, c_char_p], c_int32),
        "cc_type_text": ([S, c_char_p, POINTER(CCTypeOptions)], c_int32),

        "cc_gesture_support": ([S, c_int32, c_int32, POINTER(CCGestureSupport)], c_int32),
        "cc_gesture_perform": ([S, POINTER(CCGesture)], c_int32),

        "cc_capture": ([S, POINTER(CCCaptureOptions), c_int32, c_int32,
                        POINTER(POINTER(c_uint8)), POINTER(c_size_t),
                        POINTER(c_int32), POINTER(c_int32)], c_int32),
        "cc_capture_to_file": ([S, POINTER(CCCaptureOptions), c_char_p], c_int32),
        "cc_pixel_color": ([S, CCPoint, POINTER(c_uint32)], c_int32),

        "cc_windows_list": ([S, c_int32, POINTER(c_char_p)], c_int32),
        "cc_window_focused": ([S, POINTER(c_char_p)], c_int32),
        "cc_window_activate": ([S, c_uint64], c_int32),
        "cc_window_set_bounds": ([S, c_uint64, CCRect], c_int32),
        "cc_window_set_state": ([S, c_uint64, c_int32], c_int32),
        "cc_window_close": ([S, c_uint64], c_int32),

        "cc_apps_list": ([S, POINTER(c_char_p)], c_int32),
        "cc_app_launch": ([S, c_char_p, POINTER(c_char_p)], c_int32),
        "cc_app_activate": ([S, c_char_p], c_int32),

        "cc_a11y_snapshot": ([S, c_char_p, POINTER(c_char_p)], c_int32),
        "cc_a11y_element_at": ([S, CCPoint, POINTER(c_char_p)], c_int32),
        "cc_a11y_focused": ([S, POINTER(c_char_p)], c_int32),

        "cc_clipboard_get_text": ([S, POINTER(c_char_p)], c_int32),
        "cc_clipboard_set_text": ([S, c_char_p], c_int32),
        "cc_shell_run": ([S, c_char_p, POINTER(c_char_p)], c_int32),
        "cc_process_list": ([S, POINTER(c_char_p)], c_int32),
        "cc_process_kill": ([S, c_int64, c_int32], c_int32),
        "cc_notify": ([S, c_char_p], c_int32),

        "cc_devices_list": ([S, c_int32, POINTER(c_char_p)], c_int32),
        "cc_device_open": ([S, c_char_p, c_int32, POINTER(c_char_p)], c_int32),
        "cc_device_close": ([S, c_char_p], c_int32),
        "cc_device_tap": ([S, c_char_p, CCPoint, c_int32, c_int32], c_int32),
        "cc_device_swipe": ([S, c_char_p, CCPoint, CCPoint, c_int32], c_int32),
        "cc_device_gesture": ([S, c_char_p, POINTER(CCGesture)], c_int32),
        "cc_device_type": ([S, c_char_p, c_char_p], c_int32),
        "cc_device_button": ([S, c_char_p, c_char_p], c_int32),
        "cc_device_screenshot": ([S, c_char_p, c_int32, c_int32, POINTER(POINTER(c_uint8)),
                                  POINTER(c_size_t), POINTER(c_int32), POINTER(c_int32)], c_int32),
        "cc_device_info": ([S, c_char_p, POINTER(c_char_p)], c_int32),

        "cc_batch": ([S, c_char_p, POINTER(c_char_p)], c_int32),
    }

    missing = []
    for name, (argtypes, restype) in sigs.items():
        fn = getattr(lib, name, None)
        if fn is None:
            missing.append(name)
            continue
        fn.argtypes = argtypes
        fn.restype = restype

    if missing:
        raise OSError(
            "the loaded library is missing these entry points: "
            + ", ".join(missing)
            + "\nThis usually means an older library is on the search path. "
              "Set COMPUTER_CONTROL_LIB to the matching build."
        )

    abi = lib.cc_abi_version()
    if abi != ABI_VERSION:
        raise OSError(
            f"ABI mismatch: this binding speaks version {ABI_VERSION}, the library "
            f"reports {abi}. Update whichever is older."
        )
    return lib
