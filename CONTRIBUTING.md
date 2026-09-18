# Contributing

Thanks for taking the time. This project is a single C++ core with several
front-ends, so the most useful thing you can do before changing anything is
read `CLAUDE.md` — it lists the invariants that break subtly.

## Getting set up

```bash
git clone https://github.com/minhnd410/computer-control.git
cd computer-control
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

System packages are listed in the README's build-from-source section.

## Adding a capability

Add an `ActionSpec` to the registry in `src/capi/actions.cpp` and implement the
handler. The MCP tool list and CLI help are generated from that registry, so
one edit exposes it everywhere. Only add a C ABI entry point if a binding needs
typed access rather than the JSON path.

## What a good change looks like

- **Comments say why.** The code is full of places that look wrong until you
  know what an OS API does. Explain that; don't narrate the next line.
- **Errors carry a remedy.** `Error{code, message, remedy}`. Name the exact
  settings pane, package or udev rule.
- **Emulation is declared.** If a platform can only approximate something,
  `gesture_support()` must say `Emulated` and explain the substitution.
- **Nothing stays held.** Use a scope guard for buttons, keys and contacts so
  the early-return path cannot leave a user's desktop stuck.
- **Tests for the platform-independent parts.** Coordinate math, gesture
  geometry, codecs and parsing are all testable without a desktop; that is why
  they live in `src/core/`.

## Platform work

Nobody has run the Windows backend yet — it compiles and passes the hermetic
tests on CI, and that is all. The Linux `uinput` native-gesture path is in the
same position. If you are the first to actually exercise either, that is a
genuinely useful contribution even if you change no code; please say so in an
issue.

You can only properly test the backend for the OS you are on. That is fine —
say in the PR which platforms you exercised. CI builds all three and runs the
hermetic tests everywhere.

## Style

`clang-format` with the in-tree `.clang-format`. Run it before pushing:

```bash
find include src tests -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.mm' \
  | xargs clang-format -i
```

## Dependencies

Please do not add one without discussing it first. JSON, PNG, JPEG and deflate
are all in-tree so the library has no mandatory third-party dependencies; that
is a deliberate property, not an oversight.

## Security

Never commit credentials or screenshots — captures routinely contain password
managers and private messages. For a suspected vulnerability, follow
[SECURITY.md](SECURITY.md) and open a private advisory rather than a public
issue.
