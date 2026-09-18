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

1. `cc` never appears in the Accessibility list, so there is nothing to enable.
2. The permission prompt never fires — `AXIsProcessTrusted()` already returns true because the terminal is granted, and macOS only prompts a process it considers untrusted.
3. That inherited grant covers the *trust check* but not always real inspection. You get `AXIsProcessTrusted() == true` while every window comes back as an empty placeholder.

`computer-control-mcp --doctor` detects all three and names the owner:

```
Permissions for this process are attributed to iTerm2, not to the binary
itself, which is why
  /usr/local/bin/cc
does not appear in System Settings.
```

Two ways to fix it:

- **Add the binary by hand.** In the Accessibility list, click **+** and select the exact path `--doctor` printed. Restart the process.
- **Use the app bundle** (better, because the grant survives rebuilds):

  ```bash
  cmake --build build --target macos_bundle
  open -n build/computer-control.app --args permissions --request
  ```

  A bundle launched through LaunchServices is its own responsible process, so it prompts properly and appears in the list as **computer-control**, where you can enable it. Running the binary inside the bundle directly from a shell does *not* do this — it is a child of the shell again, and `--doctor` will say so.

**Signing matters for persistence.** An ad-hoc signature is keyed to the code hash, so every rebuild is a new identity and the grant is lost. Pass a real certificate to keep it:

```bash
cmake -S . -B build -DCC_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
```

A self-signed certificate from Keychain Access works too and costs nothing.

When the MCP server is launched by Claude Desktop, the responsible process is Claude Desktop — so its grants apply and there is usually nothing to do.

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
