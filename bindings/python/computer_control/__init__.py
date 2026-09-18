"""computer-control: cross-platform desktop and mobile-simulator automation.

A thin ctypes wrapper over the C ABI. There is no compiled extension module,
so the same wheel works on every Python 3.9+ and every interpreter (CPython,
PyPy) as long as the shared library is present.

    from computer_control import Session

    with Session() as cc:
        cc.screenshot("screen.png", max_dimension=1200)
        cc.click(640, 480)
        cc.type("hello")
        cc.gesture("pinch", scale=2.0, at=(700, 400))
"""

from .session import (  # noqa: F401
    Session,
    Point,
    Rect,
    Display,
    Space,
    Button,
    MotionProfile,
    GestureKind,
    GestureFidelity,
    ComputerControlError,
    PermissionDenied,
    Unsupported,
    NotFound,
    Timeout,
)

__version__ = "0.1.0"
__all__ = [
    "Session", "Point", "Rect", "Display", "Space", "Button", "MotionProfile",
    "GestureKind", "GestureFidelity", "ComputerControlError", "PermissionDenied",
    "Unsupported", "NotFound", "Timeout",
]
