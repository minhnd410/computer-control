<!-- Split out of the README; see the table of contents there. -->
# Installation

Two ways to install today. Everything else is listed at the bottom as planned,
not available.

## Download a release

Every tag is built by CI for macOS, Windows and Linux, and the archives are
attached to the [release](https://github.com/minhnd410/computer-control/releases).
Nothing is uploaded by hand, so what you download is what CI built from that tag.

```bash
# macOS (Apple silicon) — swap macos-arm64 for your platform
curl -fsSL -o cc.tar.gz https://github.com/minhnd410/computer-control/releases/latest/download/computer-control-macos-arm64.tar.gz
tar -xzf cc.tar.gz
sudo mv computer-control/cc computer-control/computer-control-mcp /usr/local/bin/
```

| Platform | Archive |
|---|---|
| macOS, Apple silicon | `computer-control-macos-arm64.tar.gz` |
| macOS, Intel | `computer-control-macos-x86_64.tar.gz` |
| Linux, x86_64 | `computer-control-linux-x86_64.tar.gz` |
| Windows, x86_64 | `computer-control-windows-x86_64.zip` |

Each archive ships with a `.sha256` next to it. Verify before running a binary
that can drive your desktop:

```bash
shasum -a 256 -c computer-control-macos-arm64.tar.gz.sha256
```

Linux on arm64 has no prebuilt archive; build from source.

**The archives are not code-signed or notarised.** On macOS, Gatekeeper
quarantines them (`xattr -d com.apple.quarantine cc` clears it), and — more
importantly — an unsigned binary's TCC identity is its code hash, so the
Accessibility grant does not survive an upgrade. If you want the grant to
stick, build from source and sign it. See [permissions](permissions.md).

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

Produces `build/cc`, `build/computer-control-mcp`, the static library
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

Build options: `-DCC_BUILD_MCP=OFF`, `-DCC_BUILD_CLI=OFF`, `-DCC_ENABLE_NATIVE=ON`
(tune for this CPU; not for redistributable builds), `-DCC_USE_SYSTEM_ZLIB=OFF`.

## Coming soon

None of these work yet. The scaffolding is in
[`packaging/`](../packaging); what is missing is a published tap, a submitted
manifest, and a package index entry.

| | Status |
|---|---|
| `curl … \| sh` | Script exists in `packaging/scripts/install.sh`, untested against a real release. |
| Homebrew | Formula written; no tap published. |
| winget | Manifests written; not submitted to the community repository. |
| `uvx` | Nothing published to PyPI. |
| Docker | Removed. A container cannot reach the host's display server, so it could never control a real desktop — it was a sandbox pretending to be an install method. |

## Driving it from another language

There is no C ABI and no Python package. The CLI is the binding: every action
accepts `--raw` and returns exactly the JSON the matching MCP tool returns.

```bash
cc windows --raw | jq '.result.windows[] | select(.focused)'
cc snapshot --raw --vision false | jq '.result.elements[] | select(.role=="button")'
```

The [JSON schema](json-schema.md) documents those payloads, and is
additive-only.

---

[← README](../README.md)
