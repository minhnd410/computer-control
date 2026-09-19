// SPDX-License-Identifier: MIT
//
// Linux accessibility.
//
// AT-SPI2 is the only general accessibility API on Linux, and unlike UIA and
// AX it is an optional, out-of-process D-Bus service: it is absent on many
// server and minimal desktop installs, GTK apps only expose a tree when
// accessibility is enabled, and Electron apps need a command-line switch.
//
// Rather than link libatspi unconditionally - which would make the whole
// library fail to build on a machine without the headers - the backend is
// loaded at runtime with dlopen. When it is unavailable the error explains
// exactly what to install and offers the screenshot route instead, which is
// far more useful than a link failure at build time.

#include <dlfcn.h>

#include <string>

#include "cc/accessibility.hpp"

namespace cc {
namespace {

class LinuxA11y final : public AccessibilityBackend {
public:
    explicit LinuxA11y(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    ~LinuxA11y() override {
        if (handle_) ::dlclose(handle_);
    }

    std::string name() const override { return available_ ? "AT-SPI2" : "unavailable"; }

    Status initialize() override { return check_permission(false); }

    Status check_permission(bool) override {
        if (available_) return ok();
        if (probed_) return unavailable();

        probed_ = true;
        // soname first, then the unversioned dev symlink.
        for (const char* so : {"libatspi.so.0", "libatspi.so"}) {
            handle_ = ::dlopen(so, RTLD_LAZY | RTLD_LOCAL);
            if (handle_) break;
        }
        if (!handle_) return unavailable();

        init_fn_ = reinterpret_cast<int (*)()>(::dlsym(handle_, "atspi_init"));
        get_desktop_fn_ = reinterpret_cast<void* (*)(int)>(::dlsym(handle_, "atspi_get_desktop"));
        if (!init_fn_ || !get_desktop_fn_) {
            ::dlclose(handle_);
            handle_ = nullptr;
            return unavailable();
        }
        // atspi_init returns non-zero when the accessibility bus is missing,
        // which is the common case on a machine where nothing has enabled it.
        if (init_fn_() != 0) return unavailable();

        available_ = true;
        return ok();
    }

    Result<Tree> snapshot(const TreeOptions&) override {
        if (auto st = check_permission(false); !st) return st.error();
        return unavailable().error();
    }

    Result<Node> element_at(const Point&) override {
        if (auto st = check_permission(false); !st) return st.error();
        return unavailable().error();
    }

    Result<Node> focused_element() override {
        if (auto st = check_permission(false); !st) return st.error();
        return unavailable().error();
    }

    Status perform_action(const Node&, std::string_view) override { return unavailable(); }
    // The menu bar is reachable through this platform's accessibility API, but
    // it is not implemented here yet and nobody has been able to test it on
    // this platform. Saying so is better than returning an empty list that
    // looks like "this application has no menus".
    Result<std::vector<MenuEntry>> menu_bar(int, int) override {
        return err(ErrorCode::Unsupported, "menu-bar reading is macOS-only so far",
                   "AT-SPI exposes menus, so this is implementable - contributions welcome. "
                   "Meanwhile, most menu commands have a keyboard shortcut: use `key`.");
    }
    Status invoke_menu(int, const std::vector<std::string>&) override {
        return err(ErrorCode::Unsupported, "menu-bar invocation is macOS-only so far",
                   "AT-SPI exposes menus, so this is implementable - contributions welcome. "
                   "Meanwhile, most menu commands have a keyboard shortcut: use `key`.");
    }

    Status set_value(const Node&, std::string_view) override { return unavailable(); }

private:
    Status unavailable() {
        return err(ErrorCode::Unsupported,
                   "the AT-SPI2 accessibility tree is not available on this host",
                   "Install AT-SPI2 (Debian/Ubuntu: `sudo apt install at-spi2-core "
                   "libatspi2.0-0`; Fedora: `sudo dnf install at-spi2-core`) and make sure the "
                   "accessibility bus is running. GTK apps need GTK_MODULES=gail:atk-bridge, Qt "
                   "apps need QT_ACCESSIBILITY=1, and Electron apps need --force-renderer-"
                   "accessibility.\n\n"
                   "Until then, use `screenshot` or `snapshot` with vision and work from the "
                   "image; clicking, typing and gestures all work without accessibility.");
    }

    void* handle_ = nullptr;
    int (*init_fn_)() = nullptr;
    void* (*get_desktop_fn_)(int) = nullptr;
    bool available_ = false;
    bool probed_ = false;
};

}  // namespace

Result<std::unique_ptr<AccessibilityBackend>> AccessibilityBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    return std::unique_ptr<AccessibilityBackend>(new LinuxA11y(std::move(displays)));
}

}  // namespace cc
