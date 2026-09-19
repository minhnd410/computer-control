#!/usr/bin/env python3
"""Render the Homebrew formula for a released tag.

Reads the .sha256 files the release workflow attaches, so the checksums in the
formula are the ones CI computed rather than anything typed by hand.

    render_formula.py 0.2.0 <dir-with-archives-and-sha256s> > computer-control.rb
"""
import pathlib
import sys

TARGETS = {
    "SHA_MACOS_ARM64": "computer-control-macos-arm64.tar.gz",
    "SHA_MACOS_X86_64": "computer-control-macos-x86_64.tar.gz",
    "SHA_LINUX_X86_64": "computer-control-linux-x86_64.tar.gz",
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    version, dist = sys.argv[1].lstrip("v"), pathlib.Path(sys.argv[2])

    here = pathlib.Path(__file__).parent
    out = (here / "formula_template.rb").read_text()
    out = out.replace("@@VERSION@@", version)

    for key, archive in TARGETS.items():
        f = dist / (archive + ".sha256")
        if not f.exists():
            print(f"missing {f}", file=sys.stderr)
            return 1
        digest = f.read_text().split()[0]
        if len(digest) != 64:
            print(f"{f}: not a sha256", file=sys.stderr)
            return 1
        out = out.replace(f"@@{key}@@", digest)

    if "@@" in out:
        print("template still has unfilled placeholders", file=sys.stderr)
        return 1
    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
