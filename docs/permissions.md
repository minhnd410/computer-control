# Permissions and platform access

Run the diagnostic command before troubleshooting a tool:

```bash
computer-control-mcp --doctor
```

The report lists available backends, displays, permission state, and gesture
fidelity. Tool failures include a specific remedy when the operating system
requires a setting or package.

## macOS

Grant access in **System Settings -> Privacy & Security**:

- **Accessibility**: input, window queries, menus, and the accessibility tree.
- **Screen & System Audio Recording**: screenshots and screen capture.

Request missing permissions with:

```bash
computer-control-mcp --request-permissions
```

Restart the server after changing a permission. When the server is launched by
an MCP client, macOS may attribute the grant to that client. For a stable
single grant shared by several clients, use the managed service:

```bash
computer-control-mcp setup --shared
computer-control-mcp setup --status
computer-control-mcp setup --restart
computer-control-mcp setup --stop
```

The service listens on loopback HTTP and stores its bearer token at
`~/.config/computer-control/token`. Keep the service bound to `127.0.0.1`.

If the binary does not appear in Accessibility, run `--doctor` and add the
exact path it reports. A signed app bundle can also be used for a persistent
macOS identity:

```bash
cmake --build build --target macos_bundle
open -n build/computer-control.app --args --request-permissions
```

## Windows

Windows does not show a permission prompt. Input sent to a higher-integrity
window can be discarded by UIPI. Run the server with the same or higher
privilege level as the target application.

The server enables per-monitor DPI awareness at startup. Restart it after
changing display scaling or monitor configuration.

## Linux

The X11 backend needs the runtime libraries listed in
[installation](install.md#runtime-requirements). Under Wayland, XTest reaches
XWayland applications; native Wayland input uses `/dev/uinput`.

Native multitouch requires access to `/dev/uinput`:

```bash
sudo modprobe uinput
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo usermod -aG input "$USER"
```

Log out and back in after changing group membership. Without uinput access,
capabilities reports the available emulation or unsupported gestures.

---

[<- README](../README.md)
