#!/usr/bin/env sh
# Verbatim permission section of `computer-control-mcp --doctor`.
./build/computer-control-mcp --doctor 2>/dev/null | sed -n '1,/^computer-control /p' | sed '$d'
