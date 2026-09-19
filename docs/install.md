<!-- Split out of the README; see the table of contents there. -->
# Installation

One binary, `computer-control-mcp`. No runtime, no dependencies to install
alongside it. Pick the first method that applies to you; building from source
is the last resort, not the default.

- [Homebrew](#homebrew-macos-and-linux) — macOS and Linux
- [winget](#winget-windows) — Windows
- [Download an archive](#download-an-archive) — anywhere
- [Build from source](#build-from-source) — contributors, or a platform with no archive
- [After installing](#after-installing)

## Homebrew (macOS and Linux)

```bash
brew install minhnd410/tap/computer-control
```

That pulls the prebuilt archive for your platform from the release and puts
`computer-control-mcp` on your `PATH`. It does not compile anything.

Upgrading:

```bash
brew update && brew upgrade computer-control
```

`brew upgrade` on its own will often say **`already installed`** right after a
release, and then work if you run it again. That is not a broken formula. The
first run evaluates the formula it already has, *then* auto-update refreshes
the tap — too late for that invocation, but in time for the next one. Running
`brew update` first separates the two steps and avoids the dance.

`brew info computer-control` settles it either way: it shows `0.7.1 → stable
0.7.3` when an upgrade is genuinely available.

The formula lives in [minhnd410/homebrew-tap](https://github.com/minhnd410/homebrew-tap)
and is rewritten by CI on every release, with the checksums CI computed — so the
tap cannot lag the release or carry a hand-typed digest.

**Linux on arm64 has no prebuilt archive.** The formula says so rather than
installing something wrong; build from source there.

## winget (Windows)

```powershell
winget install minhnd410.computer-control
```

Windows needs no permission grants for any of this. The one thing to know is
that input aimed at a window running at a higher integrity level — an
installer, Task Manager, anything launched as administrator — is silently
discarded by UIPI. If a click into such a window appears to do nothing, restart
the server elevated. `computer-control-mcp --doctor` reports which case you are in.

## Download an archive

Every release attaches an archive per platform plus a `.sha256` beside it.

| Platform | Archive |
|---|---|
| macOS, Apple silicon | `computer-control-macos-arm64.tar.gz` |
| macOS, Intel | `computer-control-macos-x86_64.tar.gz` |
| Linux, x86_64 | `computer-control-linux-x86_64.tar.gz` |
| Windows, x86_64 | `computer-control-windows-x86_64.zip` |

```bash
BASE=https://github.com/minhnd410/computer-control/releases/latest/download
curl -fsSLO "$BASE/computer-control-macos-arm64.tar.gz"
curl -fsSLO "$BASE/computer-control-macos-arm64.tar.gz.sha256"
shasum -a 256 -c computer-control-macos-arm64.tar.gz.sha256   # or sha256sum -c
tar -xzf computer-control-macos-arm64.tar.gz
sudo mv computer-control/computer-control-mcp /usr/local/bin/
```

Keep the archive's original filename — that is the name recorded in the
`.sha256`. The checksum step is not decoration: this is a binary that can drive
your desktop.

**The archives are not code-signed or notarised.** On macOS, Gatekeeper
quarantines them (`xattr -d com.apple.quarantine computer-control-mcp` clears
it) and, more importantly, an unsigned binary's TCC identity is its code hash —
so the Accessibility grant does not survive an upgrade. Homebrew has the same
property. If you want a grant that sticks, build from source and sign it.

## Build from source

Requirements: CMake 3.20+ and a C++20 compiler (AppleClang 14+, MSVC 19.30+,
GCC 11+, Clang 14+).

```bash
git clone https://github.com/minhnd410/computer-control.git
cd computer-control
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Produces `build/computer-control-mcp`, the static library
`libcomputer_control.a` (`computer_control_static.lib` on Windows) and the
public headers.

System packages:

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

Options: `-DCC_BUILD_MCP=OFF` (library only), `-DCC_ENABLE_NATIVE=ON` (tune for
this CPU; not for redistributable builds), `-DCC_USE_SYSTEM_ZLIB=OFF`,
`-DCC_BUILD_APP_BUNDLE=OFF`.

### A signed macOS bundle

Worth the trouble only if you are tired of re-granting Accessibility after
every rebuild. A grant follows the *responsible process*, and for a binary
started from a terminal that is the terminal — so it never appears in System
Settings on its own. An app bundle opened through LaunchServices is its own
responsible process:

```bash
cmake -S . -B build -DCC_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
cmake --build build --target macos_bundle
open -n build/computer-control.app --args --request-permissions
```

With an ad-hoc signature the grant is keyed to the code hash and is lost on
every rebuild, which is why a real identity matters here. See
[permissions](permissions.md).

## After installing

```bash
computer-control-mcp setup
```

That is the whole of it. `setup` detects the MCP clients on the machine, asks
which should get the server, writes each one's config, and then requests the OS
permissions. It is a command rather than something the installer did because
**neither Homebrew nor winget can prompt** — a formula's post-install runs with
no terminal attached, and winget's portable installer has no hook at all.

It edits only its own entry, and refuses to write to a config it cannot parse
rather than replacing it. Safe to re-run.

```bash
computer-control-mcp setup --list              # every client, and where its config lives
computer-control-mcp setup --client cursor     # no prompts
computer-control-mcp setup --no-permissions    # config only
computer-control-mcp --doctor                  # what this host can do
computer-control-mcp --list-tools
```

Clients it knows: Claude Code, Claude Desktop, VS Code / GitHub Copilot,
Cursor, Windsurf, Codex CLI and Zed. For anything else, [MCP server](mcp.md)
has the config shape and the transport options.

## Not available yet

| | Status |
|---|---|
| `uvx` | Nothing published to PyPI, and there is no Python in this project to publish. |
| `curl \| sh` | Script exists in `packaging/scripts/install.sh`; Homebrew covers the same platforms better. |
| Docker | Removed. A container cannot reach the host's display server, so it could never control a real desktop — it was a sandbox presented as an install method. |
| Linux arm64 archive | No CI runner for it yet. Build from source. |

---

[← README](../README.md)
