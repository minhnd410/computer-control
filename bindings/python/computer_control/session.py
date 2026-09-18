"""The friendly Python API."""

from __future__ import annotations

import ctypes
import enum
import json
from dataclasses import dataclass
from typing import Any, Iterable, Sequence

from . import _ffi
from ._ffi import (
    CCCaptureOptions, CCClickOptions, CCDisplay, CCGesture, CCGestureSupport,
    CCMotionOptions, CCPathPoint, CCPoint, CCRect, CCScrollOptions,
    CCSessionConfig, CCStrokeOptions, CCTypeOptions,
)


class Space(enum.IntEnum):
    LOGICAL = 0
    PHYSICAL = 1
    IMAGE = 2


class Button(enum.IntEnum):
    LEFT = 0
    RIGHT = 1
    MIDDLE = 2
    BACK = 3
    FORWARD = 4


class MotionProfile(enum.IntEnum):
    INSTANT = 0
    LINEAR = 1
    EASE = 2
    HUMAN = 3


class GestureKind(enum.IntEnum):
    TAP = 0
    SWIPE = 1
    PAN = 2
    PINCH = 3
    ROTATE = 4
    SMART_ZOOM = 5
    FORCE_PRESS = 6
    EDGE_SWIPE = 7
    LONG_PRESS = 8


class GestureFidelity(enum.IntEnum):
    UNSUPPORTED = 0
    EMULATED = 1
    NATIVE = 2


class ComputerControlError(RuntimeError):
    """Base error. `remedy` carries the actionable next step, which is usually
    the part worth showing a user."""

    def __init__(self, code: int, message: str, remedy: str = ""):
        self.code = code
        self.message = message
        self.remedy = remedy
        super().__init__(message + (f"\n\n{remedy}" if remedy else ""))


class PermissionDenied(ComputerControlError):
    pass


class Unsupported(ComputerControlError):
    pass


class NotFound(ComputerControlError):
    pass


class Timeout(ComputerControlError):
    pass


_ERROR_CLASSES = {
    2: PermissionDenied,
    3: Unsupported,
    4: NotFound,
    5: Timeout,
}

_MODIFIERS = {
    "shift": 1 << 0, "ctrl": 1 << 1, "control": 1 << 1, "alt": 1 << 2,
    "option": 1 << 2, "opt": 1 << 2, "meta": 1 << 3, "cmd": 1 << 3,
    "command": 1 << 3, "win": 1 << 3, "super": 1 << 3, "fn": 1 << 6,
}

_DIRECTIONS = {"up": 0, "down": 1, "left": 2, "right": 3}


@dataclass(frozen=True)
class Point:
    x: float
    y: float
    space: Space = Space.LOGICAL


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    w: float
    h: float
    space: Space = Space.LOGICAL


@dataclass(frozen=True)
class Display:
    index: int
    name: str
    primary: bool
    scale: float
    dpi: float
    refresh_hz: float
    bounds_logical: Rect
    bounds_physical: Rect
    work_area: Rect


def _modifier_mask(modifiers: str | Iterable[str] | int | None) -> int:
    if modifiers is None:
        return 0
    if isinstance(modifiers, int):
        return modifiers
    parts = modifiers.replace("-", "+").split("+") if isinstance(modifiers, str) else modifiers
    mask = 0
    for part in parts:
        key = part.strip().lower()
        if not key:
            continue
        if key not in _MODIFIERS:
            raise ValueError(f"unknown modifier {part!r}")
        mask |= _MODIFIERS[key]
    return mask


def _to_point(value: Any, space: Space = Space.LOGICAL) -> CCPoint:
    if isinstance(value, CCPoint):
        return value
    if isinstance(value, Point):
        return CCPoint(value.x, value.y, int(value.space))
    if isinstance(value, (tuple, list)) and len(value) >= 2:
        return CCPoint(float(value[0]), float(value[1]), int(space))
    raise TypeError(f"expected a point as (x, y) or Point, got {value!r}")


def _rect_from_c(r: CCRect) -> Rect:
    return Rect(r.x, r.y, r.w, r.h, Space(r.space))


class Session:
    """An automation session. Use it as a context manager so every held button
    and key is released even if the block raises."""

    def __init__(
        self,
        *,
        allow_shell: bool = True,
        allow_clipboard: bool = True,
        allow_registry: bool = False,
        prompt_for_permissions: bool = False,
        max_capture_dimension: int = 1600,
    ):
        self._lib = _ffi.bind(_ffi.load_library())
        self._handle = ctypes.c_void_p()

        cfg = CCSessionConfig()
        self._lib.cc_session_config_default(ctypes.byref(cfg))
        cfg.allow_shell = int(allow_shell)
        cfg.allow_clipboard = int(allow_clipboard)
        cfg.allow_registry = int(allow_registry)
        cfg.prompt_for_permissions = int(prompt_for_permissions)
        cfg.default_max_capture_dimension = int(max_capture_dimension)

        self._check(self._lib.cc_session_create(ctypes.byref(cfg), ctypes.byref(self._handle)))
        self._open_devices: dict[str, str] = {}

    # --- lifecycle ---------------------------------------------------------

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def close(self) -> None:
        if getattr(self, "_handle", None) and self._handle.value:
            # Release before destroy: a stuck modifier survives the process.
            try:
                self._lib.cc_session_release_all(self._handle)
            except Exception:
                pass
            self._lib.cc_session_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def release_all(self) -> None:
        """Release every held mouse button, key and touch contact."""
        self._check(self._lib.cc_session_release_all(self._handle))

    # --- errors ------------------------------------------------------------

    def _check(self, code: int) -> None:
        if code == 0:
            return
        message = (self._lib.cc_last_error_message() or b"").decode("utf-8", "replace")
        remedy = (self._lib.cc_last_error_remedy() or b"").decode("utf-8", "replace")
        raise _ERROR_CLASSES.get(code, ComputerControlError)(code, message, remedy)

    def _take_string(self, ptr: ctypes.c_char_p) -> str:
        """Copies the library-owned string and frees it. Not freeing leaks on
        every call, which matters in a polling loop."""
        if not ptr:
            return ""
        try:
            return ctypes.cast(ptr, ctypes.c_char_p).value.decode("utf-8", "replace")
        finally:
            self._lib.cc_string_free(ptr)

    def _json_call(self, fn, *args) -> Any:
        out = ctypes.c_char_p()
        self._check(fn(self._handle, *args, ctypes.byref(out)))
        text = self._take_string(out)
        return json.loads(text) if text else None

    # --- info --------------------------------------------------------------

    @property
    def version(self) -> str:
        return (self._lib.cc_version() or b"").decode()

    @property
    def platform(self) -> str:
        return (self._lib.cc_platform() or b"").decode()

    def capabilities(self) -> dict:
        out = ctypes.c_char_p()
        self._check(self._lib.cc_session_capabilities(self._handle, ctypes.byref(out)))
        return json.loads(self._take_string(out))

    def displays(self, refresh: bool = True) -> list[Display]:
        if refresh:
            self._check(self._lib.cc_display_refresh(self._handle))
        count = ctypes.c_int32()
        self._check(self._lib.cc_display_count(self._handle, ctypes.byref(count)))

        result = []
        for i in range(count.value):
            d = CCDisplay()
            self._check(self._lib.cc_display_get(self._handle, i, ctypes.byref(d)))
            result.append(Display(
                index=d.index,
                name=d.name.decode("utf-8", "replace"),
                primary=bool(d.primary),
                scale=d.scale,
                dpi=d.dpi,
                refresh_hz=d.refresh_hz,
                bounds_logical=_rect_from_c(d.bounds_logical),
                bounds_physical=_rect_from_c(d.bounds_physical),
                work_area=_rect_from_c(d.work_area_logical),
            ))
        return result

    def convert(self, point: Any, to: Space, *, space: Space = Space.LOGICAL) -> Point:
        """Convert a coordinate between logical, physical and image space."""
        out = CCPoint()
        self._check(self._lib.cc_convert_point(
            self._handle, _to_point(point, space), int(to), ctypes.byref(out)))
        return Point(out.x, out.y, Space(out.space))

    # --- pointer -----------------------------------------------------------

    def cursor_position(self) -> Point:
        out = CCPoint()
        self._check(self._lib.cc_cursor_position(self._handle, ctypes.byref(out)))
        return Point(out.x, out.y, Space(out.space))

    def _motion(self, profile: MotionProfile | str, duration_ms: int | None,
                seed: int = 0) -> CCMotionOptions:
        opts = CCMotionOptions()
        self._lib.cc_motion_options_default(ctypes.byref(opts))
        if isinstance(profile, str):
            profile = MotionProfile[profile.upper()]
        opts.profile = int(profile)
        if duration_ms is not None:
            opts.duration_ms = int(duration_ms)
        opts.seed = seed
        return opts

    def move(self, x: float, y: float, *, space: Space = Space.LOGICAL,
             profile: MotionProfile | str = MotionProfile.EASE,
             duration_ms: int | None = None, seed: int = 0) -> None:
        opts = self._motion(profile, duration_ms, seed)
        self._check(self._lib.cc_mouse_move(
            self._handle, _to_point((x, y), space), ctypes.byref(opts)))

    def click(self, x: float | None = None, y: float | None = None, *,
              space: Space = Space.LOGICAL, button: Button | str = Button.LEFT,
              count: int = 1, modifiers: str | None = None) -> None:
        opts = CCClickOptions()
        self._lib.cc_click_options_default(ctypes.byref(opts))
        if isinstance(button, str):
            button = Button[button.upper()]
        opts.button = int(button)
        opts.count = int(count)
        opts.modifiers = _modifier_mask(modifiers)

        if x is None or y is None:
            self._check(self._lib.cc_mouse_click_here(self._handle, ctypes.byref(opts)))
        else:
            self._check(self._lib.cc_mouse_click(
                self._handle, _to_point((x, y), space), ctypes.byref(opts)))

    def double_click(self, x: float, y: float, **kw: Any) -> None:
        self.click(x, y, count=2, **kw)

    def triple_click(self, x: float, y: float, **kw: Any) -> None:
        self.click(x, y, count=3, **kw)

    def right_click(self, x: float, y: float, **kw: Any) -> None:
        self.click(x, y, button=Button.RIGHT, **kw)

    def hover(self, x: float, y: float, **kw: Any) -> None:
        self.click(x, y, count=0, **kw)

    def mouse_down(self, button: Button | str = Button.LEFT, modifiers: str | None = None) -> None:
        if isinstance(button, str):
            button = Button[button.upper()]
        self._check(self._lib.cc_mouse_down(self._handle, int(button), _modifier_mask(modifiers)))

    def mouse_up(self, button: Button | str = Button.LEFT, modifiers: str | None = None) -> None:
        if isinstance(button, str):
            button = Button[button.upper()]
        self._check(self._lib.cc_mouse_up(self._handle, int(button), _modifier_mask(modifiers)))

    def scroll(self, x: float, y: float, direction: str = "down", clicks: int = 3, *,
               space: Space = Space.LOGICAL, pixel_units: bool = False,
               phased: bool = False, modifiers: str | None = None) -> None:
        opts = CCScrollOptions()
        self._lib.cc_scroll_options_default(ctypes.byref(opts))
        if direction not in _DIRECTIONS:
            raise ValueError("direction must be up, down, left or right")
        opts.direction = _DIRECTIONS[direction]
        opts.axis = 1 if direction in ("left", "right") else 0
        opts.clicks = int(clicks)
        opts.pixel_units = int(pixel_units)
        opts.phased = int(phased)
        opts.modifiers = _modifier_mask(modifiers)
        self._check(self._lib.cc_scroll(
            self._handle, _to_point((x, y), space), ctypes.byref(opts)))

    def _stroke_options(self, button: Button | str, duration_ms: int | None, smooth: bool,
                        modifiers: str | None, profile: MotionProfile | str) -> CCStrokeOptions:
        opts = CCStrokeOptions()
        self._lib.cc_stroke_options_default(ctypes.byref(opts))
        if isinstance(button, str):
            button = Button[button.upper()]
        opts.button = int(button)
        opts.modifiers = _modifier_mask(modifiers)
        opts.smooth = int(smooth)
        if isinstance(profile, str):
            profile = MotionProfile[profile.upper()]
        opts.motion.profile = int(profile)
        opts.motion.duration_ms = int(duration_ms if duration_ms is not None else 450)
        return opts

    def drag(self, from_xy: Sequence[float], to_xy: Sequence[float], *,
             space: Space = Space.LOGICAL, button: Button | str = Button.LEFT,
             duration_ms: int | None = None, modifiers: str | None = None,
             profile: MotionProfile | str = MotionProfile.EASE) -> None:
        opts = self._stroke_options(button, duration_ms, False, modifiers, profile)
        self._check(self._lib.cc_drag(self._handle, _to_point(from_xy, space),
                                      _to_point(to_xy, space), ctypes.byref(opts)))

    def stroke(self, points: Sequence[Sequence[float]], *, space: Space = Space.LOGICAL,
               button: Button | str = Button.LEFT, smooth: bool = False,
               duration_ms: int | None = None, modifiers: str | None = None,
               pressures: Sequence[float] | None = None) -> None:
        """Draw a freehand path with the button held."""
        if len(points) < 2:
            raise ValueError("a stroke needs at least two points")

        array = (CCPathPoint * len(points))()
        for i, p in enumerate(points):
            array[i].size = ctypes.sizeof(CCPathPoint)
            array[i].at = _to_point(p, space)
            array[i].pressure = float(pressures[i]) if pressures else 1.0
            array[i].dwell_ms = 0

        opts = self._stroke_options(button, duration_ms, smooth, modifiers, MotionProfile.EASE)
        self._check(self._lib.cc_stroke(self._handle, array, len(points), ctypes.byref(opts)))

    # --- keyboard ----------------------------------------------------------

    def key(self, chord: str, repeat: int = 1) -> None:
        """Press a chord such as 'cmd+shift+a', or a sequence: 'cmd+k cmd+s'."""
        self._check(self._lib.cc_key_tap(self._handle, chord.encode(), int(repeat)))

    def key_hold(self, chord: str, duration_ms: int = 500) -> None:
        self._check(self._lib.cc_key_hold(self._handle, chord.encode(), int(duration_ms)))

    def key_down(self, key: str) -> None:
        self._check(self._lib.cc_key_down(self._handle, key.encode()))

    def key_up(self, key: str) -> None:
        self._check(self._lib.cc_key_up(self._handle, key.encode()))

    def type(self, text: str, *, enter: bool = False, cps: float = 0) -> None:
        opts = CCTypeOptions()
        self._lib.cc_type_options_default(ctypes.byref(opts))
        opts.press_enter = int(enter)
        opts.cps = float(cps)
        self._check(self._lib.cc_type_text(self._handle, text.encode("utf-8"), ctypes.byref(opts)))

    # --- gestures ----------------------------------------------------------

    def gesture_support(self, kind: GestureKind | str, fingers: int = 2) -> dict:
        if isinstance(kind, str):
            kind = GestureKind[kind.upper()]
        out = CCGestureSupport()
        self._check(self._lib.cc_gesture_support(
            self._handle, int(kind), int(fingers), ctypes.byref(out)))
        return {
            "fidelity": GestureFidelity(out.fidelity).name.lower(),
            "max_fingers": out.max_fingers,
            "backend": out.backend.decode("utf-8", "replace"),
            "note": out.note.decode("utf-8", "replace"),
        }

    def gesture(self, kind: GestureKind | str, *, at: Sequence[float] | None = None,
                space: Space = Space.LOGICAL, fingers: int = 2, direction: str = "left",
                distance: float = 200.0, scale: float = 2.0, degrees: float = 0.0,
                spread: float = 120.0, duration_ms: int = 300, hold_ms: int = 0,
                modifiers: str | None = None, require_native: bool = False,
                path: Sequence[Sequence[float]] | None = None) -> None:
        if isinstance(kind, str):
            kind = GestureKind[kind.upper()]

        g = CCGesture()
        self._lib.cc_gesture_default(ctypes.byref(g))
        g.kind = int(kind)
        g.center = _to_point(at, space) if at is not None else _to_point(self.cursor_position())
        g.fingers = int(fingers)
        if direction not in _DIRECTIONS:
            raise ValueError("direction must be up, down, left or right")
        g.direction = _DIRECTIONS[direction]
        g.distance = float(distance)
        g.scale = float(scale)
        g.rotation_degrees = float(degrees)
        g.spread = float(spread)
        g.duration_ms = int(duration_ms)
        g.hold_ms = int(hold_ms)
        g.modifiers = _modifier_mask(modifiers)
        g.require_native = int(require_native)

        keepalive = None
        if path:
            keepalive = (CCPoint * len(path))(*[_to_point(p, space) for p in path])
            g.path = keepalive
            g.path_count = len(path)

        self._check(self._lib.cc_gesture_perform(self._handle, ctypes.byref(g)))
        del keepalive  # explicit: the array must outlive the call above

    def pinch(self, scale: float, *, at: Sequence[float] | None = None, **kw: Any) -> None:
        self.gesture(GestureKind.PINCH, at=at, scale=scale, **kw)

    def swipe(self, direction: str, *, fingers: int = 2, **kw: Any) -> None:
        self.gesture(GestureKind.SWIPE, direction=direction, fingers=fingers, **kw)

    def rotate(self, degrees: float, **kw: Any) -> None:
        self.gesture(GestureKind.ROTATE, degrees=degrees, **kw)

    def long_press(self, x: float, y: float, hold_ms: int = 700, **kw: Any) -> None:
        self.gesture(GestureKind.LONG_PRESS, at=(x, y), hold_ms=hold_ms, **kw)

    # --- capture -----------------------------------------------------------

    def _capture_options(self, display: int | None, window_id: int | None,
                         region: Sequence[float] | None, max_dimension: int | None,
                         scale: float, cursor: bool) -> CCCaptureOptions:
        opts = CCCaptureOptions()
        self._lib.cc_capture_options_default(ctypes.byref(opts))
        if display is not None:
            opts.use_display = 1
            opts.display_index = int(display)
        if window_id is not None:
            opts.use_window = 1
            opts.window_id = int(window_id)
        if region is not None:
            opts.use_region = 1
            opts.region = CCRect(float(region[0]), float(region[1]), float(region[2]),
                                 float(region[3]), int(Space.LOGICAL))
        if max_dimension is not None:
            opts.max_dimension = int(max_dimension)
        opts.scale = float(scale)
        opts.include_cursor = int(cursor)
        return opts

    def screenshot(self, path: str | None = None, *, display: int | None = None,
                   window_id: int | None = None, region: Sequence[float] | None = None,
                   max_dimension: int | None = None, scale: float = 1.0,
                   cursor: bool = True, format: str = "png",
                   quality: int = 80) -> bytes | None:
        """Capture the screen. Writes to `path` if given, else returns the
        encoded bytes."""
        opts = self._capture_options(display, window_id, region, max_dimension, scale, cursor)

        if path is not None:
            self._check(self._lib.cc_capture_to_file(
                self._handle, ctypes.byref(opts), path.encode()))
            return None

        fmt = {"png": 0, "jpeg": 1, "jpg": 1, "webp": 2, "raw": 3}[format.lower()]
        buf = ctypes.POINTER(ctypes.c_uint8)()
        length = ctypes.c_size_t()
        width = ctypes.c_int32()
        height = ctypes.c_int32()
        self._check(self._lib.cc_capture(
            self._handle, ctypes.byref(opts), fmt, int(quality), ctypes.byref(buf),
            ctypes.byref(length), ctypes.byref(width), ctypes.byref(height)))
        try:
            return bytes(bytearray(buf[i] for i in range(length.value)))
        finally:
            self._lib.cc_buffer_free(buf)

    def pixel(self, x: float, y: float, space: Space = Space.LOGICAL) -> tuple[int, int, int, int]:
        out = ctypes.c_uint32()
        self._check(self._lib.cc_pixel_color(
            self._handle, _to_point((x, y), space), ctypes.byref(out)))
        v = out.value
        return ((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)

    # --- windows, apps, accessibility --------------------------------------

    def windows(self, include_offscreen: bool = False) -> list[dict]:
        result = self._json_call(self._lib.cc_windows_list, int(include_offscreen))
        return (result or {}).get("result", {}).get("windows", [])

    def focused_window(self) -> dict:
        result = self._json_call(self._lib.cc_window_focused)
        return (result or {}).get("result", {})

    def activate_window(self, window_id: int) -> None:
        self._check(self._lib.cc_window_activate(self._handle, ctypes.c_uint64(window_id)))

    def set_window_bounds(self, window_id: int, x: float, y: float, w: float, h: float) -> None:
        rect = CCRect(x, y, w, h, int(Space.LOGICAL))
        self._check(self._lib.cc_window_set_bounds(
            self._handle, ctypes.c_uint64(window_id), rect))

    def close_window(self, window_id: int) -> None:
        self._check(self._lib.cc_window_close(self._handle, ctypes.c_uint64(window_id)))

    def apps(self) -> list[dict]:
        result = self._json_call(self._lib.cc_apps_list)
        return (result or {}).get("result", {}).get("apps", [])

    def launch(self, name: str, *, args: Sequence[str] | None = None,
               cwd: str | None = None) -> dict:
        request = {"name": name}
        if args:
            request["args"] = list(args)
        if cwd:
            request["cwd"] = cwd
        result = self._json_call(self._lib.cc_app_launch, json.dumps(request).encode())
        return (result or {}).get("result", {})

    def activate_app(self, name: str) -> None:
        self._check(self._lib.cc_app_activate(self._handle, name.encode()))

    def elements(self, *, pid: int | None = None, interactive_only: bool = True,
                 max_nodes: int | None = None, budget_ms: int | None = None) -> dict:
        options: dict[str, Any] = {"interactive_only": interactive_only}
        if pid is not None:
            options["pid"] = pid
        if max_nodes is not None:
            options["max_nodes"] = max_nodes
        if budget_ms is not None:
            options["budget_ms"] = budget_ms
        result = self._json_call(self._lib.cc_a11y_snapshot, json.dumps(options).encode())
        return (result or {}).get("result", {})

    def element_at(self, x: float, y: float, space: Space = Space.LOGICAL) -> dict:
        result = self._json_call(self._lib.cc_a11y_element_at, _to_point((x, y), space))
        return (result or {}).get("result", {})

    def focused_element(self) -> dict:
        result = self._json_call(self._lib.cc_a11y_focused)
        return (result or {}).get("result", {})

    # --- system ------------------------------------------------------------

    def get_clipboard(self) -> str:
        out = ctypes.c_char_p()
        self._check(self._lib.cc_clipboard_get_text(self._handle, ctypes.byref(out)))
        return self._take_string(out)

    def set_clipboard(self, text: str) -> None:
        self._check(self._lib.cc_clipboard_set_text(self._handle, text.encode("utf-8")))

    def shell(self, command: str, *, timeout_ms: int = 30000, cwd: str | None = None) -> dict:
        request: dict[str, Any] = {"command": command, "timeout_ms": timeout_ms}
        if cwd:
            request["cwd"] = cwd
        result = self._json_call(self._lib.cc_shell_run, json.dumps(request).encode())
        return (result or {}).get("result", {})

    def processes(self) -> list[dict]:
        result = self._json_call(self._lib.cc_process_list)
        return (result or {}).get("result", {}).get("processes", [])

    def kill(self, pid: int, force: bool = False) -> None:
        self._check(self._lib.cc_process_kill(self._handle, pid, int(force)))

    def notify(self, message: str, title: str = "computer-control") -> None:
        self._check(self._lib.cc_notify(
            self._handle, json.dumps({"message": message, "title": title}).encode()))

    # --- devices -----------------------------------------------------------

    def devices(self, booted_only: bool = False) -> list[dict]:
        result = self._json_call(self._lib.cc_devices_list, int(booted_only))
        return (result or {}).get("result", {}).get("devices", [])

    def open_device(self, id_or_name: str, transport: str = "auto") -> "DeviceHandle":
        mapping = {"auto": 0, "bridge": 1, "onscreen": 2}
        out = ctypes.c_char_p()
        self._check(self._lib.cc_device_open(
            self._handle, id_or_name.encode(), mapping[transport], ctypes.byref(out)))
        return DeviceHandle(self, self._take_string(out))

    # --- batch -------------------------------------------------------------

    def batch(self, actions: Sequence[dict]) -> dict:
        """Run several actions in one call. Far faster than separate calls when
        the sequence is predictable."""
        result = self._json_call(self._lib.cc_batch, json.dumps(list(actions)).encode())
        return (result or {}).get("result", {})


class DeviceHandle:
    """An open iOS simulator, Android emulator or mirrored device. Coordinates
    are in the device's own points."""

    def __init__(self, session: Session, handle: str):
        self._session = session
        self._handle = handle

    def __enter__(self) -> "DeviceHandle":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def close(self) -> None:
        if self._handle:
            self._session._lib.cc_device_close(self._session._handle, self._handle.encode())
            self._handle = ""

    @property
    def info(self) -> dict:
        result = self._session._json_call(
            self._session._lib.cc_device_info, self._handle.encode())
        return (result or {}).get("result", {})

    def tap(self, x: float, y: float, *, count: int = 1, hold_ms: int = 0) -> None:
        self._session._check(self._session._lib.cc_device_tap(
            self._session._handle, self._handle.encode(),
            CCPoint(x, y, int(Space.LOGICAL)), count, hold_ms))

    def swipe(self, from_xy: Sequence[float], to_xy: Sequence[float],
              duration_ms: int = 300) -> None:
        self._session._check(self._session._lib.cc_device_swipe(
            self._session._handle, self._handle.encode(),
            CCPoint(from_xy[0], from_xy[1], int(Space.LOGICAL)),
            CCPoint(to_xy[0], to_xy[1], int(Space.LOGICAL)), duration_ms))

    def type(self, text: str) -> None:
        self._session._check(self._session._lib.cc_device_type(
            self._session._handle, self._handle.encode(), text.encode("utf-8")))

    def button(self, name: str) -> None:
        self._session._check(self._session._lib.cc_device_button(
            self._session._handle, self._handle.encode(), name.encode()))

    def screenshot(self, path: str | None = None, format: str = "png",
                   quality: int = 80) -> bytes | None:
        fmt = {"png": 0, "jpeg": 1, "jpg": 1}[format.lower()]
        buf = ctypes.POINTER(ctypes.c_uint8)()
        length = ctypes.c_size_t()
        width = ctypes.c_int32()
        height = ctypes.c_int32()
        self._session._check(self._session._lib.cc_device_screenshot(
            self._session._handle, self._handle.encode(), fmt, quality,
            ctypes.byref(buf), ctypes.byref(length), ctypes.byref(width), ctypes.byref(height)))
        try:
            data = bytes(bytearray(buf[i] for i in range(length.value)))
        finally:
            self._session._lib.cc_buffer_free(buf)

        if path:
            with open(path, "wb") as f:
                f.write(data)
            return None
        return data
