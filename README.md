# computer-control

[![CI](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml/badge.svg)](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey.svg)

**Let a model drive your desktop — and the phones on it.**

One small binary, no runtime, no dependencies. Point any MCP client at it and
the model gets 33 tools: screenshots, clicks, typing, multi-touch gestures,
window and app control, the accessibility tree, and any iOS simulator, Android
emulator or mirrored handset visible on screen. macOS, Windows and Linux.

---

## Install

### macOS and Linux — Homebrew

```bash
brew install minhnd410/tap/computer-control
computer-control-mcp setup
```

### Windows — winget

```powershell
winget install minhnd410.computer-control
computer-control-mcp setup
```

### Any platform — download

Grab the archive for your platform from
[Releases](https://github.com/minhnd410/computer-control/releases), verify the
checksum beside it, and put `computer-control-mcp` on your `PATH`.

### Building it yourself

Only if you want to change it, or you are on a platform with no archive:
[docs/install.md](docs/install.md).

---

## Setup

`computer-control-mcp setup` does the rest. It finds the MCP clients you
already have, asks which should get the server, writes each one's config, and
then requests the OS permissions it needs.

```
$ computer-control-mcp setup

Found these MCP clients. Which should get computer-control?

  1) Claude Code
  2) Claude Desktop
  3) VS Code / GitHub Copilot
  4) Codex CLI

Enter numbers separated by spaces, 'a' for all, or Enter to skip:
```

It only ever touches its own entry, so the servers already in those files are
left alone — and it refuses to write at all to a config it cannot parse, rather
than replacing your work with a fresh file. Run it again whenever you add a
client.

Non-interactively, or to change your mind later:

```bash
computer-control-mcp setup --list                 # every client and its config path
computer-control-mcp setup --client cursor,zed    # no prompts
computer-control-mcp --doctor                     # permissions and capabilities
```

On **macOS two permissions are still required** — Accessibility for input and
the element tree, Screen Recording for captures. `setup` asks for them. If one
is missing, tools now refuse with a message naming the settings pane rather
than appearing to work; `--doctor` shows the current state. See
[permissions](docs/permissions.md).

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
| **Menus** | Read an application's whole menu bar and invoke any item by path — including commands with no button and no shortcut. macOS for now. |
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

## License

MIT — see [LICENSE](LICENSE).
