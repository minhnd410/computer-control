<!-- Split out of the README; see the table of contents there. -->
# Driving phones and simulators

Two transports, chosen automatically:

- **Bridge** — the device's own tooling (`xcrun simctl`, `idb`, `adb`). Exact coordinates, works when the window is hidden or the device is headless.
- **Onscreen** — locate the device's viewport inside its host window and translate device points into host points. Works for anything visible, including iPhone Mirroring and scrcpy, and is the only option when the CLI tooling is missing.

```bash
cc device --mode list
cc device --mode tap    --device "iPhone 15" --at 196,420
cc device --mode swipe  --device "Pixel 8" --from 540,1600 --to 540,600
cc device --mode gesture --device "iPhone 15" --kind pinch --scale 2.0
cc device --mode screenshot --device "iPhone 15" --out phone.png
cc device --mode tree   --device "Pixel 8"       # uiautomator element tree
```

Coordinates are always in the **device's own points**. An iPhone 15 Pro is 393×852 regardless of how the simulator window is sized on your screen.

| | iOS Simulator | Android emulator / device | Mirrored (iPhone Mirroring, scrcpy) |
|---|---|---|---|
| Tap / swipe | `idb` if present, else onscreen | `adb input` | onscreen |
| Multi-touch | onscreen | onscreen | onscreen |
| Text entry | `idb` or onscreen | `adb input text` (ASCII) | onscreen |
| Screenshot | `simctl io` | `adb exec-out screencap` | host capture of the viewport |
| Install / launch | `simctl` | `adb install` / `monkey` | not available |
| Element tree | `idb` only | `uiautomator dump` | not available |

**`simctl` has no public tap or swipe command** — Apple never shipped one. Install [idb](https://fbidb.io/) for bridge-level input, or rely on the onscreen path, which needs the simulator window visible and unoccluded. If the window is scaled below ~75% of the device's logical size, small targets become unreliable and the tool warns you.

---

[← README](../README.md)
