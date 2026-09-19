<!-- Split out of the README; see the table of contents there. -->
# Tool reference

33 tools exist; 32 are advertised by default, because `registry` stays hidden
unless you pass `--allow-registry`. A disabled tool is not listed at all rather
than listed and refusing, so the model's attention is not spent on it.

`computer-control-mcp --list-tools` prints the live list, marking which the
current safety flags would hide. The counts on this page are checked against
the registry by the test suite, so they cannot drift.

### Orientation

| Tool | What it does |
|---|---|
| `capabilities` | What this host can do: display scales, which backends came up, which permissions are missing, and which gestures are real rather than emulated. Run it first on an unfamiliar machine. |
| `permissions` | Check, and optionally request, the OS permissions needed. The one to run when macOS is silently dropping input. |
| `displays` | Every display with logical bounds, physical pixel bounds, scale factor and DPI. |

### Seeing

| Tool | What it does |
|---|---|
| `screenshot` | Capture the screen, one display, a window or a region, plus the mapping back to logical coordinates. |
| `snapshot` | Screenshot *and* accessibility tree, every interactive element given a number. The label is what later calls should target. |
| `zoom` | Re-capture a region at full resolution, to read text too small to survive downscaling. |
| `elements` | Query the accessibility tree with no screenshot: the whole tree, the element under a point, or the focused one. |
| `menu` | Read or invoke an application's menu bar. |
| `cursor_position` | Where the pointer is. |

### Menus

`menu` reads an application's menu bar through the accessibility API, which
means it sees the **entire** command surface — including commands that have no
toolbar button and no keyboard shortcut — and it reads without opening anything
on screen.

```jsonc
{"mode": "list"}                                  // frontmost app, top level + items
{"mode": "list", "depth": 2}                      // descend into submenus too
{"mode": "select", "path": ["File", "Save As…"]}  // or "File > Save As…"
```

Prefer this to clicking menus by pixel. A pixel click has to open the menu,
keep it open, and hit a target whose position depends on the window, the
theme and how many items are above it; a path does none of that. The listing
includes each item's keyboard shortcut, so a model can often skip the menu
entirely and press the chord instead.

Selecting a disabled item fails and says so rather than appearing to succeed —
menu items enable themselves based on context, so that usually means the
application is not in the state the command needs.

**macOS only so far.** Windows exposes menus through UI Automation and Linux
through AT-SPI, so both are implementable; neither is implemented, and `menu`
says that rather than returning an empty list that reads as "this application
has no menus".

### Pointer

| Tool | What it does |
|---|---|
| `move` | Move the pointer. `profile="human"` produces motion that passes hover and drag heuristics. |
| `click` | Click at a point or on a label. `count=0` hovers, `2` double-clicks, `3` triple-clicks. |
| `scroll` | Scroll at a point, optionally pixel-precise and phased for trackpad-style momentum. |
| `drag` | Press, travel, release as one uninterrupted stream, with a dwell after the press so drop targets register. |
| `stroke` | Freehand path with the button held: signatures, drawing, lasso selection. |
| `gesture` | Multi-touch: pinch, rotate, n-finger tap, swipe, pan, long press, force press, edge swipe. Fidelity differs per platform - see `capabilities`. |

### Keyboard

| Tool | What it does |
|---|---|
| `key` | A key or chord (`cmd+shift+a`, `F5`, `escape`). Space-separated chords run in sequence. |
| `key_hold` | Hold a chord for a duration, then release. |
| `key_down` / `key_up` | Explicit press and release, for the cases the two above cannot express. |
| `type` | Type text into the focused field, or into a labelled one. Unicode is injected directly rather than keystroke-mapped. |

### Windows and apps

| Tool | What it does |
|---|---|
| `windows` | List, activate, move, resize, restate or close windows. |
| `app` | List, launch, activate or quit applications. |
| `system` | The shell-level actions every desktop has but none exposes as an API: launcher, search, switch virtual desktop, window overview, show desktop. |
| `process` | List or terminate processes. |
| `notify` | Show a desktop notification. |

### Devices

| Tool | What it does |
|---|---|
| `device` | Drive an iOS simulator, Android emulator or mirrored phone, in the device's own point space. |

### Flow control

| Tool | What it does |
|---|---|
| `wait` | Pause for a number of milliseconds. |
| `wait_for` | Poll until a UI condition holds, inside one call - far cheaper than a snapshot loop from the client. |
| `batch` | Several actions in one call. Each round trip costs more than the actions themselves. |
| `release_all` | Release every held button, key and touch contact. Run it after an interrupted drag. |

### Gated

These three are off by default or restricted, because each one widens what a
model that has gone wrong can reach.

| Tool | What it does | Gate |
|---|---|---|
| `shell` | Run a command, with this process's privileges. | `--no-shell` removes it |
| `clipboard` | Read or write the clipboard. | `--no-clipboard` removes it |
| `registry` | Read or write the Windows registry; unsupported elsewhere. | hidden unless `--allow-registry` |

---

[← README](../README.md)
