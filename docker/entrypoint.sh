#!/usr/bin/env bash
# Starts a virtual display, a window manager, then computer-control.
set -euo pipefail

DISPLAY_NUM="${DISPLAY#:}"
DISPLAY_NUM="${DISPLAY_NUM%%.*}"

cleanup() {
    # Kill the whole process group so a stopped container does not leave a
    # stale X lock that blocks the next start.
    jobs -p | xargs -r kill 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "computer-control: starting Xvfb on ${DISPLAY} at ${SCREEN_GEOMETRY}" >&2
Xvfb "${DISPLAY}" -screen 0 "${SCREEN_GEOMETRY}" -nolisten tcp -ac +extension RANDR &

# Wait for the server rather than sleeping a fixed amount: a slow host makes a
# fixed sleep either flaky or needlessly slow.
for _ in $(seq 1 100); do
    if xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then break; fi
    sleep 0.1
done
if ! xdpyinfo -display "${DISPLAY}" >/dev/null 2>&1; then
    echo "computer-control: Xvfb failed to start on ${DISPLAY}" >&2
    exit 1
fi

echo "computer-control: starting openbox" >&2
openbox &
sleep 0.3

if [ ! -w /dev/uinput ]; then
    echo "computer-control: /dev/uinput is not writable - multi-touch gestures will be" >&2
    echo "                  emulated. Pass --device /dev/uinput to enable real touch." >&2
fi

case "${1:-serve}" in
    serve)
        exec computer-control-mcp \
            --transport "${CC_TRANSPORT}" --host "${CC_HOST}" --port "${CC_PORT}"
        ;;
    stdio)
        exec computer-control-mcp --transport stdio
        ;;
    doctor)
        exec cc doctor
        ;;
    shell)
        exec /bin/bash
        ;;
    *)
        exec "$@"
        ;;
esac
