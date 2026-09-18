#!/usr/bin/env sh
# computer-control installer.
#
#   curl -fsSL https://raw.githubusercontent.com/minhnd410/computer-control/main/packaging/scripts/install.sh | sh
#
# POSIX sh on purpose: this has to run under dash, busybox ash and macOS's
# ancient bash without anyone thinking about it.
set -eu

REPO="minhnd410/computer-control"
PREFIX="${CC_PREFIX:-/usr/local}"
VERSION="${CC_VERSION:-latest}"

say()  { printf '%s\n' "$*"; }
warn() { printf '%s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required but not installed"
}

detect_target() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$os" in
        Darwin) os=macos ;;
        Linux)  os=linux ;;
        *) die "unsupported OS '$os'. Windows users: use winget, or see the README." ;;
    esac
    case "$arch" in
        arm64|aarch64) arch=arm64 ;;
        x86_64|amd64)  arch=x86_64 ;;
        *) die "unsupported architecture '$arch'" ;;
    esac
    printf '%s-%s' "$os" "$arch"
}

# Building is the fallback when there is no release asset, which is the normal
# case today. It is slower but always correct, and the user is told which path
# was taken rather than left guessing.
build_from_source() {
    say "No prebuilt archive for $1; building from source."
    need git
    need cmake
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || \
        command -v clang >/dev/null 2>&1 || die "no C++ compiler found"

    if [ "$(uname -s)" = "Linux" ]; then
        # These are the four that actually break the build when missing; the
        # error from CMake otherwise names a header, not a package.
        missing=""
        for pkg in X11 Xtst Xrandr Xfixes; do
            if ! ls /usr/include/X11 >/dev/null 2>&1; then missing="libx11-dev"; break; fi
        done
        [ -n "$missing" ] && warn "X11 headers appear to be missing. On Debian/Ubuntu:
    sudo apt install build-essential cmake libx11-dev libxtst-dev libxrandr-dev libxfixes-dev zlib1g-dev"
    fi

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    say "Cloning into $tmp ..."
    git clone --depth 1 "https://github.com/$REPO.git" "$tmp/src" >/dev/null 2>&1 \
        || die "clone failed"
    cmake -S "$tmp/src" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release >/dev/null \
        || die "cmake configure failed"
    cmake --build "$tmp/build" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null \
        || die "build failed"
    install_files "$tmp/build"
}

install_files() {
    src="$1"
    bindir="$PREFIX/bin"
    # Only escalate if we have to, and say so before doing it.
    if [ -w "$bindir" ] 2>/dev/null || mkdir -p "$bindir" 2>/dev/null; then
        sudo_cmd=""
    else
        warn "$bindir is not writable; using sudo."
        need sudo
        sudo_cmd="sudo"
        $sudo_cmd mkdir -p "$bindir"
    fi

    binary=computer-control-mcp
    [ -f "$src/$binary" ] || die "expected $src/$binary after build"
    $sudo_cmd install -m 0755 "$src/$binary" "$bindir/$binary"
    say "Installed $binary to $bindir"
}

fetch_release() {
    target="$1"
    need curl
    need tar
    if [ "$VERSION" = "latest" ]; then
        url="https://github.com/$REPO/releases/latest/download/computer-control-$target.tar.gz"
    else
        url="https://github.com/$REPO/releases/download/$VERSION/computer-control-$target.tar.gz"
    fi

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    # A missing release must not look like a network failure.
    if ! curl -fsSL "$url" -o "$tmp/archive.tar.gz" 2>/dev/null; then
        return 1
    fi
    tar -xzf "$tmp/archive.tar.gz" -C "$tmp" || die "archive is corrupt"
    install_files "$tmp"
    return 0
}

main() {
    target=$(detect_target)
    say "computer-control installer - target $target, prefix $PREFIX"

    if ! fetch_release "$target"; then
        build_from_source "$target"
    fi

    case ":$PATH:" in
        *":$PREFIX/bin:"*) ;;
        *) warn "
$PREFIX/bin is not on your PATH. Add it:
    echo 'export PATH=\"$PREFIX/bin:\$PATH\"' >> ~/.profile" ;;
    esac

    say ""
    say "Next:"
    say "    computer-control-mcp --request-permissions   # grant what it needs"
    say "    computer-control-mcp --doctor                # full capability report"
    if [ "$(uname -s)" = "Darwin" ]; then
        say ""
        say "On macOS the Accessibility grant follows the *responsible process*, so a"
        say "binary run from a terminal is attributed to the terminal and never appears"
        say "in System Settings. \`--doctor\` explains what to do about it."
    fi
}

main "$@"
