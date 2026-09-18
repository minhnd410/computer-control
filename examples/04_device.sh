#!/usr/bin/env sh
# Drive a phone: an iOS simulator, an Android emulator, or a mirrored handset.
#
# Coordinates are in the device's own points, so the same script works whether
# the simulator window is full size or scaled into a corner.
#
# Read-only unless --act.
set -eu
CC=${CC_BIN:-./build/cc}

"$CC" device --mode list

DEVICE=$("$CC" device --mode list --raw 2>/dev/null | python3 -c '
import sys, json
d = json.load(sys.stdin)["result"].get("devices") or []
print(d[0]["id"] if d else "")
')

if [ -z "$DEVICE" ]; then
    echo
    echo "No devices found."
    echo "  iOS:     install Xcode, then: xcrun simctl boot <device> && open -a Simulator"
    echo "           (sudo xcode-select -s /Applications/Xcode.app if simctl is missing)"
    echo "  Android: install platform-tools, then start an emulator"
    echo
    echo "A simulator visible on screen also works with no CLI tooling at all."
    exit 1
fi

echo
echo "--- $DEVICE ---"
"$CC" device --mode info --device "$DEVICE"

if [ "${1:-}" != "--act" ]; then
    echo
    echo "(pass --act to screenshot the device and tap its centre)"
    exit 0
fi

"$CC" device --mode screenshot --device "$DEVICE" --out device.png
echo "wrote device.png"
