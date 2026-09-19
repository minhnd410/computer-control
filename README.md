# computer-control

[![CI](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml/badge.svg)](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey.svg)

**Let a model drive your desktop — and the phones on it.**

One small binary, no runtime, no dependencies. Point any MCP client at it and
the model gets 32 tools: screenshots, clicks, typing, multi-touch gestures,
window and app control, the accessibility tree, and any iOS simulator, Android
emulator or mirrored handset visible on screen. macOS, Windows and Linux.

---

## Install

### macOS and Linux — Homebrew

```bash
brew install minhnd410/tap/computer-control
computer-control-mcp --request-permissions
```

### Windows — winget

```powershell
winget install minhnd410.computer-control
```

### Any platform — download

Grab the archive for your platform from
[Releases](https://github.com/minhnd410/computer-control/releases), verify the
checksum beside it, and put `computer-control-mcp` on your `PATH`.

### Building it yourself

Only if you want to change it, or you are on a platform with no archive:
[docs/install.md](docs/install.md).

---

## Point your client at it

<details open>
<summary><b>Claude Code</b></summary>

```bash
claude mcp add computer-control computer-control-mcp
```
</details>

<details>
<summary><b>Claude Desktop, Cursor, Zed</b></summary>

```json
{
  "mcpServers": {
    "computer-control": { "command": "computer-control-mcp" }
  }
}
```
</details>

<details>
<summary><b>VS Code / GitHub Copilot</b> — <code>mcp.json</code></summary>

```json
{
  "servers": {
    "computer-control": { "type": "stdio", "command": "computer-control-mcp" }
  }
}
```
</details>

On **macOS you must grant two permissions** or every tool will appear to work
and do nothing. Run `computer-control-mcp --request-permissions`, then
`--doctor` to confirm. If something is still wrong, `--doctor` names the exact
settings pane — see [permissions](docs/permissions.md).

Using **Claude Code inside WSL**? Install the *Windows* build and have WSL
launch it. A Linux binary inside WSL cannot reach the Windows desktop:
[why](docs/wsl.md).

---

## What it can do

| | |
|---|---|
| **See** | Screenshot the desktop, a display, a window or a region. `snapshot` adds the accessibility tree with every clickable element numbered. `zoom` re-reads small text at full resolution. |
| **Point** | Move, click (single, double, triple, hover), five buttons, scroll by line or by pixel, drag and drop, freehand strokes. |
| **Type** | Chords like `cmd+shift+a`, sequences, hold-for-duration, and Unicode typed directly — emoji and CJK do not depend on your keyboard layout. |
| **Touch** | Pinch, rotate, n-finger swipe and pan, long press, force press, edge swipe. Up to 10 contacts where the OS allows it. |
| **Manage** | List, focus, move, resize and close windows. Launch and quit apps. Open the launcher, switch desktops, show notifications. |
| **Phones** | Drive an iOS simulator, Android emulator or mirrored handset in its own coordinate space. |

Two things worth knowing before you trust it with anything:

**Coordinates carry a space.** Reading a pixel off a Retina screenshot and
clicking it is the most common way to click the wrong thing. Every point here
is tagged `logical`, `physical` or `image`, and conversion happens per display,
so a mixed-DPI setup works. Pass a coordinate straight back from a screenshot
as `{"x":…,"y":…,"space":"image"}` and it lands where you meant.
[More](docs/coordinate-spaces.md).

**Emulation announces itself.** macOS has no public multi-touch API, so a pinch
there is `cmd`+scroll and a rotate is refused outright rather than faked. Ask
`capabilities` — or run `computer-control-mcp --doctor` — and it tells you which
gestures are real on the machine in front of you. [Per-platform
table](docs/gestures.md).

---

## What has actually been tested

A row here means a person ran that code on that machine. CI compiling it is not
the same thing and is listed separately.

| OS | Tested by | Exercised |
|---|---|---|
| macOS 26.6, Apple silicon | maintainer | capture, pointer, clicks, drag, stroke, gestures, accessibility tree, permissions, launcher, search, shell, simulator discovery |
| Debian 12 under Xvfb | maintainer, in a container since removed | capture, pointer, clicks, Unicode typing, chords, emulated gestures |
| Windows 11 | maintainer, **before the current code** | PowerShell and trackpad swipes were both found broken here and rewritten; **the rewrites have never run on Windows** |

Compiled and unit-tested on every push: macOS (arm64 + x86_64), Windows (MSVC),
Linux (gcc + clang, under Xvfb).

**Not exercised by anyone:** the Windows backend since those rewrites, the Linux
`/dev/uinput` native-gesture path, macOS on Intel, any BSD, any non-Debian
distribution.

**Running it anywhere not in that table is the most useful contribution you can
make** — even with no code. [Open an
issue](https://github.com/minhnd410/computer-control/issues) with the output of
`computer-control-mcp --doctor`. See [CONTRIBUTING](CONTRIBUTING.md).

---

## Safety

This binary can do anything you can do at the keyboard.

- `--no-shell` removes command execution, `--no-clipboard` removes clipboard
  access, and `--tools a,b,c` restricts the surface to exactly what you list. A
  disabled tool is not advertised at all, so the model never sees it.
- Keep HTTP on loopback. Cross-origin requests are refused, but anything that
  can reach the port controls the machine.
- Every held button, key and touch contact is released on shutdown, including
  on the error path, so an interrupted drag never leaves your desktop stuck.
- No telemetry, no analytics, no network access beyond what a tool call
  explicitly performs.

[SECURITY.md](SECURITY.md) has the threat model.

---

## Documentation

| | |
|---|---|
| [Install](docs/install.md) | Every method, per platform, and building from source. |
| [MCP server](docs/mcp.md) | Client config, protocol revisions, transports, tool gating. |
| [Tool reference](docs/tools.md) | All 32 tools and what each is for. |
| [Permissions](docs/permissions.md) | Per-platform grants, and the macOS responsible-process trap. |
| [Gestures](docs/gestures.md) | What is real and what is emulated, per platform. |
| [Coordinate spaces](docs/coordinate-spaces.md) | Logical, physical, image; mixed DPI; device points. |
| [Devices](docs/devices.md) | iOS simulators, Android emulators, mirrored handsets. |
| [WSL](docs/wsl.md) | Why the server must run on the Windows side. |
| [JSON payloads](docs/json-schema.md) | Every structured result the tools return. |
| [Embedding](docs/embedding.md) | Using the C++ library directly. |
| [Architecture](docs/architecture.md) | How the pieces fit, for contributors. |
| [Tool comparison](docs/tool-comparison.md) | What came from Windows-MCP and macOS-MCP, and what did not. |

---

## Prior art

The tool surface is a superset of
[Windows-MCP](https://github.com/CursorTouch/Windows-MCP) and
[macOS-MCP](https://github.com/CursorTouch/MacOS-MCP), both MIT-licensed and
worth reading. This project differs in being one C++ core across three desktop
platforms with an explicit coordinate-space model, honest fidelity reporting,
and mobile-device support.

## License

MIT — see [LICENSE](LICENSE).
