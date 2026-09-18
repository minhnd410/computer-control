// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "cc/accessibility.hpp"
#include "cc/device.hpp"
#include "cc/display.hpp"
#include "cc/input.hpp"
#include "cc/screen.hpp"
#include "cc/system.hpp"
#include "cc/types.hpp"
#include "cc/window.hpp"

namespace cc {

struct SessionConfig {
    // Backends are created lazily; a caller that only needs screenshots never
    // pays for an accessibility-permission prompt.
    bool eager_init = false;
    // Prompt the OS for accessibility/screen-recording access on first use
    // instead of failing. Off by default so headless/CI runs fail fast rather
    // than hanging on an invisible dialog.
    bool prompt_for_permissions = false;

    // Safety rails. The MCP server and CLI expose these as flags; they are
    // enforced here so every front-end gets the same behaviour.
    bool allow_shell = true;
    bool allow_filesystem = true;
    bool allow_registry = false;
    bool allow_clipboard = true;
    // Refuse input while the screen is locked or a screensaver is active.
    bool block_when_locked = true;

    std::int32_t default_max_capture_dimension = 1600;
};

// The single entry point. Owns the backends and the display topology, and
// guarantees that everything held down (mouse buttons, modifier keys, touch
// contacts) is released when it is destroyed, including on an exception path.
class Session {
public:
    static Result<std::shared_ptr<Session>> create(SessionConfig cfg = {});
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const SessionConfig& config() const noexcept { return cfg_; }
    SessionConfig& mutable_config() noexcept { return cfg_; }

    Result<DisplayGraph*> displays();
    Result<InputBackend*> input();
    Result<ScreenBackend*> screen();
    Result<WindowBackend*> windows();
    Result<AccessibilityBackend*> accessibility();
    Result<SystemBackend*> system();
    Result<DeviceManager*> devices();

    // Resolves a coordinate given in any space into the Logical space the
    // input backends require, and rejects points outside every display, which
    // is otherwise a silent no-op that looks like a broken click.
    Result<Point> resolve(const Point& p);

    // Human-readable capability report: which backends came up, which
    // permissions are missing, what gesture fidelity is available. This is
    // what the `Capabilities` tool and `cc doctor` print.
    std::string capability_report();

    Status release_all();

private:
    Session() = default;

    SessionConfig cfg_{};
    std::shared_ptr<DisplayGraph> displays_;
    std::unique_ptr<InputBackend> input_;
    std::unique_ptr<ScreenBackend> screen_;
    std::unique_ptr<WindowBackend> windows_;
    std::unique_ptr<AccessibilityBackend> a11y_;
    std::unique_ptr<SystemBackend> system_;
    std::shared_ptr<DeviceManager> devices_;
};

// Build/version identity, reported by every front-end.
struct BuildInfo {
    const char* version;
    const char* commit;
    const char* platform;
    const char* compiler;
    bool gestures_native;
};
BuildInfo build_info();

}  // namespace cc
