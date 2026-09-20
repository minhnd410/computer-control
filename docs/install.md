<!-- Split out of the README; see the table of contents there. -->
# Installation

One binary, `computer-control-mcp`. No runtime, no dependencies to install
alongside it. Pick the first method that applies to you; building from source
is the last resort, not the default.

- [Homebrew](#homebrew-macos-and-linux) — macOS and Linux
- [winget](#winget-windows) — Windows
- [One-line install](#one-line-install) — anywhere, no package manager
- [Download an archive](#download-an-archive) — anywhere
- [Build from source](#build-from-source) — contributors, or a platform with no archive
- [After installing](#after-installing)

## Homebrew (macOS and Linux)

```bash
brew install minhnd410/tap/computer-control
```

That pulls the prebuilt archive for your platform from the release and puts
`computer-control-mcp` on your `PATH`. On macOS it also installs the signed
`computer-control.app` bundle used by the shared service, so its Accessibility
grant survives Homebrew upgrades. It does not compile anything.

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

## One-line install

No package manager, no toolchain:

```bash
curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
```

```powershell
irm https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.ps1 | iex
```

Both fetch the archive for your platform, **verify its SHA256 against the
checksum published beside it**, and install the binary. A mismatch aborts; it
does not warn and continue. That matters more than usual here — piping a script
into a shell to install something that can drive your desktop is worth doing
carefully, and you should read the script before you run it.

Neither needs root. The shell script installs to `/usr/local` when that is
writable, uses `sudo` for the copy alone when it is not, and falls back to
`~/.local` when there is no `sudo`. The PowerShell script installs under
`%LOCALAPPDATA%` and edits only your user `PATH`. Override with `CC_PREFIX`,
and pin a version with `CC_VERSION=v0.8.1`.

Homebrew and winget are still the better choice where you have them: they know
how to upgrade and uninstall, and these scripts do not.

### What it still needs

The binary has no bundled runtime and no toolchain requirement, but it is
dynamically linked, so the platform has to supply a few things. Both scripts
check before copying anything and stop with the exact remedy rather than
installing something that cannot start.

| | |
|---|---|
| **The script itself** | `curl`, `tar`, and `shasum` or `sha256sum`. Present by default on macOS and on every mainstream Linux; if you are piping this from `curl` you already have the one that is ever missing. |
| **macOS** | **macOS 14 or newer.** Screen capture uses `SCScreenshotManager`, which does not exist before 14, so the binary is built with a matching deployment target and dyld refuses to load it on anything older. No frameworks to install — everything else it links is part of the OS. |
| **Linux** | X11 client libraries: `libX11`, `libXtst`, `libXrandr`, `libXfixes`, and `zlib`. A desktop install has these already; a server image or a slim container has none of them.<br><br>`sudo apt install libx11-6 libxtst6 libxrandr2 libxfixes3 zlib1g`<br>`sudo dnf install libX11 libXtst libXrandr libXfixes zlib`<br>`sudo pacman -S libx11 libxtst libxrandr libxfixes zlib`<br><br>These are the runtime packages, not the `-dev` ones under [Build from source](#build-from-source). |
| **Windows** | Nothing. |
| **Linux on arm64** | No archive is published; `install.sh` refuses rather than installing the x86_64 one. Build from source. |

None of this replaces the macOS permission grants — those are the same whatever
you install with, and `computer-control-mcp setup` walks through them.

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

### Getting a Developer ID certificate

Signing is what stops each release costing the user a fresh Accessibility row
and a re-grant: an unsigned binary's TCC identity is its code hash, so every
version is a new program, while a Developer ID identity is stable across them.

The certificate has to be minted by you — it authenticates as your developer
account and can sign software in your name. Two scripts remove everything
around that:

```bash
# 1. A CSR, if you do not already have one
mkdir -p ~/apple-signing && chmod 700 ~/apple-signing
openssl genrsa -out ~/apple-signing/devid.key 2048
chmod 600 ~/apple-signing/devid.key
openssl req -new -key ~/apple-signing/devid.key -out ~/apple-signing/devid.csr \
  -subj "/emailAddress=you@example.com/CN=Your Name/C=XX"

# 2. An App Store Connect API key, from
#    App Store Connect > Users and Access > Integrations > Keys.
#    It needs the Admin role; a Developer-role key cannot create certificates.
export ASC_KEY_ID=XXXXXXXXXX
export ASC_ISSUER_ID=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee
export ASC_KEY=~/Downloads/AuthKey_XXXXXXXXXX.p8

# 3. Turn it into the CI secrets
./packaging/macos/prepare-signing.sh ~/apple-signing/devid.key \
  ~/apple-signing/developerID_application.cer
```

**The certificate itself cannot be created through the App Store Connect API.**
Team keys reach the provisioning endpoints but top out at the Admin role, and
Apple restricts `DEVELOPER_ID_APPLICATION` to the Account Holder — the request
comes back `403 FORBIDDEN_ERROR, "This operation can only be performed by the
Account Holder."` Individual keys are tied to a person but have no access to
provisioning endpoints at all, so no key satisfies both halves. Create it one
of these two ways:

**Xcode** — Settings → Accounts → your Apple ID → Manage Certificates → **+** →
Developer ID Application. Xcode generates the key and CSR itself and installs
the result. Export it from Keychain Access (right-click → Export) as a `.p12`,
then:

```bash
./packaging/macos/prepare-signing.sh ~/Downloads/signed.p12
```

**The portal** — developer.apple.com → Certificates, Identifiers & Profiles →
Certificates → **+** → Developer ID Application, signed in as the Account
Holder. Upload the CSR from step 1, download the `.cer`, and pass it with its
key as shown above.

A **paid** Apple Developer Program membership is required. A free account can
only issue Apple Development certificates, which sign locally but cannot be
notarised or distributed — `prepare-signing.sh` checks for that and says so
rather than producing a `.p12` that fails on the runner.

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

| Docker | Removed. A container cannot reach the host's display server, so it could never control a real desktop — it was a sandbox presented as an install method. |
| Linux arm64 archive | No CI runner for it yet. Build from source. |

---

[← README](../README.md)
