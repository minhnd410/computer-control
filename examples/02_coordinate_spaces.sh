#!/usr/bin/env sh
# The coordinate-space trap, demonstrated.
#
# On a Retina or scaled display, a pixel read off a screenshot is not a point
# you can click. Read-only: captures and converts, nothing else.
set -eu
CC=${CC_BIN:-./build/cc}

"$CC" displays
echo
"$CC" screenshot --out /tmp/cc_example.png --max_dimension 1000
echo
sh tools/demos/coordinates.sh
echo
echo "The conversion is done for you when the space is tagged:"
echo "    cc click --at 500,300@image"
echo "Untagged, that same pair is treated as logical points and lands elsewhere."
