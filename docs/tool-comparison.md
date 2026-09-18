# Tool surface vs. the reference projects

This project's tool surface was designed as a superset of
[Windows-MCP](https://github.com/CursorTouch/Windows-MCP) and
[macOS-MCP](https://github.com/CursorTouch/MacOS-MCP). Both are MIT-licensed
and worth reading; this table records what carried over, what was renamed, and
what is new.

## Coverage of Windows-MCP

| Windows-MCP tool | Here | Notes |
|---|---|---|
| `Click` | `click` | Adds triple-click, back/forward buttons, and modifier keys. |
| `Type` | `type` | Adds layout-independent Unicode and a rate limit. |
| `Scroll` | `scroll` | Adds pixel-precise and phased (trackpad-style) scrolling. |
| `Move` | `move`, `drag` | Split: `move` positions, `drag` is an explicit press-travel-release. |
| `Shortcut` | `key` | Adds chord sequences (`cmd+k cmd+s`) and repeat. |
| `Wait` | `wait` | Milliseconds rather than seconds. |
| `WaitFor` | `wait_for` | Same conditions, polled in-process. |
| `Snapshot` | `snapshot` | Same idea: screenshot plus labelled elements. |
| `Screenshot` | `screenshot` | Adds JPEG, region and window capture, explicit image-space mapping. |
| `App` | `app` | Same four modes. |
| `DisplayInventory` | `displays` | Adds both coordinate spaces per display. |
| `PowerShell` | `shell` | Generalised; `shell` selects the interpreter per platform. |
| `Clipboard` | `clipboard` | Same. |
| `Process` | `process` | Same. |
| `Notification` | `notify` | Same. |
| `Registry` | `registry` | Same; gated behind `--allow-registry`. |
| `FileSystem` | — | **Not carried over.** See below. |
| `Scrape` | — | **Not carried over.** See below. |
| `MultiSelect`, `MultiEdit` | `batch` | Subsumed: `batch` expresses both and anything else. |

## Coverage of macOS-MCP

| macOS-MCP tool | Here | Notes |
|---|---|---|
| `App` | `app` | `move` is folded into `windows(mode="bounds")`. |
| `Shell` | `shell` | `mode="osascript"` becomes `shell(shell="osascript")`. |
| `Snapshot` | `snapshot` | Same. |
| `Click`, `Type`, `Scroll`, `Move` | same names | As above. |
| `Shortcut` | `key` | Same. |
| `Wait` | `wait` | Same. |
| `Scrape` | — | **Not carried over.** |
| `Desktop` (Spaces) | `gesture` | A 3-finger swipe is the general form; on macOS it maps to the Spaces key equivalents. |
| `Notification` | `notify` | Same, including the sound argument. |

## Deliberately not carried over

**`FileSystem`** — reading and writing files needs no desktop and no special
permission, and every MCP client already has a filesystem server. Including it
here would widen the blast radius of a tool whose whole job is privileged
desktop access, for no capability gain. Use `shell`, or a filesystem server.

**`Scrape`** — fetching a URL is not desktop automation. It would put an
outbound network client inside a process that otherwise makes no network calls
at all, which is a property worth keeping. Use a fetch server, or drive a real
browser through this one.

## New here

| Tool | Why it exists |
|---|---|
| `capabilities` | Per-gesture fidelity, missing permissions, detected tooling. Nothing equivalent in either reference. |
| `permissions` | Functional permission probe plus the responsible-process diagnosis, and a request path. macOS-MCP prompts on startup; neither reference can explain why a grant that looks present does not work. |
| `gesture` | Multi-touch as a first-class operation: pinch, rotate, n-finger swipe and pan, force press, edge swipe. |
| `stroke` | Freehand paths with pressure, dwell and optional spline smoothing. Drawing, signatures, lasso selection. |
| `drag` | An explicit press-travel-release with the dwell and settle timing that drag-and-drop actually needs. |
| `zoom` | Re-capture a region at full resolution to read small text. |
| `device` | iOS simulators, Android emulators and mirrored handsets. |
| `elements` | The accessibility tree without paying for a screenshot. |
| `windows` | Window management as its own tool rather than a mode of `App`. |
| `key_hold`, `key_down`, `key_up` | Holding a key across other actions, for games and modifier-driven UIs. |
| `batch` | Several actions per round trip. |
| `release_all` | Recovery after an interrupted drag. |

## Structural differences

- **One C++ core, three platforms.** Both references are Python, one platform
  each. Here the platform-independent logic — coordinate conversion, gesture
  geometry, path interpolation, image codecs — is written once and unit-tested
  without a desktop.
- **Coordinate spaces are explicit.** Neither reference distinguishes points
  from pixels, which is why both have caveats about high-DPI displays.
- **Fidelity is reported.** `capabilities` states whether each gesture is
  native, emulated or unsupported, and names the substitution.
- **No telemetry.** Both references ship optional PostHog analytics. This has
  no network code outside what a tool call explicitly performs.
