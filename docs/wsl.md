# Claude Code in WSL

Choose the binary for the desktop you want to control:

- Windows desktop: install the Windows build and launch it from WSL.
- WSLg desktop applications: install the Linux build inside the distro.

A Linux process in WSL cannot enumerate or control Windows applications. A
Windows process launched through WSL interop cannot control Linux WSLg
applications unless you run a separate Linux server for them.

## Control the Windows desktop

Install the Windows build from PowerShell:

```powershell
winget install minhnd410.computer-control
```

Register the Windows executable in the WSL-side MCP client configuration. Use
the `/mnt/c` path:

```json
{
  "mcpServers": {
    "computer-control-windows": {
      "command": "/mnt/c/Users/you/AppData/Local/Microsoft/WinGet/Links/computer-control-mcp.exe"
    }
  }
}
```

Verify interop and the executable:

```bash
/mnt/c/Users/you/path/to/computer-control-mcp.exe --version
```

If Windows interop is disabled, add this to `/etc/wsl.conf`, shut down WSL from
PowerShell, and reopen the distro:

```ini
[interop]
enabled = true
appendWindowsPath = true
```

The server's `shell` tool runs PowerShell on Windows. To run a Linux command,
invoke `wsl.exe` explicitly through that tool. Paths passed to the Windows
server are Windows paths; use `wslpath -w` when converting a WSL path.

## Control WSLg applications

Install the Linux runtime libraries inside the distro:

```bash
sudo apt install libx11-6 libxtst6 libxrandr2 libxfixes3 zlib1g
```

Then configure the Linux binary normally:

```bash
computer-control-mcp setup
computer-control-mcp --doctor
```

The Linux server controls the WSLg display, not the Windows desktop. Native
multitouch requires `/dev/uinput`; otherwise capabilities reports the available
emulation.

## Run both

Use distinct server names when controlling both desktops:

```json
{
  "mcpServers": {
    "windows-desktop": {
      "command": "/mnt/c/Users/you/path/to/computer-control-mcp.exe"
    },
    "wslg-desktop": {
      "command": "/usr/local/bin/computer-control-mcp"
    }
  }
}
```

---

[<- README](../README.md)
