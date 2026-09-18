#!/usr/bin/env python3
"""Fidelity-aware gestures.

Multi-touch support differs sharply by platform: Windows and Linux can inject
real contacts, macOS cannot. Rather than substituting something that looks
similar and hoping, ask first.

Read-only unless you pass --act, which performs a pinch at the pointer.
"""

import sys

from computer_control import GestureKind, Session, Unsupported


def main() -> int:
    act = "--act" in sys.argv

    with Session() as cc:
        print(f"{cc.platform}\n")

        interesting = [
            ("tap", 1), ("long_press", 1), ("swipe", 2), ("swipe", 3),
            ("pan", 2), ("pinch", 2), ("rotate", 2), ("force_press", 1),
        ]
        for kind, fingers in interesting:
            s = cc.gesture_support(kind, fingers)
            label = f"{kind} x{fingers}"
            print(f"  {label:16} {s['fidelity']:12} via {s['backend']}")
            if s["note"]:
                print(f"                   {s['note'][:96]}")

        print("\nAsking for a gesture this platform cannot do honestly:")
        try:
            cc.gesture(GestureKind.ROTATE, degrees=90, require_native=True)
            print("  rotate: performed natively")
        except Unsupported as e:
            # This is the point: a clear refusal beats a wrong approximation.
            print(f"  refused: {e.message}")

        if not act:
            print("\n(pass --act to actually perform a pinch at the pointer)")
            return 0

        where = cc.cursor_position()
        print(f"\nPinching to zoom in at ({where.x:.0f}, {where.y:.0f})...")
        cc.pinch(2.0, at=(where.x, where.y), duration_ms=400)
        print("done; pinch back out with cc.pinch(0.5) if something zoomed")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
