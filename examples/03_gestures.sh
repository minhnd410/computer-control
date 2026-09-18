#!/usr/bin/env sh
# Fidelity-aware gestures.
#
# Multi-touch differs sharply by platform: Windows and Linux can inject real
# contacts, macOS cannot. Rather than substituting something that looks similar
# and hoping, ask first.
#
# Read-only unless --act.
set -eu
CC=${CC_BIN:-./build/cc}

"$CC" capabilities | awk '/^gestures$/,/^$/'

echo "Asking for a gesture this platform cannot do honestly:"
"$CC" gesture --kind rotate --degrees 90 --require_native 2>&1 | sed 's/^/    /' || true

if [ "${1:-}" != "--act" ]; then
    echo
    echo "(pass --act to perform a pinch at the pointer)"
    exit 0
fi

echo
echo "Pinching to zoom in at the pointer..."
"$CC" gesture --kind pinch --scale 2.0 --duration_ms 400
echo "done; cc gesture --kind pinch --scale 0.5 reverses it"
