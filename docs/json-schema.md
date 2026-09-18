# JSON schema

The MCP tools return JSON for anything whose shape is open-ended: window lists,
accessibility trees, device inventories, shell results. This document is the
contract for those payloads.

**Compatibility:** additive only. Fields are added, never removed or retyped.
Treat an unknown field as ignorable, and never assume an optional field is
present.

Every action that produces JSON wraps the payload:

```json
{ "ok": true, "text": "human-readable summary", "result": { ... } }
```

`text` is what an agent should read; `result` is what a program should parse.

## Shared types

### Point

```json
{ "x": 640.0, "y": 480.0, "space": "logical" }
```

`space` is `logical`, `physical` or `image`. Accepted as input in three forms —
`[x, y]`, the object above, or `"640,480"` / `"640,480@image"`.

### Rect

```json
{ "x": 0.0, "y": 0.0, "w": 1440.0, "h": 900.0, "space": "logical" }
```

## Displays

```json
{
  "displays": [{
    "index": 0,
    "name": "Built-in Display",
    "primary": true,
    "scale": 2.0,
    "dpi": 144.0,
    "refresh_hz": 60.0,
    "bounds_logical":  { "x": 0, "y": 0, "w": 1440, "h": 900,  "space": "logical" },
    "bounds_physical": { "x": 0, "y": 0, "w": 2880, "h": 1800, "space": "physical" },
    "work_area":       { "x": 0, "y": 25, "w": 1440, "h": 875, "space": "logical" }
  }],
  "virtual_bounds": { "x": 0, "y": 0, "w": 1440, "h": 900, "space": "logical" }
}
```

`scale` is `bounds_physical.w / bounds_logical.w`. On a mixed-DPI setup each
display has its own, which is why conversion is per display rather than global.

## Windows

```json
{
  "windows": [{
    "id": 9694,
    "title": "iPhone 17 Pro Max",
    "app": "Simulator",
    "pid": 86727,
    "bounds": { "x": 510, "y": 30, "w": 393, "h": 850, "space": "logical" },
    "focused": false,
    "display": 0
  }]
}
```

`id` is a `CGWindowID` on macOS, an `HWND` on Windows and an X11 `Window` on
Linux. It is only valid while the window exists; re-list rather than caching.

## Accessibility element

```json
{
  "label": 7,
  "role": "button",
  "raw_role": "AXButton",
  "name": "Save",
  "value": "",
  "id": "save-button",
  "bounds": { "x": 100, "y": 200, "w": 80, "h": 24, "space": "logical" },
  "center": { "x": 140, "y": 212, "space": "logical" },
  "enabled": true,
  "focused": false,
  "checked": null,
  "actions": ["press"],
  "app": "TextEdit"
}
```

`label` is assigned per snapshot in document order and is what `click`, `type`
and `scroll` accept. It is only meaningful until the next snapshot; the
dispatcher rejects a label from a snapshot older than 60 seconds.

`role` is the normalised role; `raw_role` is the platform's own name
(`AXButton`, a UIA class name, a Java/GTK class). Match on `role`.

A tree response adds:

```json
{
  "element_count": 42,
  "nodes_walked": 1831,
  "elapsed_ms": 287,
  "truncated": false,
  "truncation_reason": ""
}
```

`truncated` means a budget was hit, so absence of an element proves nothing.

## Device

```json
{
  "id": "onscreen:9694",
  "name": "iPhone 17 Pro Max",
  "platform": "ios",
  "kind": "simulator",
  "os_version": "26.0",
  "booted": true,
  "screen_points": { "width": 440, "height": 956, "scale": 3.0 },
  "transports": ["onscreen"],
  "active_transport": "onscreen",
  "viewport": {
    "host_rect": { "x": 510, "y": 58, "w": 393, "h": 850 },
    "host_px_per_device_pt": 0.893,
    "warning": "..."
  }
}
```

`id` is a simctl UDID, an adb serial, or `onscreen:<window id>` for a device
found only by its window. `screen_points` is `null` when the device model is
not recognised — coordinates then cannot be translated precisely, and the
response says so.

`host_px_per_device_pt` below about 0.75 means the window is drawn well under
the device's logical size and small tap targets become unreliable.

## Shell result

```json
{ "exit_code": 0, "stdout": "...", "stderr": "", "timed_out": false, "elapsed_ms": 42 }
```

A non-zero `exit_code` is **not** a tool failure: the command was asked for and
it ran. `ok` is false only when the command could not be started.

## Errors

```json
{ "ok": false, "error": "...", "code": "permission_denied", "remedy": "..." }
```

`code` is one of `invalid_argument`, `permission_denied`, `unsupported`,
`not_found`, `timeout`, `busy`, `backend_failure`, `device_error`, `io_error`,
`internal`. `remedy` is the actionable next step and is worth surfacing to a
user verbatim.

---

[← README](../README.md)
