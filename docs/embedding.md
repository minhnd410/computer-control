<!-- Split out of the README; see the table of contents there. -->
# Embedding the library

The MCP server is a thin wrapper over a C++20 library, and a source build
produces `libcomputer_control.a` plus the public headers in `include/cc/`. If
you are writing C++ and want the automation without a protocol in the middle,
link against it.

```cpp
#include <cc/session.hpp>

auto session = cc::Session::create().value();
auto input = session->input().value();

input->click(cc::Point{640, 480}, cc::ClickOptions{.count = 2});

cc::GestureRequest pinch;
pinch.kind = cc::GestureKind::Pinch;
pinch.center = cc::Point{700, 400};
pinch.scale = 2.0;
input->gesture(pinch);
```

`Session` owns the backends and creates them lazily, so a program that only
takes screenshots never triggers an accessibility prompt. Its destructor calls
`release_all()`, which guarantees that every held mouse button, modifier key
and touch contact is released — including on an exception path. An interrupted
drag cannot leave a desktop stuck.

Nothing in the core throws. Anything that can fail returns `Result<T>` or
`Status`, and every error carries a remedy naming the next step.

## Linking with CMake

```cmake
add_subdirectory(computer-control)
target_link_libraries(your_target PRIVATE cc::static)
```

Or against an installed copy:

```bash
cmake --install build --prefix /usr/local
```

```cmake
find_library(CC_LIB computer_control REQUIRED)
target_link_libraries(your_target PRIVATE ${CC_LIB})
```

You are responsible for the platform frameworks the library needs —
`CoreGraphics`, `ScreenCaptureKit` and friends on macOS, `user32`/`gdi32` and
friends on Windows, X11 and XTest on Linux. Using `add_subdirectory` is easier
because the `cc_config` interface target carries all of them.

## Other languages

Use MCP from another language. The stdio server exchanges line-delimited
JSON-RPC on stdin and stdout:

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{
  "name":"windows","arguments":{},
  "_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",
           "io.modelcontextprotocol/clientCapabilities":{}}}}' \
  | computer-control-mcp | jq '.result.structuredContent.windows[] | select(.focused)'
```

[JSON payloads](json-schema.md) documents what comes back.

---

[← README](../README.md)
