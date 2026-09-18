#!/usr/bin/env sh
# The gesture section of `cc capabilities`: which are real, which are
# emulated, and what each falls back to.
./build/cc capabilities 2>/dev/null | awk '/^gestures$/,/^$/' | sed '$d'
