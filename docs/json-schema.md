# JSON payloads

The server uses MCP JSON-RPC. Tool calls return a short text summary for the
model and structured data for programs. Fields are additive: clients should
ignore fields they do not know and treat optional fields as absent.

## Tool result

A successful `tools/call` result has this shape:

```json
{
  "content": [{"type": "text", "text": "Processes: 2 returned."}],
  "structuredContent": {
    "ok": true,
    "action": "process",
    "processes": [
      {"pid": 1234, "name": "example", "memory_mb": 12.5}
    ]
  }
}
```

The text is a compact status summary. `structuredContent` contains the exact
bounded payload. Screenshots add an image content item:

```json
{"type": "image", "data": "<base64>", "mimeType": "image/png"}
```

A failed tool call has `isError: true`, one actionable text item, and a stable
structured error:

```json
{
  "isError": true,
  "content": [{"type": "text", "text": "No window matches 'Editor'."}],
  "structuredContent": {
    "ok": false,
    "action": "windows",
    "error": {
      "code": "not_found",
      "message": "no window matches 'Editor'",
      "remedy": "Check the title and call windows(mode=\"list\") first."
    }
  }
}
```

When an operation has partial data, it is retained under
`structuredContent.result`. Error codes are `invalid_argument`,
`permission_denied`, `unsupported`, `not_found`, `timeout`, `busy`,
`backend_failure`, `device_error`, `io_error`, or `internal`.

## Coordinates

### Point

```json
{"x": 640.0, "y": 480.0, "space": "logical"}
```

`space` is `logical`, `physical`, or `image`. Input accepts `[x, y]`, the
object form, or `"640,480"` / `"640,480@image"`.

### Rectangle

```json
{"x": 0.0, "y": 0.0, "w": 1440.0, "h": 900.0, "space": "logical"}
```

## Bounded collections

List and tree responses report their bounds when applicable:

```json
{
  "windows": [],
  "returned": 2,
  "total": 9,
  "truncated": true
}
```

Accessibility responses also report `nodes_walked`, `element_count`, and, when
a walk stops at a budget, `truncation_reason`. Device tree responses use the
same fields.

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
    "bounds_logical": {"x": 0, "y": 0, "w": 1440, "h": 900, "space": "logical"},
    "bounds_physical": {"x": 0, "y": 0, "w": 2880, "h": 1800, "space": "physical"},
    "work_area": {"x": 0, "y": 25, "w": 1440, "h": 875, "space": "logical"}
  }],
  "virtual_bounds": {"x": 0, "y": 0, "w": 1440, "h": 900, "space": "logical"}
}
```

## Windows

```json
{
  "windows": [{
    "id": 9694,
    "title": "Example",
    "app": "Editor",
    "pid": 86727,
    "bounds": {"x": 510, "y": 30, "w": 900, "h": 850, "space": "logical"},
    "focused": false,
    "display": 0
  }]
}
```

Window identifiers are valid only while the window exists. Re-list before using
an identifier after the desktop changes.

## Accessibility elements

```json
{
  "label": 7,
  "role": "button",
  "name": "Save",
  "bounds": {"x": 100, "y": 200, "w": 80, "h": 24, "space": "logical"},
  "center": {"x": 140, "y": 212, "space": "logical"},
  "enabled": true,
  "actions": ["press"]
}
```

Labels are assigned in snapshot order and can be passed to `click`, `type`, and
`scroll`. They expire when the snapshot is older than 60 seconds or a new
snapshot replaces it.

## Shell and clipboard output

Shell stdout and stderr are capped at 12,000 bytes per field by default. Pass
`max_output_bytes` to request a different bound, up to 262,144 bytes. The
response includes the original byte count and a `*_truncated` flag when needed.
Clipboard reads use the same policy with `text_bytes` and `text_truncated`.
Truncation never splits a UTF-8 sequence.

```json
{
  "exit_code": 0,
  "stdout": "...",
  "stderr": "",
  "stdout_bytes": 24000,
  "stderr_bytes": 0,
  "stdout_truncated": true,
  "timed_out": false,
  "elapsed_ms": 42
}
```

A non-zero `exit_code` means the command ran and returned that code. It is not
a tool failure; failures to start the command use the structured error shape.

---

[<- README](../README.md)
