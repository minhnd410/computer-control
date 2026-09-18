#!/usr/bin/env python3
"""The coordinate-space trap, demonstrated.

On a Retina or scaled display, a pixel read off a screenshot is not a point you
can click. This shows the arithmetic and how the space tag removes the problem.
Read-only: it takes a screenshot and converts numbers, nothing else.
"""

from computer_control import Session, Space


def main() -> int:
    with Session() as cc:
        display = next(d for d in cc.displays() if d.primary)
        print(f"{display.name}: {display.bounds_logical.w:.0f}x{display.bounds_logical.h:.0f} pt "
              f"at {display.scale:g}x = "
              f"{display.bounds_physical.w:.0f}x{display.bounds_physical.h:.0f} px\n")

        if display.scale == 1.0:
            print("This display is 1x, so logical and physical coincide and the trap does not")
            print("bite here. On a Retina Mac or a Windows monitor at 150% it would.\n")

        # Capture downscaled, the way an agent would to fit a payload budget.
        png = cc.screenshot(max_dimension=1000)
        print(f"Captured {len(png)} bytes, downscaled to fit 1000 px on the long side.")

        # Pretend we spotted something at this position in that image.
        image_point = (500, 300)
        logical = cc.convert(image_point, Space.LOGICAL, space=Space.IMAGE)
        physical = cc.convert(image_point, Space.PHYSICAL, space=Space.IMAGE)

        print(f"\nA feature at {image_point} in that image is:")
        print(f"  logical  ({logical.x:7.1f}, {logical.y:7.1f})   <- what a click needs")
        print(f"  physical ({physical.x:7.1f}, {physical.y:7.1f})")

        dx = abs(logical.x - image_point[0])
        dy = abs(logical.y - image_point[1])
        if dx > 1 or dy > 1:
            print(f"\nClicking the raw image coordinate would miss by "
                  f"({dx:.0f}, {dy:.0f}) points.")
            print("Passing space=Space.IMAGE converts it for you:")
            print("    cc.click(500, 300, space=Space.IMAGE)")

        # Round-tripping must be lossless, which is what makes the model safe.
        back = cc.convert((logical.x, logical.y), Space.IMAGE)
        print(f"\nRound trip: image -> logical -> image = "
              f"({back.x:.1f}, {back.y:.1f}), started at {image_point}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
