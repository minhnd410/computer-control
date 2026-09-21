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

## Persistent server config

The server reads `~/.config/computer-control/config.json` when it exists. Use
`--config PATH` to select another file. Precedence is:

1. Built-in defaults
2. The JSON config file
3. `CC_AUTH_TOKEN`, `CC_NO_SHELL` and other environment settings
4. Explicit command-line flags

Example:

```json
{
  "server": {
    "transport": "http",
    "host": "127.0.0.1",
    "port": 8765,
    "auth_token": "replace-me",
    "enabled_tools": [],
    "disabled_tools": [],
    "log_requests": false
  },
  "session": {
    "allow_shell": false,
    "allow_filesystem": true,
    "allow_registry": false,
    "allow_clipboard": false,
    "block_when_locked": true,
    "prompt_for_permissions": false,
    "default_max_capture_dimension": 1600
  }
}
```

An omitted or empty `enabled_tools` list enables every tool. Use
`disabled_tools` or the session safety flags to narrow that default.

`computer-control-mcp setup` writes this file and accepts the server flags
needed to update it. On macOS the launchd service starts with `--config`, so the
saved transport, host, port, tool allowlist and session safety settings survive
service restarts. The generated token is also kept in
`~/.config/computer-control/token` for bridge clients; both files are written
with user-only permissions.

## macOS snapshot scope

An unrestricted macOS `snapshot` adaptively scans the frontmost application,
Finder desktop roots, reachable visible dialog owners, and a small system-UI
allowlist: Dock, Control Center, SystemUIServer menu-bar extras, Spotlight and
Notification Center. It also probes background applications and scans only
those that expose Accessibility menu-bar extras. Explicit `pid` snapshots keep
the older single-process behavior.

`capabilities` first, then `snapshot` to get numbered elements, then act on them by label rather than by pixel. `batch` runs a predictable sequence in one round trip, which is usually the difference between a snappy agent and a sluggish one.

---

## Protocol revisions

The server speaks **2026-07-28**, **2025-11-25** and **2025-06-18**. Which era a
request gets is decided by how it opens, not by a server setting:

| The client sends | It gets |
|---|---|
| `_meta` with `io.modelcontextprotocol/protocolVersion` in `params` or at the request level | that revision, served statelessly |
| `initialize` | the requested revision if supported, otherwise 2025-11-25 |
| Neither | legacy compatibility mode |

Clients that do not send either form of metadata are accepted in legacy
compatibility mode. The stdio bridge also adds minimal modern metadata before
forwarding those requests. A legacy `initialize` handshake is forwarded
unchanged. Requests that include a protocol version but omit the required
`clientCapabilities` field still return `-32602`.

The 2026-07-28 path is stateless: every request carries its own protocol
version and client capabilities. Older handshake clients are supported through
the compatibility path.

- `server/discover` is mandatory and answers without any handshake. It is also
  the compatibility probe - a recognisable answer tells a dual-era client this
  server speaks the modern protocol.
- Every result carries `resultType`, and `_meta` carries the server identity,
  because a stateless client never saw an `initialize` response to learn it
  from.
- List results (`server/discover`, `tools/list`) carry `ttlMs` and
  `cacheScope`. The tool set is fixed at startup, so it is publicly cacheable
  for an hour.
- Legacy `ping`, `logging/setLevel`, and handshake messages remain accepted for
  compatibility.

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

### Output schema

Tools return `structuredContent` alongside their text. Per-tool output schemas
are not advertised because collection and platform results vary with the
desktop state. The stable response envelope is documented in
[JSON payloads](json-schema.md).

### LLM-friendly results

The text content of a tool result is a short status summary. The complete
bounded payload is in `structuredContent`, so clients should prefer that field
instead of asking the model to parse repeated table-like prose. List results
include `returned`, `total`, and `truncated` when a result is bounded. Accessibility
trees also report `nodes_walked` and a truncation reason.

Successful tool results preserve each tool's existing fields and add
`structuredContent.ok: true` plus `structuredContent.action`, so a client can
route results without inspecting the prose summary. Batch results keep the
structured result for each step but omit repeated human-readable step text.

Shell and clipboard text defaults to 12,000 bytes per field. Pass
`max_output_bytes` when a larger bounded result is needed; the response reports
the original byte count and whether truncation occurred. All text truncation is
UTF-8 safe.

Failures set `isError` and keep the human content to one actionable message.
They also provide `structuredContent` with `ok: false`, the `action`, and an
`error` object containing stable `code`, `message`, and optional `remedy` fields.
Partial result data, such as timeout polling counts, is retained under
`structuredContent.result`.

---

## Tools

The full list, with what each one is for, is in the
[tool reference](tools.md). `computer-control-mcp --list-tools` prints the live
version, marking which tools the current safety flags would hide.

Start with `capabilities`, use `snapshot` to get numbered elements, and target
later input calls by label when possible. Use `batch` for predictable sequences.

---

## Running under WSL

A Linux build inside WSL cannot control the Windows desktop. Install the
Windows build and have WSL launch it - see [WSL setup](wsl.md).

---

[← README](../README.md)
