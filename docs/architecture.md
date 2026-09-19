<!-- Split out of the README; see the table of contents there. -->
# Architecture

For contributors. [CLAUDE.md](../CLAUDE.md) has the invariants that break
subtly if you get them wrong; this is the map.

```
include/cc/*.hpp     Public C++ API.
src/core/            Platform-independent: coordinate math, motion and gesture
                     geometry, JSON, PNG/JPEG codecs, deflate, UTF-8.
                     No OS headers here, ever.
src/platform/macos/    CGEvent, ScreenCaptureKit, AXUIElement
src/platform/windows/  SendInput, InjectTouchInput, UI Automation, GDI
src/platform/linux/    XTest, /dev/uinput, EWMH, AT-SPI2
src/devices/         Simulator, emulator and mirrored-device transports.
src/actions/         The single action dispatcher: JSON in, JSON out.
src/mcp/             MCP server. A schema wrapper over the dispatcher, and the
                     only executable this project produces.
tests/               A ~60-line harness; no test framework dependency.
```

Each platform directory holds the same six backends — display, input, screen,
window, a11y, system — behind the interfaces in `include/cc/`. Adding a
platform means implementing those six, not touching anything above.

## One dispatcher

`src/actions/actions.cpp` is the whole surface. Every capability is one
`ActionSpec` in the registry there: a name, a title, a description, a JSON
Schema for its arguments, and two behaviour flags. Adding a tool means adding
an entry and implementing its handler.

The MCP tool list, its schemas, and `--list-tools` are all generated from that
registry, so they cannot drift from what the code actually accepts. Several
tests read the registry directly for the same reason — the documented tool
counts are checked against it, and every schema is validated.

## Lazy backends

`Session` creates each backend on first use. This is load-bearing on macOS: a
caller that only wants screenshots must never trigger an accessibility prompt,
because the prompt is attributed to the responsible process and asking for a
permission you do not need is how a user ends up denying one you do.

## Coordinates

`DisplayGraph` resolves every conversion against the display that contains the
point, because a mixed-DPI setup has no single scale factor. `Session::resolve()`
is the chokepoint for anything user-supplied and rejects points outside the
desktop rather than emitting a click that silently goes nowhere.
[Coordinate spaces](coordinate-spaces.md) has the model.

## No dependencies

JSON, PNG, JPEG and deflate are all in-tree. That is deliberate: the selling
point is a binary you drop somewhere and run, on three platforms, with no
runtime to install. System zlib is used when found and the bundled encoder
otherwise.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Useful while iterating:

```bash
./build/computer-control-mcp --doctor       # capabilities, backends, permissions
./build/computer-control-mcp --list-tools   # the action registry
```

The suite is hermetic except for `test_display.cpp`, which reads the real
display topology, and a few `exec` tests that spawn `/bin/echo`. Neither needs
a granted permission.

---

[← README](../README.md)
