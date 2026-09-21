# computer-control

[![CI](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml/badge.svg)](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey.svg)

**Cross-platform desktop and mobile automation over MCP.**

One native binary gives an MCP client screenshots, input, accessibility queries,
window and app control, gestures, shell and clipboard controls, and device
automation. It runs on macOS, Windows and Linux.

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

### No package manager

```bash
curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
```
```powershell
irm https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.ps1 | iex
```

Both verify the published SHA256 before installing, and abort on a mismatch.
Or take the archive straight from
[Releases](https://github.com/minhnd410/computer-control/releases).

Needs **macOS 14+**, Windows 10+, or a Linux desktop with the X11 client
libraries — [the full list](docs/install.md#runtime-requirements). No runtime,
no toolchain. The installers check first and stop with the remedy rather than
leaving you a binary that will not start.

### Build from source

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

  > [x] Claude Code
    [x] Claude Desktop
    [x] VS Code / GitHub Copilot
    [x] Codex CLI

  space toggles, a all, enter confirms, esc cancels
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

On **macOS**, Accessibility is required for input and the element tree, and
Screen Recording is required for captures. `setup` asks for them. If one is
missing, the affected tool returns an actionable error and `--doctor` shows the
current state. See
[permissions](docs/permissions.md).

Using **Claude Code inside WSL**? Install the *Windows* build and have WSL
launch it. A Linux binary inside WSL cannot reach the Windows desktop:
[WSL setup](docs/wsl.md).

---

## Current surface

The server exposes the live action registry as MCP tools. See the [tool
reference](docs/tools.md) for the complete list, arguments, platform support,
safety gates, and examples.

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

## Verification status

Every push builds the native server and runs the test suite on macOS arm64,
macOS x86_64, Windows with MSVC, and Linux with GCC and Clang under Xvfb.
Platform capabilities and permissions still depend on the desktop where the
server runs. Use `computer-control-mcp --doctor` to inspect the active host.

For a manual platform report, include the output of `--doctor` when opening an
[issue](https://github.com/minhnd410/computer-control/issues).

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
