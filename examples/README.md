# Examples

Shell scripts driving the `cc` CLI, and one C++ program using the library
directly. Build first:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

| | |
|---|---|
| `01_capabilities.sh` | What this machine can do, and what is missing. Start here. |
| `02_coordinate_spaces.sh` | The Retina trap, and how the space tag avoids it. |
| `03_gestures.sh` | Fidelity-aware gestures; refuses rather than faking. |
| `04_device.sh` | Drive an iOS simulator or Android emulator in device points. |
| `05_library.cpp` | The C++ API directly, for embedding. |

Every script is read-only unless you pass `--act`, so running one cannot
disturb a desktop you care about.

For scripting from another language, drive the CLI with `--raw` and parse the
JSON — every action returns the same structured payload the MCP tools do:

```bash
cc windows --raw | jq '.result.windows[] | select(.focused)'
```
