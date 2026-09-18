#!/usr/bin/env python3
"""Drive a phone: an iOS simulator, an Android emulator, or a mirrored handset.

Coordinates are in the device's own points, so the same script works whether
the simulator window is full size or scaled to a corner of the screen.

Read-only unless you pass --act.
"""

import sys

from computer_control import ComputerControlError, Session


def main() -> int:
    act = "--act" in sys.argv

    with Session() as cc:
        devices = cc.devices()
        if not devices:
            tooling = cc.capabilities().get("device_tooling") or []
            print("No devices found.")
            print(f"Tooling on PATH: {', '.join(tooling) if tooling else 'none'}")
            print("\nFor iOS:     install Xcode, then `xcrun simctl boot <device>`")
            print("             (`sudo xcode-select -s /Applications/Xcode.app` if simctl")
            print("              is missing but Xcode is installed)")
            print("For Android: install platform-tools, then start an emulator")
            print("\nA simulator visible on screen is also usable without any CLI tooling.")
            return 1

        for d in devices:
            size = d.get("screen_points")
            geometry = f"{size['width']:.0f}x{size['height']:.0f} pt @{size['scale']:g}x" \
                if size else "unknown geometry"
            print(f"  {d['name']:28} {d['platform']:8} {geometry}")
            print(f"    id={d['id']}  transports={','.join(d['transports'])}")

        target = devices[0]
        print(f"\nOpening {target['name']}...")
        try:
            with cc.open_device(target["id"]) as phone:
                info = phone.info
                print(f"  transport: {info.get('active_transport')}")
                viewport = info.get("viewport")
                if viewport:
                    scale = viewport["host_px_per_device_pt"]
                    print(f"  viewport:  {scale:.2f} host px per device pt")
                    if viewport.get("warning"):
                        print(f"  warning:   {viewport['warning']}")

                if not act:
                    print("\n(pass --act to take a device screenshot and tap the centre)")
                    return 0

                phone.screenshot("device.png")
                print("  wrote device.png")

                size = info.get("screen_points")
                if size:
                    phone.tap(size["width"] / 2, size["height"] / 2)
                    print(f"  tapped the centre ({size['width'] / 2:.0f}, "
                          f"{size['height'] / 2:.0f}) in device points")
        except ComputerControlError as e:
            print(f"\n  {e.message}")
            if e.remedy:
                print(f"  {e.remedy}")
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
