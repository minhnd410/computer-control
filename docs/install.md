# Installation

`computer-control-mcp` is distributed as one native executable. Choose a
package manager when available; use the installer or an archive otherwise.

## Homebrew

macOS and Linux:

```bash
brew install minhnd410/tap/computer-control
```

Upgrade with:

```bash
brew update && brew upgrade computer-control
```

## winget

Windows:

```powershell
winget install minhnd410.computer-control
```

## Installer scripts

macOS and Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
```

Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.ps1 | iex
```

The scripts select the platform archive, verify its SHA-256 checksum, and
install `computer-control-mcp` for the current user or system. Set `CC_PREFIX`
to choose an installation directory. Set `CC_VERSION=v0.8.12` to install a
specific release.

## Release archives

Archives and checksums are attached to each [release](https://github.com/minhnd410/computer-control/releases).

| Platform | Archive |
|---|---|
| macOS Apple silicon | `computer-control-macos-arm64.tar.gz` |
| macOS Intel | `computer-control-macos-x86_64.tar.gz` |
| Linux x86_64 | `computer-control-linux-x86_64.tar.gz` |
| Windows x86_64 | `computer-control-windows-x86_64.zip` |

Linux arm64 archives are not published. Build from source on that platform.

## Runtime requirements

| Platform | Requirement |
|---|---|
| macOS | macOS 14 or newer. |
| Windows | Windows 10 or newer. |
| Linux | X11 client libraries and zlib: `libX11`, `libXtst`, `libXrandr`, `libXfixes`, and `zlib`. |

Debian or Ubuntu runtime packages:

```bash
sudo apt install libx11-6 libxtst6 libxrandr2 libxfixes3 zlib1g
```

Fedora:

```bash
sudo dnf install libX11 libXtst libXrandr libXfixes zlib
```

Arch:

```bash
sudo pacman -S libx11 libxtst libxrandr libxfixes zlib
```

The Linux build can automate X11 and XWayland applications. Native Wayland
input requires the `/dev/uinput` device and appropriate permissions.

## Build from source

Requirements: CMake 3.20+ and a C++20 compiler. On Linux, install the
development packages:

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libx11-dev libxtst-dev libxrandr-dev libxfixes-dev zlib1g-dev
```

Build and test:

```bash
git clone https://github.com/minhnd410/computer-control.git
cd computer-control
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The server is `build/computer-control-mcp`. The static library and public
headers are also produced. Useful options include:

- `-DCC_BUILD_MCP=OFF` builds the library without the server.
- `-DCC_ENABLE_NATIVE=ON` enables CPU-specific optimizations.
- `-DCC_USE_SYSTEM_ZLIB=OFF` uses the bundled deflate implementation.

## First setup

After installation, configure MCP clients and inspect the host:

```bash
computer-control-mcp setup
computer-control-mcp --doctor
computer-control-mcp --list-tools
```

`setup --list` shows detected clients and their config paths. Use
`setup --client <names>` for a non-interactive configuration, or
`setup --no-permissions` to write configuration without requesting OS access.

See [MCP usage](mcp.md), [permissions](permissions.md), and [WSL usage](wsl.md)
for client and platform-specific setup.

---

[<- README](../README.md)
