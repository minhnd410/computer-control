<!-- Split out of the README; see the table of contents there. -->
# Installation

Three ways to install, with real trade-offs. Pick by what you need, not by what looks easiest.

## Comparison

| | Prebuilt binary | Build from source | Docker |
|---|---|---|---|
| **Setup effort** | lowest | moderate | low |
| **Real desktop control** | yes | yes | **no** — container only |
| **Mobile simulators** | yes | yes | Android only |
| **Native multi-touch** | yes | yes | yes (with `--device /dev/uinput`) |
| **Startup time** | ~8 ms | ~8 ms | ~8 ms + container |
| **Customisable build flags** | no | yes | yes |
| **Signed / notarised** | not yet — see below | you sign it | n/a |
| **Best for** | trying it out, CI runners | contributors, custom flags | headless CI, sandboxed scraping |

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

Produces `build/cc`, `build/computer-control-mcp`, the static library `libcomputer_control.a` (`computer_control_static.lib` on Windows) and the public headers.

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

Useful options: `-DCC_BUILD_MCP=OFF`, `-DCC_BUILD_CLI=OFF`, `-DCC_ENABLE_NATIVE=ON` (tune for this CPU — do not use for redistributable builds), `-DCC_USE_SYSTEM_ZLIB=OFF` (use the bundled deflate instead).

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


---

---

## Package managers

These all exist in the repository; none is published yet, because publishing
needs a tagged release with a stable URL and a checksum.

## curl

```bash
curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
```

Prefers a release archive and falls back to building from source, which is the
only path today. `CC_PREFIX` changes the install location (default
`/usr/local`); it uses `sudo` only when the target is not writable, and says so
first. Read it before piping it to a shell — good practice generally, and this
one installs a tool that can control your desktop.

## Homebrew

```bash
# once a tap exists
brew install --HEAD minhnd410/tap/computer-control

# today, from the formula in this repository
brew install --build-from-source ./packaging/homebrew/computer-control.rb
```

## winget

```powershell
winget install computer-control
```

Manifests are in [`packaging/winget`](../packaging/winget). **Not yet
submitted** to the community repository. Until then, build from source on
Windows — and if you use Claude Code from WSL, note that the Windows build is
the one you need ([why](wsl.md)).

---

## Driving it from another language

There is no C ABI and no Python package: both were surface maintained for a use
case this project does not have. The CLI is the binding. Every action accepts
`--raw` and returns exactly the JSON the matching MCP tool returns:

```bash
cc windows --raw | jq '.result.windows[] | select(.focused)'
cc snapshot --raw --vision false | jq '.result.elements[] | select(.role=="button")'
```

The [JSON schema](json-schema.md) documents those payloads, and it is
additive-only.

---

[← README](../README.md)
