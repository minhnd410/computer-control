# computer-control

[![CI](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml/badge.svg)](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey.svg)

**One C++20 core for driving a desktop — and the phones on it.** macOS, Windows and Linux, plus iOS simulators, Android emulators and mirrored handsets, behind a single API that ships as a native library, a stable C ABI, a CLI, and an MCP server.

```bash
cc permissions --request                # ask the OS for what it needs
cc screenshot --out screen.png          # capture, DPI-correct
cc click --at 500,300@image             # click what you saw in that capture
cc system --action launcher             # open the launcher, whatever it is this year
cc device --mode tap --device "iPhone 15" --at 196,420
```

---

## Contents

- [Why this exists](#why-this-exists)
- [What it can do](#what-it-can-do)
  - [Gesture fidelity by platform](#gesture-fidelity-by-platform)
  - [What has actually been tested](#what-has-actually-been-tested)
- [Install](#install)
- [Use as an MCP server](docs/mcp.md)
- [Permissions](docs/permissions.md)
- [Driving phones and simulators](docs/devices.md)
- [Claude Code in WSL](docs/wsl.md)
- [Using the library directly](#using-the-library-directly)
- [Safety](#safety)
- [Contributing](#contributing) · [Project layout](#project-layout)
- [Full documentation index](#documentation)

---

## Why this exists

Desktop automation tools tend to pick one platform, one language, and one level of abstraction. This one makes three specific bets.

### Coordinate spaces are part of the type system

The single most common bug in this space is reading a pixel off a Retina screenshot and clicking it as if it were a point. Every coordinate here carries a space — `logical`, `physical`, or `image` — and conversion resolves against the display that contains it, so mixed-DPI multi-monitor setups convert per display rather than with one global factor.

![Coordinate spaces](docs/media/coordinates.gif)

### Fidelity is reported, not faked

Windows and Linux can synthesize genuine multi-touch. macOS cannot — there is no public API for it. Instead of silently substituting something that looks similar, `capabilities` tells you whether each gesture is `native`, `emulated`, or `unsupported`, names the backend, and explains the substitution. `require_native` refuses emulation outright.

![Gesture fidelity](docs/media/gestures.gif)

### Errors carry a remedy

Every failure names the exact next step — the settings pane, the package, the udev rule. The macOS permission model in particular is genuinely confusing: a grant belongs to the *responsible process*, so a CLI started from a terminal is attributed to the terminal, never appears in System Settings, and cannot prompt for itself. The tool says so rather than returning an empty result.

![Permission diagnosis](docs/media/permissions.gif)

Everything is written once in C++20 and exposed through a stable C ABI, so the Python binding, the CLI and the MCP server are thin shells over identical behaviour. There is no second implementation to drift.

---

## What it can do

| Area | Capabilities |
|---|---|
| **Pointer** | move, click (single/double/triple/hover), five buttons, modifier-clicks, press/release, scroll (line and pixel-precise, phased), drag-and-drop with correct press/settle timing |
| **Paths** | freehand strokes with optional Catmull-Rom smoothing, per-point pressure and dwell, pen routing where supported |
| **Motion** | `instant`, `linear`, `ease`, and `human` (jitter + overshoot-and-settle), seedable for reproducible paths |
| **Keyboard** | chords (`cmd+shift+a`), sequences (`cmd+k cmd+s`), hold-for-duration, explicit down/up, layout-independent Unicode (emoji, CJK) |
| **Gestures** | tap, n-finger swipe/pan, pinch, rotate, smart zoom, long press, force press, edge swipe — up to 10 contacts |
| **System** | launcher, search, app switcher, overview, desktop switching, notifications, capture UI, emoji, run dialog, settings, file manager, lock |
| **Capture** | full desktop, per-display, per-window, per-region; PNG and JPEG encoded in-process; automatic downscaling to a payload budget |
| **Windows** | list, activate, move, resize, minimise/maximise/fullscreen, close; fuzzy title matching |
| **Accessibility** | full element tree with numbered labels, element-at-point, focused element, invoke/toggle/set-value |
| **System services** | clipboard, process list/kill, shell, notifications, Windows registry |
| **Mobile** | discovery, boot/shutdown, tap/swipe/stroke/gesture, text, hardware buttons, screenshots, install/launch/terminate, deep links, device UI tree |
| **Permissions** | functional probe, request, and the responsible-process diagnosis |

### Gesture fidelity by platform

| Gesture | Windows | Linux | macOS |
|---|---|---|---|
| Tap, long press | native | native (uinput) / emulated | native |
| 2-finger swipe, pan | native | native (uinput) / emulated | **native** (phased scroll) |
| 3–5 finger swipe | shell shortcuts | native (uinput) | shell shortcuts |
| Pinch / zoom | native | native (uinput) / emulated | emulated (`cmd`+scroll) |
| Rotate | native | native (uinput) | **unsupported** |
| Force press | native | native (uinput) | emulated (long press) |

- **Windows** uses `InjectTouchInput` for real contacts, up to 10. But a **multi-finger *trackpad* swipe cannot be synthesized at all**: Windows interprets those in the Precision Touchpad driver from HID reports, and injected contacts are *touchscreen* input that goes to the window underneath. Those route to the shortcuts that driver invokes instead — four fingers switch virtual desktops, three switch apps.
- **Linux** creates a virtual multitouch touchscreen via `/dev/uinput`, which works on Wayland as well as X11. Without write access it falls back to XTest emulation.
- **macOS** has no public multi-touch synthesis. Two-finger swipe and pan are genuinely native because `CGEvent` exposes trackpad scroll phases; pinch maps to `cmd`+scroll; rotation has no honest equivalent and is refused.

Run `cc capabilities` for the live answer on any machine. For shell-level effects, prefer [`cc system`](docs/mcp.md) — it is both the reliable path and the fast one.

### What has actually been tested

Maintainer-tested:

| OS | Version | Tested by | Exercised |
|---|---|---|---|
| macOS | 26.6 (Tahoe), Apple silicon | maintainer | capture, pointer, clicks, drag, stroke, gestures, accessibility tree, permissions, launcher, simulator discovery |
| Windows | 11 | maintainer | PowerShell, multi-finger swipe behaviour |
| Linux | Debian 12 under Xvfb (container) | CI + maintainer | capture, pointer, clicks, Unicode typing, chords, emulated gestures |

Compiled and unit-tested on every push: macOS (arm64 + x86_64), Windows (MSVC), Linux (gcc + clang).

**Not yet exercised by anyone:** the Linux `/dev/uinput` native-gesture path (needs a host with a writable uinput device), macOS on Intel, any BSD, and every non-Debian distribution.

**Please help.** If you run this anywhere not in that table — another Windows build, a KDE or Wayland session, an Intel Mac, a Raspberry Pi, a physical Android phone over scrcpy — [open an issue](https://github.com/minhnd410/computer-control/issues) with the output of `cc doctor`. That is a genuinely useful contribution even if you change no code, and it is how the table above grows. See [CONTRIBUTING](CONTRIBUTING.md).

---

## Install

Four ways, with real trade-offs. Full detail and the comparison table: **[docs/install.md](docs/install.md)**.

```bash
# curl — builds from source today; there is no tagged release yet
curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh

# Homebrew (formula in-repo; no tap published yet)
brew install --build-from-source ./packaging/homebrew/computer-control.rb

# Windows (manifests in-repo; not yet submitted to winget-pkgs)
winget install computer-control

# from source — the supported path
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

> **Status:** no tagged release, no PyPI wheel, nothing code-signed. `uvx computer-control-mcp` and `winget install` will work once a release exists; today they need a local build. [docs/install.md](docs/install.md) says exactly what works now.

**Claude Code in WSL:** install the **Windows** build and have WSL launch it. A Linux binary inside WSL cannot control Windows — WSLg is one-directional. See [docs/wsl.md](docs/wsl.md).

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

This library can do anything the user in front of the machine can do.

- `--no-shell` removes command execution; `--tools` restricts the surface to exactly what you list; registry writes need `--allow-registry`.
- Bind HTTP to loopback. Binding elsewhere without `CC_AUTH_TOKEN` prints a warning — anything that can reach the port controls the machine.
- Every held button, key and touch contact is released on shutdown, including on the error path, so an interrupted drag never leaves the desktop stuck. `release_all` recovers manually.
- No telemetry, no analytics, no network access beyond what a tool call explicitly performs.

**Never commit screenshots.** They routinely contain password managers, private messages and customer data; `.gitignore` excludes images by default. [SECURITY.md](SECURITY.md) has the threat model.

---

## Project layout

```
include/cc/        Public headers; capi.h is the stable C ABI
src/core/          Platform-independent: coordinate math, motion and gesture
                   geometry, JSON, PNG/JPEG codecs, deflate, UTF-8
src/platform/      macos/ (CGEvent, ScreenCaptureKit, AX)
                   windows/ (SendInput, InjectTouchInput, UIA, GDI)
                   linux/ (XTest, uinput, EWMH, AT-SPI2)
src/devices/       Simulator, emulator and mirrored-device transports
src/capi/          C ABI plus the shared action dispatcher
src/mcp/ src/cli/  MCP server and the `cc` command
bindings/python/   ctypes binding
packaging/         curl installer, Homebrew formula, winget manifests
```

Every front-end funnels through one action dispatcher (`src/capi/actions.cpp`), so the CLI, the MCP server and `cc_batch` cannot drift apart.

---

## Contributing

Contributions are very welcome, and **testing on hardware I do not have is the most useful kind**. See [CONTRIBUTING.md](CONTRIBUTING.md); [CLAUDE.md](CLAUDE.md) documents the architecture and the invariants that break subtly.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j && ctest --test-dir build --output-on-failure
```

---

## Documentation

| | |
|---|---|
| [Install](docs/install.md) | All four methods, with trade-offs and current status. |
| [MCP server](docs/mcp.md) | Client config, transports, tool gating. |
| [Permissions](docs/permissions.md) | Per-platform grants, and the macOS responsible-process problem. |
| [Devices](docs/devices.md) | iOS simulators, Android emulators, mirrored handsets. |
| [WSL](docs/wsl.md) | Why the server must run on the Windows side. |
| [Coordinate spaces](docs/coordinate-spaces.md) | Logical vs physical vs image, mixed DPI, device points. |
| [JSON schema](docs/json-schema.md) | Every JSON payload the C ABI and MCP tools return. |
| [Tool comparison](docs/tool-comparison.md) | What came from Windows-MCP and macOS-MCP, and what did not. |
| [Docker](docker/README.md) | What the container can and cannot do. |
| [examples/](examples/) | Runnable: capabilities, coordinate spaces, gestures, devices, permissions. |
| [tools/make_demo_gif.py](tools/make_demo_gif.py) | Regenerates the GIFs above from real command output. |

---

## Prior art

The tool surface is a superset of [Windows-MCP](https://github.com/CursorTouch/Windows-MCP) and [macOS-MCP](https://github.com/CursorTouch/MacOS-MCP), both MIT-licensed and worth reading. This project differs in being a single C++ core across three desktop platforms with an explicit coordinate-space model, honest fidelity reporting, and mobile-device support.

## License

MIT — see [LICENSE](LICENSE).
