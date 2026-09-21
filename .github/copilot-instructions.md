# Computer Control Repository Instructions

This repository is `computer-control-mcp`, a cross-platform C++20 MCP server for desktop and mobile-simulator automation. The MCP server is the primary contract and `computer-control-mcp` is the only executable. Do not reintroduce a second CLI, C ABI, or language binding without a concrete consumer.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Useful focused checks:

```bash
./build/computer-control-mcp --doctor
./build/computer-control-mcp --list-tools
```

The suite is hermetic except for the display-topology test and a few tests that spawn `/bin/echo`.

## Architecture

- `include/cc/`: public C++ API.
- `src/core/`: platform-independent code; never include OS headers here.
- `src/platform/<os>/`: display, input, screen, window, accessibility, and system backends.
- `src/devices/`: iOS, Android, and mirrored-device transports.
- `src/actions/`: the single JSON action registry and dispatcher.
- `src/mcp/`: MCP protocol, schema wrapper, server, and stdio transport.

Add a capability by adding one `ActionSpec` to the registry in `src/actions/actions.cpp` and implementing it. MCP tools, CLI help, and schemas are generated from that registry. Backends are lazy: a screenshot request must not trigger an accessibility permission prompt.

## Non-negotiable invariants

1. Coordinates carry a `Space` (`Logical`, `Physical`, or `Image`). Convert through `DisplayGraph`; `Session::resolve()` is the boundary for user coordinates and rejects points outside the desktop.
2. Release every held mouse button, modifier, or touch contact on every path. Prefer scope guards. `Session` cleanup must release all held input.
3. Emulation is explicit: report `Emulated`, the substitute backend, and the caveat. Honor `require_native`.
4. Every error is `Error{code, message, remedy}`. The remedy names the exact setting, package, or rule to fix the issue when one exists.
5. Nothing escapes the dispatcher. Malformed input becomes an MCP error response, never a crashed server.
6. Accessibility walks have both a wall-clock deadline and node cap. Report truncation and its reason.
7. Never truncate UTF-8 by bytes. Use `text::truncate_utf8` and `text::pad_utf8`; JSON escaping is only a backstop.
8. stdout belongs to the MCP protocol. Logs go to stderr.
9. Preserve both MCP protocol eras: the stateless 2026-07-28 path and handshake-based 2025-11-25 / 2025-06-18 compatibility. `legacy_session_` is the only per-connection state.
10. Tool schemas must be complete and valid, including `items` for arrays.

## Output design

MCP responses are for language models first. Keep summaries short, deterministic, and action-oriented. Return structured fields for machine use, omit redundant prose and duplicate data, bound collections and source text, and expose pagination or truncation metadata whenever output is bounded. Preserve exact data needed to act, especially coordinate spaces, identifiers, error remedies, and image metadata. Add focused tests for response size, truncation, paging, and schema validity when changing output.

## Platform notes

- macOS: ScreenCaptureKit is required on macOS 15 SDKs; Accessibility permission checks must be functional, not only `AXIsProcessTrusted()`; Core Graphics coordinates are points.
- Windows: set per-monitor DPI awareness V2 before monitor queries; absolute input spans the virtual desktop; UIPI can silently reject higher-integrity input; use UIA `CacheRequest` / `FindAllBuildCache`.
- Linux: XTest cannot create touch contacts; use `/dev/uinput` for real multitouch; scale precedence is `GDK_SCALE` / `QT_SCALE_FACTOR`, then `Xft.dpi`, then 1.0; restore temporary keycodes on failure paths.

## Style and safety

Use `Result<T>` / `Status` for failures and do not throw from core paths. Match the existing 4-space, snake_case function, PascalCase type style and roughly 100-column width. Comments explain why, especially for odd OS behavior; do not narrate obvious code. Keep edits focused, preserve public APIs, avoid unrelated refactors and dependencies, and never commit credentials, tokens, or screenshots. Subprocesses use argv vectors, never concatenated shell strings. Do not write to stdout except through the MCP protocol.

## Documentation

Keep `README.md` focused on installation, client configuration, capabilities, tested status, safety, and links. Put mechanism and platform detail in `docs/`. Tool counts and tool mentions must stay synchronized with the action registry because documentation tests enforce them. State only behavior that has actually been tested on a named platform; stale tested claims must be updated when covered code changes.
