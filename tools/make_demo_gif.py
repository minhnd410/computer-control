#!/usr/bin/env python3
"""Render a terminal-style GIF from real command output.

Why render rather than screen-record: these GIFs go in a public README, and a
screen recording of a maintainer's desktop leaks whatever else was on it. This
runs the actual commands, captures their real output, and draws it on a
synthetic terminal - authentic content, nothing private.

    python3 tools/make_demo_gif.py capabilities

Needs ImageMagick and ffmpeg.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

FONT = "/System/Library/Fonts/Menlo.ttc" if sys.platform == "darwin" else "DejaVu-Sans-Mono"
BG = "#11131a"
FG = "#d5dae3"
PROMPT_FG = "#7fd1b9"
DIM = "#8b93a7"

CHAR_W = 8
LINE_H = 19
PAD = 18


def run(cmd: list[str]) -> str:
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return (out.stdout + out.stderr).rstrip("\n")


def wrap(lines: list[str], width: int) -> list[str]:
    """Wrap on word boundaries, preserving indentation.

    A hard character wrap splits words ("the bi / nary") which looks like a
    rendering bug in a GIF people cannot scroll or re-read.
    """
    wrapped: list[str] = []
    for line in lines:
        if len(line) <= width:
            wrapped.append(line)
            continue
        indent = line[: len(line) - len(line.lstrip())]
        words = line.split(" ")
        current = ""
        for word in words:
            candidate = word if not current else current + " " + word
            if len(candidate) <= width:
                current = candidate
                continue
            if current:
                wrapped.append(current)
            # A single word longer than the line still has to be cut.
            while len(word) > width:
                wrapped.append(word[:width])
                word = word[width:]
            current = indent + word if indent and not word.startswith(indent) else word
        if current:
            wrapped.append(current)
    return wrapped


def render_frame(path: str, lines: list[str], width: int, height: int, cursor_line: int) -> None:
    px_w = width * CHAR_W + PAD * 2
    px_h = height * LINE_H + PAD * 2

    args = [
        "magick", "-size", f"{px_w}x{px_h}", f"xc:{BG}",
        "-font", FONT, "-pointsize", "14",
    ]
    for i, line in enumerate(lines[:height]):
        y = PAD + (i + 1) * LINE_H - 5
        colour = FG
        text = line
        if line.startswith("$ "):
            colour = PROMPT_FG
        elif line.startswith("  ") and ("->" in line or line.strip().startswith("#")):
            colour = DIM
        # A caret on the line currently being typed.
        if i == cursor_line:
            text = line + "█"
        # -annotate needs literal text; escape the few characters it treats
        # specially rather than risk a mangled frame.
        safe = text.replace("\\", "\\\\").replace("%", "%%")
        args += ["-fill", colour, "-annotate", f"+{PAD}+{y}", safe]
    args.append(path)
    subprocess.run(args, check=True, capture_output=True)


def build(name: str, script: list[tuple[str, list[str] | None]], width: int = 96) -> str:
    """script is a list of (prompt_line, command_or_None). None means a literal
    line of narration."""
    frames_dir = tempfile.mkdtemp(prefix="ccgif_")
    shown: list[str] = []
    frame_paths: list[str] = []
    n = 0

    def emit(cursor_line: int, repeat: int = 1) -> None:
        nonlocal n
        for _ in range(repeat):
            p = os.path.join(frames_dir, f"f{n:04d}.png")
            render_frame(p, shown, width, HEIGHT, cursor_line)
            frame_paths.append(p)
            n += 1

    # Two passes: work out the height first so the canvas never resizes.
    preview: list[str] = []
    for prompt, cmd in script:
        preview.append("$ " + prompt if cmd is not None else prompt)
        if cmd is not None:
            preview += wrap(run(cmd).split("\n"), width)
        preview.append("")
    while preview and not preview[-1].strip():
        preview.pop()
    global HEIGHT
    HEIGHT = len(preview) + 1

    for prompt, cmd in script:
        if cmd is None:
            shown.append(prompt)
            emit(len(shown) - 1, 4)
            continue
        # Type the command out a character at a time.
        shown.append("$ ")
        for ch in prompt:
            shown[-1] += ch
            emit(len(shown) - 1, 1)
        emit(len(shown) - 1, 8)          # pause on the complete command
        for line in wrap(run(cmd).split("\n"), width):
            shown.append(line)
        emit(-1, 26)                      # hold on the output
        shown.append("")

    out = f"docs/media/{name}.gif"
    pattern = os.path.join(frames_dir, "f%04d.png")
    # One filtergraph rather than a separate palette file: generating the
    # palette inline avoids a second input stream whose dimensions ffmpeg then
    # has to reconcile with the frames.
    graph = ("split[a][b];[a]palettegen=stats_mode=full[p];"
             "[b][p]paletteuse=dither=bayer:bayer_scale=3")
    r = subprocess.run(
        ["ffmpeg", "-y", "-framerate", "24", "-i", pattern,
         "-filter_complex", graph, "-loop", "0", out],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("ffmpeg failed:\n" + r.stderr[-1500:])
    shutil.rmtree(frames_dir, ignore_errors=True)
    return out


HEIGHT = 20

# Each demo is a list of (typed command, argv to actually run). Narration
# lines pass None as the command.
# Each demo is (canvas width in characters, [(typed line, argv or None)]).
# A None command means the line is narration, not something that runs.
#
# The typed text must be a command that genuinely produces the output shown -
# a prettier but fictional pipeline would make the GIF a lie.
DEMOS = {
    "gestures": (
        74,
        [
            ("# which gestures are real here, and what the rest fall back to", None),
            ("computer-control-mcp --doctor", ["sh", "tools/demos/gestures.sh"]),
        ],
    ),
    "coordinates": (
        70,
        [
            ("# a pixel read off a Retina screenshot is not a click", None),
            ("sh tools/demos/coordinates.sh", ["sh", "tools/demos/coordinates.sh"]),
        ],
    ),
    "permissions": (
        84,
        [
            ("# why the binary never appears in System Settings", None),
            ("computer-control-mcp --doctor", ["sh", "tools/demos/permissions.sh"]),
        ],
    ),
}


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: make_demo_gif.py <demo>")
        print("demos:", ", ".join(DEMOS))
        return 2
    name = sys.argv[1]
    if name not in DEMOS:
        print(f"unknown demo '{name}'")
        return 2
    width, script = DEMOS[name]
    out = build(name, script, width)
    size = os.path.getsize(out)
    print(f"wrote {out} ({size // 1024} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
