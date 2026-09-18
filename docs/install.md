<!-- Split out of the README; see the table of contents there. -->
# Installation

Build from source. Nothing is published yet — the bottom of this page lists
what is planned and what is missing for each.

> **No release is published.** There are no prebuilt binaries to download
> today, so building from source is the only way to install. The pipeline
> exists (`.github/workflows/release.yml`) and attaches archives with SHA256
> checksums for macOS, Windows and Linux the moment a `v*` tag is pushed. The
> archives it produces are **not** code-signed or notarised.

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

Build options: `-DCC_BUILD_MCP=OFF` (library only), `-DCC_ENABLE_NATIVE=ON`
(tune for this CPU; not for redistributable builds), `-DCC_USE_SYSTEM_ZLIB=OFF`.

## Coming soon

None of these work yet. The scaffolding is in
[`packaging/`](../packaging); what is missing is a published tap, a submitted
manifest, and a package index entry.

| | Status |
|---|---|
| Prebuilt binaries | The release workflow is written and tested end to end; no tag is currently published. |
| `curl … \| sh` | Script exists in `packaging/scripts/install.sh`, untested against a real release. |
| Homebrew | Formula written; no tap published. |
| winget | Manifests written; not submitted to the community repository. |
| `uvx` | Nothing published to PyPI. |
| Docker | Removed. A container cannot reach the host's display server, so it could never control a real desktop — it was a sandbox pretending to be an install method. |

## What ships

One binary, `computer-control-mcp`, plus a static library for embedding. There
is no CLI, no C ABI and no Python package.

To drive this from another language, speak MCP to the server — it is a
line-delimited JSON-RPC conversation on stdin and stdout, which any language
can hold without a client library:

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{
  "name":"windows","arguments":{},
  "_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",
           "io.modelcontextprotocol/clientCapabilities":{}}}}' \
  | computer-control-mcp | jq '.result.structuredContent.windows[] | select(.focused)'
```

The [JSON schema](json-schema.md) documents those payloads, and is
additive-only.

---

[← README](../README.md)
