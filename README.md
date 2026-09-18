# computer-control

[![CI](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml/badge.svg)](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey.svg)

**One C++ core for driving a desktop — and the phones on it.** macOS, Windows and Linux, plus iOS simulators, Android emulators and mirrored handsets, behind a single API that ships as a native library, a stable C ABI, a CLI, and an MCP server.

```bash
cc permissions --request                # ask the OS for what it needs
cc screenshot --out screen.png          # capture, DPI-correct
cc click --at 640,480                   # click
cc gesture --kind pinch --scale 2.0     # real multi-touch where the OS allows it
cc device --mode tap --device "iPhone 15" --at 196,420   # tap an iOS simulator
```

---

## Why this exists

Desktop automation tools tend to pick one platform, one language, and one level of abstraction. This one makes three specific bets:

**Coordinate spaces are part of the type system.** The single most common bug in this space is reading a pixel off a Retina screenshot and clicking it as if it were a point. Every coordinate here carries a space — `logical`, `physical`, or `image` — and conversion goes through the display topology, so mixed-DPI multi-monitor setups convert per display rather than with one global factor.

**Gesture fidelity is reported, not faked.** Windows and Linux can synthesize genuine multi-touch contacts. macOS cannot — there is no public API for it. Instead of silently substituting something that looks similar, `capabilities` tells you whether each gesture is `native`, `emulated`, or `unsupported`, names the backend, and explains the substitution. You can pass `require_native` to refuse emulation outright.

**The phone on your screen is a first-class target.** An iOS simulator is a rectangle inside a host window with its own coordinate system and its own scale factor, on top of the host's. This library knows that, and the same script drives a bridged simulator, an `adb` device, and a mirrored handset unchanged.

Everything is written once in C++20 and exposed through a stable C ABI, so the Python binding, the CLI and the MCP server are all thin shells over identical behaviour. There is no second implementation to drift.

---

## What it can do

| Area | Capabilities |
|---|---|
| **Pointer** | move, click (single/double/triple/hover), all five buttons, modifier-clicks, press/release, scroll (line and pixel-precise, phased), drag-and-drop with correct press/settle timing |
| **Paths** | freehand strokes with optional Catmull-Rom smoothing, per-point pressure and dwell, pen/stylus routing where supported |
| **Motion** | `instant`, `linear`, `ease`, and `human` (jitter + overshoot-and-settle), seedable for reproducible paths |
| **Keyboard** | chords (`cmd+shift+a`), chord sequences (`cmd+k cmd+s`), hold-for-duration, explicit down/up, layout-independent Unicode typing (emoji, CJK) |
| **Gestures** | tap, n-finger swipe/pan, pinch, rotate, smart zoom, long press, force press, edge swipe — up to 10 contacts |
| **Capture** | full desktop, per-display, per-window, per-region; PNG and JPEG encoded in-process; automatic downscaling to a payload budget |
| **Windows** | list, activate, move, resize, minimise/maximise/fullscreen, close; fuzzy title matching |
| **Apps** | list, launch with args, activate, quit |
| **Accessibility** | full element tree with numbered labels, element-at-point, focused element, invoke/toggle/set-value |
| **System** | clipboard, process list/kill, shell, notifications, Windows registry |
| **Permissions** | check and request OS grants, with the responsible-process diagnosis that explains why a grant looks present but is not |
| **Mobile** | discovery, boot/shutdown, tap/swipe/stroke/gesture, text, hardware buttons, screenshots, install/launch/terminate, deep links, device UI tree |

### Gesture fidelity by platform

This is the table worth reading before you design around gestures.

| Gesture | Windows | Linux | macOS |
|---|---|---|---|
| Tap, long press | native | native (uinput) / emulated | native |
| 2-finger swipe, pan | native | native (uinput) / emulated | **native** (phased scroll) |
| 3–5 finger swipe | native | native (uinput) | emulated (Mission Control keys) |
| Pinch / zoom | native | native (uinput) / emulated | emulated (`cmd`+scroll) |
| Rotate | native | native (uinput) | **unsupported** |
| Force press | native | native (uinput) | emulated (long press) |

- **Windows** uses `InjectTouchInput` — real contacts, up to 10, seen by the compositor and the app.
- **Linux** creates a virtual multitouch touchscreen via `/dev/uinput`. This works on Wayland as well as X11 because it operates at the kernel level. Without write access to `/dev/uinput` it falls back to XTest emulation; see [Linux setup](#linux).
- **macOS** has no public multi-touch synthesis API at all. Two-finger swipe and pan are genuinely native because `CGEvent` exposes trackpad scroll phases; pinch maps to `cmd`+scroll, which nearly every Mac app treats as zoom; rotation has no honest equivalent and is refused rather than approximated.

Run `cc capabilities` on any machine for the live answer.

---

## Installation

Four ways to install, with real trade-offs. Pick by what you need, not by what looks easiest.

### Comparison

| | Prebuilt binary | Build from source | Docker | Python package |
|---|---|---|---|---|
| **Setup effort** | lowest | moderate | low | low |
| **Real desktop control** | yes | yes | **no** — container only | yes |
| **Mobile simulators** | yes | yes | Android only | yes |
| **Native multi-touch** | yes | yes | yes (with `--device /dev/uinput`) | yes |
| **Startup time** | ~5 ms | ~5 ms | ~5 ms + container | ~30 ms (ctypes) |
| **Customisable build flags** | no | yes | yes | no |
| **Signed / notarised** | not yet — see below | you sign it | n/a | n/a |
| **Best for** | trying it out, CI runners | contributors, custom flags | headless CI, sandboxed scraping | scripting, notebooks |

<details>
<summary><b>Prebuilt binary</b> — fastest path, no toolchain</summary>

> **No tagged release yet.** Until there is one, the closest thing is the build artifacts attached to every green CI run: open the latest run under [Actions](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml), scroll to **Artifacts**, and download the archive for your platform. Building from source is the supported path for now.

Once extracted, put `cc` and `computer-control-mcp` on your `PATH`:

```bash
sudo mv cc computer-control-mcp /usr/local/bin/
```

**Advantages** — no compiler, no dependencies, one file to delete when you are done. Starts in milliseconds, which matters when an agent invokes it repeatedly.

**Limitations** — you get the flags CI chose. CI artifacts are **not code-signed or notarised**, so macOS Gatekeeper will quarantine them (`xattr -d com.apple.quarantine cc` to clear it) and the Accessibility grant will not persist across downloads, because an unsigned binary's TCC identity is its code hash. If you intend to keep the grant, build from source with `CC_CODESIGN_IDENTITY` set — see the [macOS permissions](#macos) section.

</details>

<details>
<summary><b>Build from source</b> — full control</summary>

Requirements: CMake 3.20+, a C++20 compiler (AppleClang 14+, MSVC 19.30+, GCC 11+ or Clang 14+).

```bash
git clone https://github.com/minhnd410/computer-control.git
cd computer-control
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Produces `build/cc`, `build/computer-control-mcp`, `libcomputer_control.{a,so,dylib,dll}` and the public headers.

Platform packages:

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libx11-dev libxtst-dev libxrandr-dev libxfixes-dev zlib1g-dev
# Fedora
sudo dnf install gcc-c++ cmake libX11-devel libXtst-devel libXrandr-devel libXfixes-devel zlib-devel
# macOS
xcode-select --install
# Windows — Visual Studio 2022 with the C++ workload, then:
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release
```

Useful options: `-DCC_BUILD_MCP=OFF`, `-DCC_BUILD_CLI=OFF`, `-DCC_BUILD_SHARED=OFF`, `-DCC_ENABLE_NATIVE=ON` (tune for this CPU — do not use for redistributable builds), `-DCC_USE_SYSTEM_ZLIB=OFF` (use the bundled deflate instead).

**Advantages** — choose your flags, build with sanitisers, and on macOS you can sign the binary yourself so permission grants survive rebuilds.

**Limitations** — you need a toolchain, and a cold Release build takes a couple of minutes.

</details>

<details>
<summary><b>Docker</b> — headless and disposable</summary>

```bash
docker build -t computer-control -f docker/Dockerfile .
docker run --rm -p 8765:8765 computer-control
```

The image runs Xvfb, so there is a real X display to capture and drive. Useful for scraping, browser automation and CI where you want a throwaway desktop.

```bash
# With real multi-touch (needs the uinput device from the host)
docker run --rm --device /dev/uinput -p 8765:8765 computer-control

# Driving Android emulators over adb on the host
docker run --rm --network host -e ADB_SERVER_SOCKET=tcp:host.docker.internal:5037 computer-control
```

**Advantages** — zero host contamination, reproducible, and the only safe way to let an untrusted agent drive a desktop. Ships with X11, `adb` and the Linux dependencies preinstalled.

**Limitations that matter:**
- **It cannot control your real desktop.** A container has no access to the host's display server. This is a sandbox, not a remote control.
- iOS simulators cannot run in Docker at all — they need macOS and Xcode.
- Native multi-touch needs `--device /dev/uinput` plus a kernel with `uinput` loaded, and that grants the container real input-injection rights on the host kernel. Weigh that before using it.
- GPU-accelerated rendering needs extra plumbing; the default is software rendering, so capture of 3D content is slow.

</details>

<details>
<summary><b>Python package</b> — for scripting</summary>

> **Not on PyPI yet.** Install it from the checkout alongside a local build:
>
> ```bash
> cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
> pip install -e bindings/python
> export COMPUTER_CONTROL_LIB=$PWD/build/libcomputer_control.dylib   # .so on Linux
> ```

```python
from computer_control import Session

with Session() as cc:
    cc.screenshot("before.png", max_dimension=1200)
    cc.click(640, 480)
    cc.type("hello world", enter=True)
    cc.pinch(2.0, at=(700, 400))
```

The binding is pure `ctypes` over the C ABI — no compiled extension, no build step, and it works on CPython and PyPy alike. Point `COMPUTER_CONTROL_LIB` at a local build to use one:

```bash
COMPUTER_CONTROL_LIB=$PWD/build/libcomputer_control.dylib python my_script.py
```

**Advantages** — the nicest API of the four, no compiler needed, integrates with pytest and notebooks.

**Limitations** — roughly 30 ms of interpreter startup, and each call crosses the ctypes boundary (microseconds, so irrelevant next to the OS calls themselves). Until a wheel is published you need a local build for the library to point at. Note that the OS permission belongs to the **Python interpreter**, not to `cc` — see the [binding README](bindings/python/README.md).

</details>

---

## Use as an MCP server

`computer-control-mcp` speaks the Model Context Protocol over stdio, which is what Claude Desktop, Claude Code, Cursor and Zed launch.

```json
{
  "mcpServers": {
    "computer-control": {
      "command": "computer-control-mcp"
    }
  }
}
```

Hardened, for a client you do not fully trust:

```json
{
  "mcpServers": {
    "computer-control": {
      "command": "computer-control-mcp",
      "args": ["--no-shell", "--tools", "capabilities,displays,screenshot,snapshot,zoom,click,type,key,scroll"]
    }
  }
}
```

Over HTTP on loopback, with a bearer token:

```bash
CC_AUTH_TOKEN=$(openssl rand -hex 16) computer-control-mcp --transport http --port 8765
```

31 tools exist; 30 are advertised by default, because `registry` stays hidden unless you pass `--allow-registry`. A tool that is switched off is not listed at all rather than listed and refusing, so the model's attention is not spent on it.

`capabilities` first, then `snapshot` to get numbered elements, then act on them by label rather than by pixel. `batch` runs a predictable sequence in one round trip, which is usually the difference between a snappy agent and a sluggish one.

---

## Permissions

The OS gates this deliberately. Each platform fails in its own way, and every error from this library carries a `remedy` naming the exact setting.

### macOS

Run this first — it tells you exactly what is wrong and how to fix it:

```bash
cc permissions            # what the OS is allowing
cc permissions --request  # prompt for anything missing, and open the right pane
```

Two grants are needed, both under **System Settings → Privacy & Security**:

- **Accessibility** — synthetic input and the element tree.
- **Screen & System Audio Recording** — screenshots.

Both are read at process launch, so **restart the process after granting**. Toggling while it runs does nothing.

#### Why the binary may not appear in the list

This is the part that wastes an afternoon, so it is worth stating plainly.

macOS attributes a permission to the **responsible process**, not to the binary that asks. A command-line tool started from a terminal is attributed to *the terminal*. Three things follow:

1. `cc` never appears in the Accessibility list, so there is nothing to enable.
2. The permission prompt never fires — `AXIsProcessTrusted()` already returns true because the terminal is granted, and macOS only prompts a process it considers untrusted.
3. That inherited grant covers the *trust check* but not always real inspection. You get `AXIsProcessTrusted() == true` while every window comes back as an empty placeholder.

`cc permissions` detects all three and names the owner:

```
Permissions for this process are attributed to iTerm2, not to the binary
itself, which is why
  /usr/local/bin/cc
does not appear in System Settings.
```

Two ways to fix it:

- **Add the binary by hand.** In the Accessibility list, click **+** and select the exact path `cc permissions` printed. Restart the process.
- **Use the app bundle** (better, because the grant survives rebuilds):

  ```bash
  cmake --build build --target macos_bundle
  open -n build/computer-control.app --args permissions --request
  ```

  A bundle launched through LaunchServices is its own responsible process, so it prompts properly and appears in the list as **computer-control**, where you can enable it. Running the binary inside the bundle directly from a shell does *not* do this — it is a child of the shell again, and `cc permissions` will say so.

**Signing matters for persistence.** An ad-hoc signature is keyed to the code hash, so every rebuild is a new identity and the grant is lost. Pass a real certificate to keep it:

```bash
cmake -S . -B build -DCC_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
```

A self-signed certificate from Keychain Access works too and costs nothing.

When the MCP server is launched by Claude Desktop, the responsible process is Claude Desktop — so its grants apply and there is usually nothing to do.

`CGDisplayCreateImage` and `CGWindowListCreateImage` are **removed**, not merely deprecated, in the macOS 15 SDK. Capture uses ScreenCaptureKit, which needs macOS 12.3+.

### Windows

No permission prompts, but two rules:

- **UIPI**: synthetic input to a window running at a higher integrity level is silently discarded. To drive an elevated app, run elevated.
- The process sets per-monitor DPI awareness (V2) at startup. Without it Windows reports a virtualised 96-DPI desktop and every click on a scaled monitor lands wrong.

### Linux

X11 works out of the box. Two things to know:

**Wayland**: XTest reaches XWayland clients only — native Wayland apps will not see synthetic input. The `uinput` path works everywhere because it is kernel-level. This library detects a Wayland session and tells you rather than silently doing nothing.

**Multi-touch** needs write access to `/dev/uinput`:

```bash
sudo modprobe uinput
sudo groupadd -f uinput
sudo usermod -aG uinput "$USER"
echo 'KERNEL=="uinput", GROUP="uinput", MODE="0660", OPTIONS+="static_node=uinput"' \
  | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
# log out and back in
```

**Accessibility** uses AT-SPI2, loaded at runtime so its absence is never a build failure. Install `at-spi2-core`; GTK apps need `GTK_MODULES=gail:atk-bridge`, Qt needs `QT_ACCESSIBILITY=1`, Electron needs `--force-renderer-accessibility`.

---

## Driving phones and simulators

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

## Using the library directly

```cpp
#include <cc/session.hpp>

auto session = cc::Session::create().value();
auto input = session->input().value();

input->click(cc::Point{640, 480}, cc::ClickOptions{.count = 2});

cc::GestureRequest pinch;
pinch.kind = cc::GestureKind::Pinch;
pinch.center = cc::Point{700, 400};
pinch.scale = 2.0;
input->gesture(pinch);
```

The C ABI in [`include/cc/capi.h`](include/cc/capi.h) is the stable surface: enumerators are append-only, structs are versioned by a leading `size` field, nothing throws, and errors are thread-local.

---

## Safety

This library can do anything the user in front of the machine can do. Treat it accordingly.

- `--no-shell` removes command execution; `--tools` restricts the surface to exactly what you list.
- Registry writes are **off** unless `--allow-registry` is passed.
- Bind HTTP to loopback. Binding elsewhere without `CC_AUTH_TOKEN` prints a warning — anything that can reach the port controls the machine.
- The session releases every held button, key and touch contact on shutdown, including on the error path, so an interrupted drag never leaves the desktop stuck. `release_all` recovers manually.
- Nothing is sent anywhere. There is no telemetry, no analytics and no network access beyond what you explicitly ask for.

**Never commit credentials.** `.gitignore` excludes tokens, keys and captured images by default — screenshots routinely contain password managers, private messages and customer data.

[SECURITY.md](SECURITY.md) has the threat model and how to report a vulnerability.

---

## Project layout

```
include/cc/        Public headers; capi.h is the stable C ABI
src/core/          Platform-independent: coordinate math, motion and gesture
                   geometry, JSON, PNG/JPEG codecs, deflate
src/platform/      macos/ (CGEvent, ScreenCaptureKit, AX)
                   windows/ (SendInput, InjectTouchInput, UIA, GDI)
                   linux/ (XTest, uinput, EWMH, AT-SPI2)
src/devices/       Simulator, emulator and mirrored-device transports
src/capi/          C ABI plus the shared action dispatcher
src/mcp/           MCP server (stdio and HTTP)
src/cli/           The `cc` command
bindings/python/   ctypes binding
```

Every front-end funnels through one action dispatcher (`src/capi/actions.cpp`), so the CLI, the MCP server and `cc_batch` cannot drift apart.

---

## Contributing

`CLAUDE.md` documents the architecture and the invariants that are easy to break. In short: coordinates always carry a space; anything held must be released on every path; a capability that is emulated must say so; and every error carries a remedy.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j && ctest --test-dir build --output-on-failure
```

---

## Documentation

| | |
|---|---|
| [Coordinate spaces](docs/coordinate-spaces.md) | Logical vs physical vs image, mixed-DPI, device points. Read this one. |
| [JSON schema](docs/json-schema.md) | The shape of every JSON payload the C ABI and MCP tools return. |
| [Tool comparison](docs/tool-comparison.md) | What carried over from Windows-MCP and macOS-MCP, what was dropped, and why. |
| [CLAUDE.md](CLAUDE.md) | Architecture and the invariants that break subtly. |
| [Docker notes](docker/README.md) | What the container can and cannot do. |
| [examples/](examples/) | Runnable: capabilities, coordinate spaces, gestures, devices, the C++ API. |

## Prior art

The tool surface is a superset of [Windows-MCP](https://github.com/CursorTouch/Windows-MCP) and [macOS-MCP](https://github.com/CursorTouch/MacOS-MCP), both MIT-licensed and worth reading. This project differs in being a single C++ core across three desktop platforms with an explicit coordinate-space model, honest gesture-fidelity reporting, and mobile-device support.

## License

MIT — see [LICENSE](LICENSE).
