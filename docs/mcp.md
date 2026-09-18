<!-- Split out of the README; see the table of contents there. -->
# Use as an MCP server

`computer-control-mcp` speaks the Model Context Protocol over stdio, which is what Claude Desktop, Claude Code, Cursor and Zed launch.

```json
{
  "mcpServers": {
    "computer-control": {
      "command": "computer-control-mcp"
    }
  }
}
```

Hardened, for a client you do not fully trust:

```json
{
  "mcpServers": {
    "computer-control": {
      "command": "computer-control-mcp",
      "args": ["--no-shell", "--tools", "capabilities,displays,screenshot,snapshot,zoom,click,type,key,scroll"]
    }
  }
}
```

Over HTTP on loopback, with a bearer token:

```bash
CC_AUTH_TOKEN=$(openssl rand -hex 16) computer-control-mcp --transport http --port 8765
```

`capabilities` first, then `snapshot` to get numbered elements, then act on them by label rather than by pixel. `batch` runs a predictable sequence in one round trip, which is usually the difference between a snappy agent and a sluggish one.

---

## Protocol revisions

The server speaks **2026-07-28**, **2025-11-25** and **2025-06-18**. Which era a
request gets is decided by how it opens, not by a server setting:

| The client sends | It gets |
|---|---|
| `_meta` with `io.modelcontextprotocol/protocolVersion` | that revision, served statelessly |
| `initialize` | the requested revision if supported, otherwise 2025-11-25 |
| Neither | `-32602`, naming the field that is missing |

2025-11-25 is the newest handshake-based revision and is what clients actually
send today — Claude Code opens with exactly it. Everything it added over
2025-06-18 is optional (icons, experimental tasks), client-side (elicitation,
sampling tool calls), OAuth for HTTP, or a clarification; the one requirement
that lands on a server like this is Origin validation, below.

2026-07-28 made the protocol stateless. There is no handshake: every request
carries its own protocol version and client capabilities, so the server holds
no per-connection state and a request can be served in isolation. In practice:

- `server/discover` is mandatory and answers without any handshake. It is also
  the compatibility probe - a recognisable answer tells a dual-era client this
  server speaks the modern protocol.
- Every result carries `resultType`, and `_meta` carries the server identity,
  because a stateless client never saw an `initialize` response to learn it
  from.
- List results (`server/discover`, `tools/list`) carry `ttlMs` and
  `cacheScope`. The tool set is fixed at startup, so it is publicly cacheable
  for an hour.
- `ping`, `logging/setLevel` and the handshake itself were removed. This server
  still answers all three, because a 2025-06-18 client sends them and has no
  way to discover they are gone.

Error codes follow the spec's reserved range:

| Code | Meaning |
|---|---|
| `-32020` | `Mcp-Method` or `Mcp-Name` contradicts the request body (HTTP only) |
| `-32022` | The requested protocol version is not one of the three above |
| `-32602` | A required `_meta` field is missing; the message names which |

### Over HTTP

There is no session id. `Mcp-Method` and `Mcp-Name` let a gateway route and
authorise a call without parsing the body, and this server rejects a request
whose headers contradict its body with `-32020` - otherwise a caller could
declare a harmless method in the headers and smuggle a different one past the
gateway. The headers are validated when present rather than required: an absent
header cannot contradict anything, and requiring them would break pre-2026
clients for no gain on a loopback socket. Responses carry
`MCP-Protocol-Version`.

**Origin is validated.** A request whose `Origin` is not loopback gets `403`,
as 2025-11-25 requires. This is the only thing standing between a web page and
a server that drives the desktop: any site the user visits can make their
browser POST to a loopback port, and DNS rebinding defeats the usual
same-origin assumptions. A browser always sends `Origin`; a non-browser client
normally sends none, so an absent header is allowed and a cross-origin one is
not. The host is matched in full, so `http://localhost.evil.com` is refused
rather than passing a prefix check.

### No `outputSchema`

Tools return `structuredContent` alongside their text, but do not declare an
`outputSchema`. The spec makes the schema a promise: declare one and every
result must validate against it. Several tools here return shapes that vary
with what they found on screen, and a schema that is subtly wrong is worse for
a client than no schema at all - it turns a readable payload into a validation
error. This will change per-tool as individual shapes are pinned down, not in
one sweep.

---

## Running under WSL

A Linux build inside WSL cannot control the Windows desktop. Install the
Windows build and have WSL launch it - see [WSL setup](wsl.md).

## Tools

32 tools exist; 31 are advertised by default, because `registry` stays hidden
unless you pass `--allow-registry`. A disabled tool is not listed at all rather
than listed and refusing, so the model's attention is not spent on it.
`computer-control-mcp --list-tools` prints them all, marking which the current
safety flags would hide.

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
| `cursor_position` | Where the pointer is. |

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
