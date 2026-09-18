# CLAUDE.md

Guidance for Claude Code and other agents working in this repository.

## What this is

An MCP server for desktop and mobile-simulator automation on macOS, Windows and
Linux, on a C++20 core. **MCP is the primary contract**, spoken at revision
2026-07-28 with a 2025-06-18 fallback.

**`computer-control-mcp` is the only binary.** There was a `cc` CLI mirroring
the same action registry; it was removed because it added no capability and
doubled what had to be installed, signed, documented and kept in step. Anything
an operator needs must therefore be reachable from the server binary itself —
that is why `--doctor` and `--request-permissions` exist. Do not reintroduce a
second front-end without a concrete consumer. The static library is there for
embedding.

There is deliberately no C ABI and no Python binding. They existed, and were
removed as surface maintained for a use case this project does not have. If
something needs to drive this from another language, it runs the CLI with
`--raw` and parses the JSON, which is byte-for-byte what the MCP tool returns.
Do not reintroduce a second binding without a concrete consumer.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # or ./build/cc_tests
```

Run a single check while iterating:

```bash
./build/computer-control-mcp --doctor            # capabilities, backends, permissions
./build/computer-control-mcp --list-tools        # the action registry
```

The test suite is hermetic except for `test_display.cpp`, which reads the real display topology, and a few `exec` tests that spawn `/bin/echo`. Neither needs a granted permission.

## Architecture

```
include/cc/*.hpp     Public C++ API.
src/core/            Platform-independent. No OS headers here, ever.
src/platform/<os>/   One backend per subsystem per OS. Same six files each:
                     display, input, screen, window, a11y, system.
src/devices/         iOS/Android/mirrored device transports.
src/actions/         The single action dispatcher: JSON in, JSON out.
src/mcp/             MCP server. A schema wrapper over the dispatcher, and
                     the only executable this project produces.
```

Adding a capability means editing **one** place: add an `ActionSpec` to the registry in `src/actions/actions.cpp` and implement it. The MCP tool list and CLI help are generated from that registry.

Backends are created lazily by `Session`. A caller that only wants screenshots must never trigger an accessibility prompt.

## Invariants

These are the things that break subtly if you get them wrong.

**1. Coordinates always carry a space.** `Space::Logical` (OS points, what input uses), `Space::Physical` (device pixels), `Space::Image` (pixels of the last capture). Never pass a bare number across a boundary. Conversion goes through `DisplayGraph`, which resolves per display — a mixed-DPI setup has no single scale factor. `Session::resolve()` is the chokepoint for anything user-supplied; it rejects points outside the desktop rather than emitting a click that silently goes nowhere.

**2. Anything held must be released on every path.** Mouse buttons, modifier keys, touch contacts. Use a scope guard (`ModifierGuard` on Windows/Linux) rather than a matching call at the end of the function, because the early-return path is the one that leaves a user's desktop with Ctrl stuck down. `Session`'s destructor calls `release_all()`; so does the MCP server when the client disconnects mid-drag.

**3. Emulation must announce itself.** If a platform cannot do the real thing, `gesture_support()` returns `Emulated` with the substitute named in `backend` and the caveat in `note`. Never quietly substitute. `require_native` exists so a caller can refuse.

**4. Every error carries a remedy.** `Error{code, message, remedy}`. The message says what happened; the remedy says what to do, naming the exact System Settings pane, package, or udev rule. An error without a remedy is only acceptable when there genuinely is no action to take.

**5. Nothing throws out of the dispatcher.** `actions::run` catches everything, so a malformed JSON shape is an error response rather than a crashed server. An exception reaching the stdio loop takes the MCP session down with no diagnostic the client can show.

**6. Budgets on every tree walk.** Accessibility APIs are cross-process IPC; an unresponsive app can hang a walk indefinitely. Every walk honours a wall-clock deadline and a node cap, and reports `truncated` with a reason rather than returning a plausible-looking partial tree.

**7. Never truncate text by bytes.** printf's `%.28s` cuts mid-character in UTF-8, and one split character invalidates a whole JSON document — which on the stdio transport drops the connection with a parse error nowhere near the cause. Use `text::truncate_utf8` / `text::pad_utf8`. `json::escape` also sanitises as a backstop, but the backstop is not the fix.

**8. stdout belongs to the MCP protocol.** On the stdio transport, any stray write corrupts the stream and the client drops the connection with an opaque parse error. Log to stderr.

**9. The server speaks two protocol eras, and must keep doing so.** 2026-07-28 is stateless: a request carrying `_meta` with a protocol version is served in isolation, and the server must not infer anything about it from earlier requests. 2025-06-18 is handshake-based, and whatever Claude Desktop and Cursor ship today still sends `initialize`, so dropping it breaks real setups. `legacy_session_` is the *only* per-connection state in the server; adding a second piece re-introduces the statefulness the revision removed. `src/mcp/protocol.cpp` decides the era, and `tests/test_mcp.cpp` drives both paths through the real dispatcher — add a case there rather than reasoning about the wire format from memory.

## Platform notes worth knowing before you debug

**macOS**
- `CGDisplayCreateImage` and `CGWindowListCreateImage` are *removed* in the macOS 15 SDK, not merely deprecated. ScreenCaptureKit is the only path.
- `AXIsProcessTrusted()` can report true while per-app inspection is still refused, returning placeholder elements whose role is `AXApplication` and whose `AXChildren` is `kAXErrorAttributeUnsupported`. `a11y_macos.mm` detects this and reports a permissions problem instead of an empty tree. Do not "fix" that by removing the check.
- Permission checks must be **functional**, never just `AXIsProcessTrusted()`. `permissions_macos.mm` actually reads a window from another process, because the trust flag and the capability disagree in exactly the case people hit.
- TCC attributes a grant to the *responsible process*, which for a CLI is the terminal. `responsibility_get_pid_responsible_for_pid` (resolved via `dlsym`, no public header) gives the owning pid; naming it is what turns an unexplainable failure into an obvious one. A CLI therefore cannot prompt itself into the Accessibility list — the app bundle target exists for that.
- CGEvent coordinates are **points**, not pixels.
- Multi-clicks need `kCGMouseEventClickState` set to 2 or 3; two fast single clicks are not a double click.
- `kCGWindowListOptionOnScreenOnly` means "on the active Space", which is why device discovery lists offscreen windows.

**Windows**
- Per-monitor DPI awareness V2 must be set before the first monitor query.
- Absolute mouse coordinates are 0–65535 over the **virtual** desktop (`SM_XVIRTUALSCREEN` and friends), not over the primary display.
- UIPI silently discards input aimed at a higher-integrity window. `send_failed()` names this on `ERROR_ACCESS_DENIED`.
- UIA without a `CacheRequest` issues one cross-process call per property. Always use `FindAllBuildCache`.

**Linux**
- XTest cannot produce touch contacts; `/dev/uinput` is the only route to real multi-touch, and it works on Wayland too.
- X11 has no per-monitor scale. The order of precedence is `GDK_SCALE`/`QT_SCALE_FACTOR`, then `Xft.dpi`, then 1.0.
- Typing rebinds a spare keycode per character and restores it afterwards. If you change that path, make sure the restore happens on the failure path too.

## Documentation

The README is a hub: anything long lives in `docs/` and is linked from its
table of contents. Keep the README under roughly 250 lines — when a section
outgrows that, split it into `docs/` and leave a one-line pointer. When
splitting, watch for `#` inside fenced code blocks: a naive heading regex
treats `# Debian / Ubuntu` in a bash block as a section boundary and silently
drops everything after it.

**State what has actually been tested, and by whom.** The README's *What has
actually been tested* table is the project's credibility. Rules for it:

- A row means a human ran that code on that machine. CI compiling it is not
  testing it, and is reported separately.
- Name what was exercised, not "works". "4-finger swipe, PowerShell with
  quoted arguments" is a claim a reader can check; "gestures" is not.
- **When you change the code a row covers, the row is stale.** Either re-test
  it or mark it as predating the change. A row that silently keeps its claim
  across a rewrite is worse than no row.
- Behaviour that was attempted but not observed goes under *Partially
  verified* with what was and was not seen. Never round it up to working.
- Currently maintainer-tested: the author's macOS machine and one Windows 11
  machine. Linux is CI-compiled only. Keep asking for outside reports rather
  than quietly widening the claim.

**Do not hand-maintain a number that the code already knows.** The tool counts
in `docs/mcp.md` are checked against the registry by `tests/test_docs.cpp`,
which also fails if a tool is never mentioned there. Adding a tool therefore
breaks the build until the docs catch up, which is the point. If you write a
new counted claim, pin it the same way instead of trusting the next person to
remember.

## Style

Match the surrounding code. Notable conventions:

- Comments explain **why**, especially where the code looks odd because an OS API is odd. Do not narrate what the next line does.
- `Result<T>` / `Status` for anything that can fail. No exceptions in the core.
- 4-space indent, 100-column soft limit, `snake_case` functions, `PascalCase` types.
- No new third-party dependencies without a strong reason. JSON, PNG, JPEG and deflate are all in-tree precisely so the library stays dependency-free; system zlib is used when found and the bundled encoder otherwise.

## Security

This is a public repository for a tool that can drive a desktop.

- Never commit credentials, tokens, or screenshots. `.gitignore` excludes them by default; captures routinely contain password managers and private messages.
- Shell and registry access are policy-gated in `SessionConfig`. Keep new side-effectful capabilities behind a flag.
- Subprocesses take an argv vector and never a concatenated shell string. `devices::exec` enforces this; there is a test that a `;` in an argument is not interpreted.
