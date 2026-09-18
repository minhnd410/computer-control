"""Console entry points.

These let `uvx computer-control-mcp` and `pipx run computer-control` start the
native binaries without the caller knowing where they live. The Python package
is a thin wrapper: the MCP server is the C++ binary, not a reimplementation, so
there is one behaviour to maintain rather than two that drift.
"""

from __future__ import annotations

import os
import platform
import shutil
import sys


def _binary_name(stem: str) -> str:
    return f"{stem}.exe" if platform.system() == "Windows" else stem


def _find(stem: str) -> str | None:
    name = _binary_name(stem)
    here = os.path.dirname(os.path.abspath(__file__))

    candidates = [
        # Shipped inside the wheel, next to the shared library.
        os.path.join(here, "bin", name),
        os.path.join(here, name),
    ]
    # An explicit override wins, and accepts either the binary or its directory.
    if env := os.environ.get("COMPUTER_CONTROL_BIN"):
        candidates.insert(0, env if os.path.isfile(env) else os.path.join(env, name))
    # A local CMake build tree, which is how contributors run this.
    root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    candidates += [
        os.path.join(root, "build", name),
        os.path.join(root, "build", "Release", name),
    ]

    for path in candidates:
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return shutil.which(stem)


def _run(stem: str) -> int:
    path = _find(stem)
    if not path:
        sys.stderr.write(
            f"computer-control: cannot find the {stem} binary.\n\n"
            "The Python package wraps a native binary rather than reimplementing it,\n"
            "and no wheel with the binary bundled has been published yet. Build it:\n\n"
            "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\n\n"
            "then either put build/ on your PATH or set COMPUTER_CONTROL_BIN to it.\n"
        )
        return 1

    # exec rather than subprocess: an MCP server owns stdin and stdout, and an
    # intermediate Python process would add a layer that has to forward signals
    # and stream framing correctly for no benefit.
    args = [path, *sys.argv[1:]]
    try:
        if platform.system() == "Windows":
            import subprocess

            return subprocess.call(args)
        os.execv(path, args)
    except OSError as exc:
        sys.stderr.write(f"computer-control: cannot start {path}: {exc}\n")
        return 1
    return 0


def mcp() -> int:
    """Entry point for `computer-control-mcp`."""
    return _run("computer-control-mcp")


def cli() -> int:
    """Entry point for `computer-control`."""
    return _run("cc")


if __name__ == "__main__":
    raise SystemExit(mcp())
