<!-- Split out of the README; see the table of contents there. -->
# Gesture fidelity

The rule this project follows: if a platform cannot do the real thing, say so.
`gesture_support()` returns `native`, `emulated` or `unsupported`, names the
substitute, and explains it. Nothing is quietly faked.

Ask the machine in front of you rather than trusting this table:

```bash
computer-control-mcp --doctor      # includes the gesture section
```

| Gesture | Windows | Linux | macOS |
|---|---|---|---|
| Tap, long press | native | native (uinput) / emulated | native |
| 2-finger swipe, pan | native | native (uinput) / emulated | **native** (phased scroll) |
| 3–5 finger swipe | shell shortcuts | native (uinput) | shell shortcuts |
| Pinch / zoom | native | native (uinput) / emulated | emulated (`cmd`+scroll) |
| Rotate | native | native (uinput) | **unsupported** |
| Force press | native | native (uinput) | emulated (long press) |

Pass `require_native: true` to any gesture to refuse an emulated substitute
rather than accept one silently.

## Windows

`InjectTouchInput` produces real touch contacts, up to ten.

Windows reports touchscreen contacts as native input. Trackpad-style
three- and four-finger swipes are exposed through the corresponding desktop
shortcuts, and `capabilities` reports that fidelity.

## Linux

A virtual multitouch touchscreen is created through `/dev/uinput`, which works
on Wayland as well as X11 because it enters the kernel below the display
server. This needs write access to `/dev/uinput`; without it, gestures fall
back to XTest emulation and report themselves as emulated.

Granting access, if you want the native path:

```bash
sudo modprobe uinput
echo 'KERNEL=="uinput", GROUP="input", MODE="0660"' | sudo tee /etc/udev/rules.d/99-uinput.rules
sudo usermod -aG input "$USER"   # log out and back in
```

## macOS

There is no public API for synthesizing multi-touch. What that leaves:

- **Two-finger swipe and pan are genuinely native.** `CGEvent` exposes trackpad
  scroll phases, so apps receive the same begin/change/end stream a real
  trackpad produces, momentum included.
- **Pinch and smart zoom map to `cmd`+scroll**, which most applications treat
  as zoom. It is a substitute and is reported as one.
- **Rotate has no honest equivalent and is refused.** Returning something that
  looks like a rotation but is not would be worse than an error.
- **Three- and four-finger swipes** map to the Mission Control shortcuts, the
  same approach Windows takes and for the same reason.

---

[← README](../README.md)
