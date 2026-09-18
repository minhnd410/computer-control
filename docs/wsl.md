# Claude Code in WSL

Short answer: **install the Windows build and have WSL launch it.** A Linux
binary inside WSL cannot control Windows, and no amount of configuration will
change that.

## Why

WSLg is one-directional by design. Microsoft's
[architecture post](https://devblogs.microsoft.com/commandline/wslg-architecture/)
describes Linux GUI apps connecting to a Weston compositor inside a separate
CBL-Mariner *system distro*, which streams individual windows to Windows over
RDP:

> The system distro as a containerized Linux environment in which we are
> running Weston and friends and projecting the various server sockets back
> into the user distro. … We decided to go with this approach for WSLg as it
> allows us to isolate WSLg from the user distro.

So a Linux process in WSL sees a Wayland/X11 session containing **only Linux
GUI apps**. It cannot enumerate Windows windows, capture the Windows screen, or
inject input into Windows applications. The Linux build of this library running
in WSL will happily start, report a display, and automate the WSLg session —
which is almost certainly not what you wanted.

## The setup

Claude Code runs in WSL; the MCP server runs on Windows. WSL interop lets a
Linux process launch a Windows executable with its stdin and stdout piped,
which is exactly what an MCP stdio server needs.

**1. Install on the Windows side.** From PowerShell:

```powershell
winget install computer-control
```

or build it there (see the [build instructions](../README.md#install)). Note
the Windows path, e.g. `C:\Users\you\scoop\shims\computer-control-mcp.exe`.

**2. Point Claude Code at the `.exe` from inside WSL.** In
`~/.claude/settings.json` (or your client's MCP config):

```json
{
  "mcpServers": {
    "computer-control": {
      "command": "/mnt/c/Users/you/scoop/shims/computer-control-mcp.exe"
    }
  }
}
```

Use the `/mnt/c/...` path, not the `C:\...` one — WSL resolves it and interop
does the rest. Check it runs before wiring it up:

```bash
/mnt/c/Users/you/scoop/shims/computer-control-mcp.exe --version
```

If that prints nothing, interop is disabled. Enable it in `/etc/wsl.conf`:

```ini
[interop]
enabled = true
appendWindowsPath = true
```

then `wsl --shutdown` from PowerShell and reopen.

## Things that will catch you out

**Paths cross a boundary.** The server is a Windows process, so every path it
receives or returns is a Windows path. `cc screenshot --out /home/you/s.png`
writes to `C:\home\you\s.png` on the Windows filesystem, not your WSL home.
Translate with `wslpath`:

```bash
OUT=$(wslpath -w ~/shot.png)          # /home/you/shot.png -> \\wsl.localhost\...
computer-control-mcp.exe ...          # hand it $OUT
```

Simplest is to write to a Windows directory and read it back through `/mnt/c`.

**The shell tool runs PowerShell on Windows**, not bash in your distro. If you
want the WSL shell, call `wsl.exe -e bash -c "..."` through it.

**Permissions are the Windows ones.** No TCC, no `/dev/uinput` — see the
[UIPI note](../README.md#windows). Driving an elevated Windows app needs the
server running elevated, which WSL cannot do for it.

## If you actually want to automate Linux GUI apps in WSLg

Then the Linux build is right, and it works normally inside the distro:

```bash
sudo apt install libx11-6 libxtst6 libxrandr2 libxfixes3
cc displays        # reports the WSLg session
```

Gestures fall back to emulation there: WSL2 has no `/dev/uinput` unless you
build a custom kernel with `CONFIG_INPUT_UINPUT`.

## Running both

There is no conflict in registering two servers, one per side:

```json
{
  "mcpServers": {
    "windows-desktop": { "command": "/mnt/c/Users/you/bin/computer-control-mcp.exe" },
    "wsl-desktop":     { "command": "/usr/local/bin/computer-control-mcp" }
  }
}
```

Name them distinctly, or you will spend an afternoon wondering why the clicks
land nowhere.

---

*Not yet verified end to end by a maintainer — the architecture is from
Microsoft's documentation and the interop path is standard, but nobody has run
this exact configuration. If you do,
[say so](https://github.com/minhnd410/computer-control/issues); it would be a
genuinely useful report.*
