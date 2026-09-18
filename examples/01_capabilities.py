#!/usr/bin/env python3
"""What can this machine actually do?

The first thing to run on an unfamiliar host. Backends come up lazily, so a
missing accessibility grant does not stop screenshots from working - this
prints exactly which pieces are available and what to do about the rest.
"""

from computer_control import Session


def main() -> int:
    with Session() as cc:
        print(f"computer-control {cc.version} on {cc.platform}\n")

        print("Displays")
        for d in cc.displays():
            marker = " (primary)" if d.primary else ""
            print(f"  [{d.index}] {d.name}{marker}")
            print(f"       {d.bounds_logical.w:.0f}x{d.bounds_logical.h:.0f} pt "
                  f"@{d.scale:g}x = {d.bounds_physical.w:.0f}x{d.bounds_physical.h:.0f} px, "
                  f"{d.dpi:.0f} dpi")

        caps = cc.capabilities()

        print("\nBackends")
        for name, info in sorted(caps["backends"].items()):
            if info["available"]:
                print(f"  {name:14} ok    {info.get('backend', '')}")
            else:
                print(f"  {name:14} FAIL  {info.get('error', '')}")
                if info.get("remedy"):
                    # The remedy is the useful half of an error; print it.
                    for line in info["remedy"].split("\n"):
                        print(f"                 {line}")

        print("\nGestures")
        for name, info in sorted(caps["gestures"].items()):
            print(f"  {name:16} {info['fidelity']:12} {info['backend']}")
            if info["fidelity"] == "emulated" and info.get("note"):
                print(f"                   -> {info['note'][:100]}")

        tooling = caps.get("device_tooling") or []
        print(f"\nMobile tooling on PATH: {', '.join(tooling) if tooling else 'none'}")
        devices = cc.devices()
        for d in devices:
            print(f"  {d['name']} ({d['platform']}, {d['kind']}) {d['id']}")
        if not devices:
            print("  no devices found")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
