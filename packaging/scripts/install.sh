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

# The archive is a dynamically linked binary, not a self-contained one, and the
# two ways it fails to start are both silent until you run it. Check before
# copying anything: an install that reports success and then cannot launch is
# the worst of the outcomes available here.

# macOS: the binary is built against a 14.0 deployment target because the
# capture path needs SCScreenshotManager. Below that, dyld refuses to load it
# and prints nothing a person can act on.
check_macos_version() {
    have=$(sw_vers -productVersion 2>/dev/null) || return 0
    major=${have%%.*}
    case "$major" in
        ''|*[!0-9]*) return 0 ;;
    esac
    if [ "$major" -lt 14 ]; then
        die "this needs macOS 14 or newer; you are on $have.
       Screen capture uses SCScreenshotManager, which does not exist before 14.
       Nothing published here will run on this version."
    fi
}

# Linux: X11 is linked dynamically, and a server install or a slim container
# has none of it. ldd names exactly what is missing, so use it rather than
# guessing from the distribution.
check_linux_libraries() {
    command -v ldd >/dev/null 2>&1 || return 0
    missing=$(ldd "$1" 2>/dev/null | awk '/not found/ { print $1 }' | sort -u)
    [ -n "$missing" ] || return 0

    warn "error: this binary needs shared libraries this system does not have:"
    for lib in $missing; do warn "         $lib"; done
    warn "
       Install them, then re-run this script:
         Debian/Ubuntu  sudo apt install libx11-6 libxtst6 libxrandr2 libxfixes3 zlib1g
         Fedora/RHEL    sudo dnf install libX11 libXtst libXrandr libXfixes zlib
         Arch           sudo pacman -S libx11 libxtst libxrandr libxfixes zlib
       Nothing has been installed."
    exit 1
}

main() {
    need curl
    need tar

    TARGET=$(target)
    case "$TARGET" in macos-*) check_macos_version ;; esac

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

    case "$TARGET" in linux-*) check_linux_libraries "$BIN" ;; esac

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
