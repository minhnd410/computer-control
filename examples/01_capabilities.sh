#!/usr/bin/env sh
# What can this machine actually do?
#
# The first thing to run on an unfamiliar host. Backends start lazily, so a
# missing accessibility grant does not stop screenshots working - this shows
# exactly which pieces are available and what to do about the rest.
set -eu
CC=${CC_BIN:-./build/cc}

"$CC" capabilities
echo
echo "--- permissions ---"
"$CC" permissions
echo
echo "--- shell-level actions this desktop supports ---"
"$CC" system
