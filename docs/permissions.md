<!-- Split out of the README; see the table of contents there. -->
# Permissions

The OS gates this deliberately. Each platform fails in its own way, and every error from this library carries a `remedy` naming the exact setting.

## macOS

Run this first — it tells you exactly what is wrong and how to fix it:

```bash
computer-control-mcp --doctor                # what the OS is allowing
computer-control-mcp --request-permissions   # prompt for anything missing, then report
```

Two grants are needed, both under **System Settings → Privacy & Security**:

- **Accessibility** — synthetic input and the element tree.
- **Screen & System Audio Recording** — screenshots.

Both are read at process launch, so **restart the process after granting**. Toggling while it runs does nothing.

### Why the binary may not appear in the list

This is the part that wastes an afternoon, so it is worth stating plainly.

macOS attributes a permission to the **responsible process**, not to the binary that asks. A command-line tool started from a terminal is attributed to *the terminal*. Three things follow:

1. The binary never appears in the Accessibility list, so there is nothing to enable.
2. The permission prompt never fires — `AXIsProcessTrusted()` already returns true because the terminal is granted, and macOS only prompts a process it considers untrusted.
3. That inherited grant covers the *trust check* but not always real inspection. You get `AXIsProcessTrusted() == true` while every window comes back as an empty placeholder.

`computer-control-mcp --doctor` detects all three and names the owner:

```
Permissions for this process are attributed to iTerm2, not to the binary
itself, which is why
  /usr/local/bin/computer-control-mcp
does not appear in System Settings.
```

Two ways to fix it:

- **Add the binary by hand.** In the Accessibility list, click **+** and select the exact path `--doctor` printed. Restart the process.
- **Use the app bundle** (better, because the grant survives rebuilds):

  ```bash
  cmake --build build --target macos_bundle
  open -n build/computer-control.app --args permissions --request
  ```

  A bundle launched as the shared service is its own responsible process, so it prompts properly and appears in the list as **computer-control-mcp**, where you can enable it. Running the binary inside the bundle directly from a shell does *not* do this — it is a child of the shell again, and `--doctor` will say so.

**Signing matters for persistence.** An ad-hoc signature is keyed to the code hash, so every rebuild is a new identity and the grant is lost. Pass a real certificate to keep it:

```bash
cmake -S . -B build -DCC_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
```

A self-signed certificate from Keychain Access works too and costs nothing.

When the MCP server is launched by Claude Desktop, the responsible process is Claude Desktop — so **Claude Desktop** is what needs the grant, not this binary. The same is true of every other client: VS Code, Cursor and each terminal are separate subjects, so configuring the server in three clients means granting Accessibility three times, usually with no prompt to guide you because macOS treats the request as already answered by the parent.

`computer-control-mcp setup` offers a way out of that. Its **shared service** installs a launchd job, which is its own responsible process: it appears in System Settings under its own name, one grant covers every client, and clients reach it over loopback HTTP instead of each spawning a copy.

```bash
computer-control-mcp setup --shared     # install and point clients at it
computer-control-mcp setup --status     # is it running?
computer-control-mcp setup --restart    # after granting a permission
computer-control-mcp setup --stop       # remove it
```

Every client reaches it. Those that speak HTTP — Claude Code, VS Code, Cursor, Codex — connect directly. Those that can only launch a command, such as Claude Desktop, launch `computer-control-mcp bridge <url>`, which forwards JSON-RPC to the service and touches no OS API itself. **The bridge holds no permissions**, so the grant stays with the service and still covers those clients. There is no per-client mode on macOS any more, because there is no longer a case it serves better.

The trade-off is real and worth stating: a background process that can drive your desktop is running whether or not a client is attached. It listens on 127.0.0.1 only, requires a bearer token kept at `~/.config/computer-control/token` (mode 0600) and passed to the job through its environment rather than its argument list, and refuses cross-origin requests.

Codex is the one client that reads its token from the environment rather than its config file, which keeps the secret out of the config but means you have to export it:

```bash
export CC_AUTH_TOKEN=$(cat ~/.config/computer-control/token)
```

### How the walkthrough works

`setup` takes one permission at a time. For each it prints what to do, opens
the right pane, and waits for you to press enter — it does not poll, watch or
time out. When you have done both it reloads the service and reports what is
actually granted.

Pressing enter without granting is fine; it just moves on, and the report at
the end says what is missing. Nothing is destructive: run `setup` again, or
grant it later and `setup --restart`.

**Remove obsolete rows.** Older installations may have left raw
`computer-control-mcp` entries behind, all with the same name and no way to
tell them apart in the UI. Clear only the entries whose paths point into an
old Cellar version, then add the stable app bundle that `setup` prints.

Tools that need a permission you declined will fail with a message naming it,
rather than appearing to work.

### Two things that look like bugs and are not

**A grant is read when the process starts.** Enabling the checkbox while the
service is already running changes nothing until it restarts, and until then
`--doctor` reports `denied` with the checkbox visibly on. That is the likelier
explanation once you have already been to System Settings, so `--doctor` offers
to restart and re-check before sending you back there.

```bash
computer-control-mcp setup --restart
```

Homebrew installs each version to its own `Cellar/computer-control/<version>/`
path. The release therefore includes the signed `computer-control.app` bundle
with the stable identifier `dev.computercontrol.mcp`; `computer-control-mcp setup`
resolves it through Homebrew's stable `opt/computer-control` path and launchd
supervises the executable inside that bundle directly. The raw CLI remains
available for clients and diagnostics, but the long-lived service is no longer
attributed to a versioned Cellar executable or an orphaned `open` wrapper. Its
Accessibility grant survives `brew upgrade` when the release was signed with
the same Developer ID identity.

If an older installation left duplicate raw-binary entries behind, remove only
the entries whose paths point into an old `Cellar/computer-control/<version>/`
directory. Keep the `computer-control` bundle entry, then run:

```bash
computer-control-mcp setup --restart
```

**On Windows and Linux none of this applies.** There is no launchd, and a stdio child needs no grant in the first place, so clients launch the server directly.

`CGDisplayCreateImage` and `CGWindowListCreateImage` are **removed**, not merely deprecated, in the macOS 15 SDK. Capture uses ScreenCaptureKit, which needs macOS 12.3+.

## Windows

No permission prompts, but two rules:

- **UIPI**: synthetic input to a window running at a higher integrity level is silently discarded. To drive an elevated app, run elevated.
- The process sets per-monitor DPI awareness (V2) at startup. Without it Windows reports a virtualised 96-DPI desktop and every click on a scaled monitor lands wrong.

## Linux

X11 works out of the box. Two things to know:

**Wayland**: XTest reaches XWayland clients only — native Wayland apps will not see synthetic input. The `uinput` path works everywhere because it is kernel-level. This library detects a Wayland session and tells you rather than silently doing nothing.

**Multi-touch** needs write access to `/dev/uinput`:

```bash
sudo modprobe uinput
sudo groupadd -f uinput
sudo usermod -aG uinput "$USER"
echo 'KERNEL=="uinput", GROUP="uinput", MODE="0660", OPTIONS+="static_node=uinput"' \
  | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger

---

[← README](../README.md)
