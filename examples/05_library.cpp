// SPDX-License-Identifier: MIT
//
// Using the library directly, for embedding it in a C++ program.
//
// Build against a local tree:
//   c++ -std=c++20 -Iinclude -Isrc examples/05_library.cpp build/libcomputer_control.a \
//       -framework CoreGraphics -framework ApplicationServices -framework Foundation \
//       -framework AppKit -framework ScreenCaptureKit -framework IOKit -lz -o library_demo
//
// Read-only: it captures and reports, and only moves the pointer.

#include <chrono>
#include <cstdio>

#include "cc/session.hpp"

int main() {
    auto session = cc::Session::create();
    if (!session) {
        std::printf("cannot start a session: %s\n", session.error().message.c_str());
        if (!session.error().remedy.empty()) {
            std::printf("\n%s\n", session.error().remedy.c_str());
        }
        return 1;
    }
    auto& s = *session.value();

    auto displays = s.displays();
    if (!displays) return 1;
    for (const auto& d : displays.value()->displays()) {
        std::printf("[%d] %s  %.0fx%.0f pt @%gx\n", d.index, d.name.c_str(),
                    d.bounds_logical.w, d.bounds_logical.h, d.scale);
    }

    auto input = s.input();
    if (!input) {
        std::printf("\ninput unavailable: %s\n", input.error().message.c_str());
        return 1;
    }

    // The library never blocks on a round trip the way an out-of-process
    // front-end does, so the interesting cost here is the OS calls themselves.
    const auto start = std::chrono::steady_clock::now();

    auto where = input.value()->cursor_position();
    if (where) {
        cc::MotionOptions motion;
        // Human motion is seeded here so the path is identical on every run,
        // which is what makes it usable in a test.
        motion.profile = cc::MotionProfile::Human;
        motion.duration = std::chrono::milliseconds{250};
        motion.seed = 42;

        const cc::Point target{where.value().x + 120, where.value().y + 80};
        if (auto st = input.value()->move(target, motion); !st) {
            std::printf("move failed: %s\n", st.error().message.c_str());
        }
        (void)input.value()->move(where.value(), motion);   // put it back
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::printf("\ntwo human-profile moves in %lldms\n", static_cast<long long>(elapsed.count()));

    // Gesture fidelity, the thing worth checking before designing around it.
    const auto pinch = input.value()->gesture_support(cc::GestureKind::Pinch, 2);
    std::printf("pinch: %s via %s\n",
                pinch.fidelity == cc::GestureFidelity::Native ? "native" : "emulated",
                pinch.backend.c_str());

    // Session's destructor releases everything held, including on an exception
    // path, so a desktop is never left with a stuck button.
    return 0;
}
