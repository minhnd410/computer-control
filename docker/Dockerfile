# syntax=docker/dockerfile:1
#
# A headless Linux desktop with computer-control on it.
#
# Important: this controls the *container's* display, not the host's. A
# container has no access to the host window system, so this is a sandbox for
# browser automation, scraping and CI - not a remote control for your machine.

# ---- build ----------------------------------------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git \
        libx11-dev libxtst-dev libxrandr-dev libxfixes-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Copy the build inputs only, so editing the README does not invalidate the
# compile cache.
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY tests/ tests/

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ./build/cc_tests

# ---- runtime --------------------------------------------------------------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        # X server and a window manager. Without an EWMH-compliant WM the
        # window tools have nothing to talk to, and openbox is the smallest
        # one that implements the hints properly.
        xvfb x11-utils openbox \
        libx11-6 libxtst6 libxrandr2 libxfixes3 zlib1g \
        # Clipboard needs a resident helper on X11.
        xclip \
        # Accessibility tree, and notifications.
        at-spi2-core libnotify-bin \
        # Android device support over adb.
        android-tools-adb \
        ca-certificates tini \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/cc /usr/local/bin/cc
COPY --from=build /src/build/computer-control-mcp /usr/local/bin/computer-control-mcp
COPY --from=build /src/build/libcomputer_control.so* /usr/local/lib/
RUN ldconfig

# Run as a normal user: a container that can inject input should not also be
# root. The uinput group matches the common host gid so `--device /dev/uinput`
# works without extra flags on most distributions.
RUN groupadd -g 995 uinput || true \
 && useradd -m -s /bin/bash -G uinput automation \
 # Xvfb wants to create /tmp/.X11-unix itself and cannot as a non-root user,
 # which prints an alarming "euid != 0" error even though the server then
 # starts fine over the abstract socket. Creating it up front keeps the logs
 # honest.
 && mkdir -p /tmp/.X11-unix \
 && chmod 1777 /tmp/.X11-unix

# openbox looks for the Debian menu at a fixed path and logs a warning on every
# start when it is missing; the package that supplies it drags in a large
# dependency tree. An empty menu is the right answer for a headless container.
# This has to happen while still root - /var/lib is not writable afterwards.
RUN mkdir -p /var/lib/openbox /home/automation/.config/openbox \
 && printf '<?xml version="1.0" encoding="UTF-8"?>\n<openbox_menu/>\n' \
      | tee /var/lib/openbox/debian-menu.xml \
            /home/automation/.config/openbox/menu.xml > /dev/null \
 && chown -R automation:automation /home/automation/.config

USER automation
WORKDIR /home/automation

ENV DISPLAY=:99 \
    SCREEN_GEOMETRY=1920x1080x24 \
    CC_TRANSPORT=http \
    CC_HOST=0.0.0.0 \
    CC_PORT=8765

# openbox logs a warning on every start when the Debian menu file is absent
# and the package that provides it pulls in a large dependency tree. An empty
# menu is the right answer for a headless container.
COPY --chown=automation:automation docker/entrypoint.sh /usr/local/bin/entrypoint.sh

EXPOSE 8765

# tini reaps Xvfb and openbox; without an init, a container stop leaves
# zombies and the X lock file behind.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]
CMD ["serve"]
