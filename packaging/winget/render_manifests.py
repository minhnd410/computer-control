#!/usr/bin/env python3
"""Render the Winget manifests for one published release."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

VERSION_PATTERN = re.compile(r"^\d+\.\d+\.\d+$")
SHA256_PATTERN = re.compile(r"^[0-9a-fA-F]{64}$")


def replace_line(text: str, key: str, value: str) -> str:
    pattern = re.compile(rf"(?m)^([ \t]*){re.escape(key)}:.*$")
    updated, count = pattern.subn(lambda match: f"{match.group(1)}{key}: {value}", text, count=1)
    if count != 1:
        raise ValueError(f"template is missing {key}")
    return updated


def render(template_dir: Path, output_dir: Path, version: str, installer_url: str, sha256: str) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    version_manifest = (template_dir / "minhnd410.computer-control.yaml").read_text()
    version_manifest = replace_line(version_manifest, "PackageVersion", version)

    locale_manifest = (template_dir / "minhnd410.computer-control.locale.en-US.yaml").read_text()
    locale_manifest = replace_line(locale_manifest, "PackageVersion", version)
    locale_manifest = replace_line(
        locale_manifest,
        "ReleaseNotesUrl",
        f"https://github.com/minhnd410/computer-control/releases/tag/v{version}",
    )

    installer_manifest = (template_dir / "minhnd410.computer-control.installer.yaml").read_text()
    installer_manifest = replace_line(installer_manifest, "PackageVersion", version)
    installer_manifest = replace_line(installer_manifest, "InstallerUrl", installer_url)
    installer_manifest = replace_line(installer_manifest, "InstallerSha256", sha256.upper())

    files = {
        "minhnd410.computer-control.yaml": version_manifest,
        "minhnd410.computer-control.locale.en-US.yaml": locale_manifest,
        "minhnd410.computer-control.installer.yaml": installer_manifest,
    }
    for name, content in files.items():
        (output_dir / name).write_text(content, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--installer-url", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--template-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not VERSION_PATTERN.fullmatch(args.version):
        parser.error("--version must be a three-part numeric version")
    if not SHA256_PATTERN.fullmatch(args.sha256):
        parser.error("--sha256 must be a 64-character hexadecimal digest")
    if not args.installer_url.startswith("https://"):
        parser.error("--installer-url must use HTTPS")

    render(args.template_dir, args.output, args.version, args.installer_url, args.sha256)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
