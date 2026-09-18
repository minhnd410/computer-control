#!/usr/bin/env sh
# The gesture section of the `--doctor` capability report: which gestures are
# real here, which are emulated, and what each one falls back to.
./build/computer-control-mcp --doctor 2>/dev/null | awk '/^gestures$/,/^$/' | sed '$d'
