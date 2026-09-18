<!-- Split out of the README; see the table of contents there. -->
# Installation

Four ways to install, with real trade-offs. Pick by what you need, not by what looks easiest.

## Comparison

| | Prebuilt binary | Build from source | Docker | Python package |
|---|---|---|---|---|
| **Setup effort** | lowest | moderate | low | low |
| **Real desktop control** | yes | yes | **no** — container only | yes |
| **Mobile simulators** | yes | yes | Android only | yes |
| **Native multi-touch** | yes | yes | yes (with `--device /dev/uinput`) | yes |
| **Startup time** | ~5 ms | ~5 ms | ~5 ms + container | ~30 ms (ctypes) |
| **Customisable build flags** | no | yes | yes | no |
| **Signed / notarised** | not yet — see below | you sign it | n/a | n/a |
| **Best for** | trying it out, CI runners | contributors, custom flags | headless CI, sandboxed scraping | scripting, notebooks |

<details>
<summary><b>Prebuilt binary</b> — fastest path, no toolchain</summary>

> **No tagged release yet.** Until there is one, the closest thing is the build artifacts attached to every green CI run: open the latest run under [Actions](https://github.com/minhnd410/computer-control/actions/workflows/ci.yml), scroll to **Artifacts**, and download the archive for your platform. Building from source is the supported path for now.

Once extracted, put `cc` and `computer-control-mcp` on your `PATH`:

```bash
sudo mv cc computer-control-mcp /usr/local/bin/
```

**Advantages** — no compiler, no dependencies, one file to delete when you are done. Starts in milliseconds, which matters when an agent invokes it repeatedly.

**Limitations** — you get the flags CI chose. CI artifacts are **not code-signed or notarised**, so macOS Gatekeeper will quarantine them (`xattr -d com.apple.quarantine cc` to clear it) and the Accessibility grant will not persist across downloads, because an unsigned binary's TCC identity is its code hash. If you intend to keep the grant, build from source with `CC_CODESIGN_IDENTITY` set — see [Permissions](permissions.md#macos).

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

Produces `build/cc`, `build/computer-control-mcp`, `libcomputer_control.{a,so,dylib,dll}` and the public headers.

Platform packages:

```bash

---

## curl

```bash
curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
```

Prefers a release archive and falls back to building from source, which is the
only path today. `CC_PREFIX` changes the install location (default
`/usr/local`); it uses `sudo` only if the target is not writable, and says so
first. Read it before piping it to a shell - that is good practice generally,
and this one can install a tool that controls your desktop.

## Homebrew

```bash
brew install --HEAD minhnd410/tap/computer-control
```

The formula lives at [`packaging/homebrew`](../packaging/homebrew). There is no
tap published yet, so for now:

```bash
brew install --build-from-source ./packaging/homebrew/computer-control.rb
```

## winget

```powershell
winget install computer-control
```

Manifests are in [`packaging/winget`](../packaging/winget). **Not yet
submitted** to the community repository - that needs a tagged release with a
stable URL and SHA256. Until then, build from source on Windows.

## uvx / pipx

```bash
uvx computer-control-mcp          # run the MCP server without installing
pipx install computer-control     # or install the CLI
```

These resolve to console entry points that exec the native binary; the Python
package deliberately does not reimplement the server. **No wheel is published
yet**, so today this needs a local build with `COMPUTER_CONTROL_BIN` pointing
at it. See the [Python binding README](../bindings/python/README.md).

---

[← README](../README.md)
