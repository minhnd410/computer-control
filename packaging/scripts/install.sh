#!/usr/bin/env sh
# computer-control installer for macOS and Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
#
# POSIX sh on purpose: this has to run under dash, busybox ash and macOS's
# ancient bash without anyone thinking about it.
#
# Environment:
#   CC_PREFIX    install root, default /usr/local (or ~/.local when that is
#                not writable and sudo is unavailable)
#   CC_VERSION   a tag such as v0.8.1, default the latest release
set -eu

REPO="minhnd410/computer-control"
VERSION="${CC_VERSION:-latest}"

say()  { printf '%s\n' "$*"; }
warn() { printf '%s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required but not installed"
}

target() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$os" in
        Darwin) os=macos ;;
        Linux)  os=linux ;;
        *) die "unsupported OS '$os'. Windows: use install.ps1 or winget." ;;
    esac
    case "$arch" in
        arm64|aarch64) arch=arm64 ;;
        x86_64|amd64)  arch=x86_64 ;;
        *) die "unsupported architecture '$arch'" ;;
    esac
    if [ "$os" = "linux" ] && [ "$arch" = "arm64" ]; then
        die "no prebuilt archive for Linux on arm64 yet.
       Build from source: https://github.com/$REPO/blob/main/docs/install.md"
    fi
    printf '%s-%s' "$os" "$arch"
}

# Where to put it, and whether that needs sudo. Choosing ~/.local over asking
# for a password is the friendlier default for a one-line installer.
choose_prefix() {
    if [ -n "${CC_PREFIX:-}" ]; then
        printf '%s' "$CC_PREFIX"
        return
    fi
    if [ -w /usr/local/bin ] 2>/dev/null; then
        printf '/usr/local'
    elif command -v sudo >/dev/null 2>&1 && [ -d /usr/local/bin ]; then
        printf '/usr/local'
    else
        printf '%s/.local' "$HOME"
    fi
}

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        die "need shasum or sha256sum to verify the download"
    fi
}

main() {
    need curl
    need tar

    TARGET=$(target)
    PREFIX=$(choose_prefix)
    ARCHIVE="computer-control-$TARGET.tar.gz"

    if [ "$VERSION" = "latest" ]; then
        BASE="https://github.com/$REPO/releases/latest/download"
    else
        BASE="https://github.com/$REPO/releases/download/$VERSION"
    fi

    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT

    say "computer-control: $TARGET, installing to $PREFIX/bin"

    curl -fsSL "$BASE/$ARCHIVE" -o "$TMP/$ARCHIVE" \
        || die "could not download $BASE/$ARCHIVE
       Check https://github.com/$REPO/releases for what is published."

    # Every release publishes a .sha256 beside the archive. Skipping this on a
    # binary that can drive the desktop would be indefensible, so a missing or
    # mismatched checksum aborts rather than warns.
    curl -fsSL "$BASE/$ARCHIVE.sha256" -o "$TMP/$ARCHIVE.sha256" \
        || die "no checksum published for $ARCHIVE; refusing to install unverified"

    EXPECTED=$(cut -d' ' -f1 < "$TMP/$ARCHIVE.sha256")
    ACTUAL=$(sha256_of "$TMP/$ARCHIVE")
    if [ "$EXPECTED" != "$ACTUAL" ]; then
        die "checksum mismatch for $ARCHIVE
       expected $EXPECTED
       got      $ACTUAL
       Do not use this download."
    fi
    say "  checksum ok"

    tar -xzf "$TMP/$ARCHIVE" -C "$TMP" || die "archive is corrupt"
    BIN="$TMP/computer-control/computer-control-mcp"
    [ -f "$BIN" ] || die "unexpected archive layout: no computer-control/computer-control-mcp"

    SUDO=""
    if [ ! -w "$PREFIX/bin" ] 2>/dev/null; then
        if [ -d "$PREFIX/bin" ] || ! mkdir -p "$PREFIX/bin" 2>/dev/null; then
            command -v sudo >/dev/null 2>&1 || die "$PREFIX/bin is not writable and sudo is unavailable.
       Set CC_PREFIX to somewhere you own, e.g. CC_PREFIX=\$HOME/.local"
            say "  $PREFIX/bin needs root; using sudo for the copy only"
            SUDO="sudo"
            $SUDO mkdir -p "$PREFIX/bin"
        fi
    fi

    $SUDO install -m 0755 "$BIN" "$PREFIX/bin/computer-control-mcp"
    say "  installed $($PREFIX/bin/computer-control-mcp --version | head -1)"

    case ":$PATH:" in
        *":$PREFIX/bin:"*) ;;
        *) warn "
$PREFIX/bin is not on your PATH. Add it:
    echo 'export PATH=\"$PREFIX/bin:\$PATH\"' >> ~/.profile" ;;
    esac

    say "
Next:
    computer-control-mcp setup

That registers the server with the MCP clients on this machine and walks
through the permissions it needs."
}

main "$@"
