# Docker images

```bash
docker build -t computer-control -f docker/Dockerfile .
docker run --rm -p 8765:8765 computer-control          # HTTP MCP on :8765
docker run --rm computer-control doctor                # capability report
docker run --rm -it computer-control shell             # poke around
```

## What you get

A Debian image with Xvfb, openbox, `xclip`, AT-SPI2, `adb`, and both binaries.
`DISPLAY=:99` at 1920x1080; override with `-e SCREEN_GEOMETRY=2560x1440x24`.

## What you do not get

- **Control of the host desktop.** A container cannot reach the host's display
  server. This is a sandbox.
- **iOS simulators.** They require macOS and Xcode.
- **GPU rendering** by default. Software rendering only, so capturing 3D or
  video content is slow.

## Real multi-touch

```bash
docker run --rm --device /dev/uinput -p 8765:8765 computer-control
```

Needs `uinput` loaded on the host (`sudo modprobe uinput`). Note that this
grants the container input-injection rights against the host kernel; decide
whether that is acceptable before using it.

## Android devices

```bash
# adb server on the host
docker run --rm --network host \
  -e ADB_SERVER_SOCKET=tcp:localhost:5037 computer-control

# or pass the USB device straight through
docker run --rm --device /dev/bus/usb -p 8765:8765 computer-control
```
