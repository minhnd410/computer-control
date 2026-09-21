<!-- Split out of the README; see the table of contents there. -->
# Architecture

This is the current source layout and runtime flow. Repository invariants are
listed in [CLAUDE.md](../CLAUDE.md).

```
include/cc/*.hpp     Public C++ API.
src/core/            Platform-independent: coordinate math, motion and gesture
                     geometry, JSON, PNG/JPEG codecs, deflate, UTF-8.
                     No OS headers.
src/platform/macos/    CGEvent, ScreenCaptureKit, AXUIElement
src/platform/windows/  SendInput, InjectTouchInput, UI Automation, GDI
src/platform/linux/    XTest, /dev/uinput, EWMH, AT-SPI2
src/devices/         Simulator, emulator and mirrored-device transports.
src/actions/         The single action dispatcher: JSON in, JSON out.
src/mcp/             MCP server. A schema wrapper over the dispatcher, and the
                     only executable this project produces.
tests/               Unit and protocol tests.
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

`Session` creates each backend on first use. A screenshot-only call therefore
does not initialize the accessibility backend.

## Coordinates

`DisplayGraph` resolves every conversion against the display that contains the
point, because a mixed-DPI setup has no single scale factor. `Session::resolve()`
is the chokepoint for anything user-supplied and rejects points outside the
desktop rather than emitting a click that silently goes nowhere.
[Coordinate spaces](coordinate-spaces.md) has the model.

## Runtime dependencies

The executable uses the platform APIs and libraries listed in
[installation](install.md#runtime-requirements). JSON, image codecs, and
deflate support are included in the source tree; system zlib is used when
available.

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
