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

31 tools exist; 30 are advertised by default, because `registry` stays hidden unless you pass `--allow-registry`. A tool that is switched off is not listed at all rather than listed and refusing, so the model's attention is not spent on it.

`capabilities` first, then `snapshot` to get numbered elements, then act on them by label rather than by pixel. `batch` runs a predictable sequence in one round trip, which is usually the difference between a snappy agent and a sluggish one.

---

---

## Running under WSL

A Linux build inside WSL cannot control the Windows desktop. Install the
Windows build and have WSL launch it - see [WSL setup](wsl.md).

## Tools

31 exist; 30 are advertised by default, because `registry` stays hidden unless
you pass `--allow-registry`. A disabled tool is not listed at all rather than
listed and refusing, so the model's attention is not spent on it.

`computer-control-mcp --list-tools` prints them all, marking which the current
safety flags would hide.

---

[← README](../README.md)
