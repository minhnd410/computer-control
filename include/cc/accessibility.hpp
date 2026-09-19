// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cc/display.hpp"
#include "cc/types.hpp"

namespace cc {

// A normalised control role. The three platform accessibility APIs disagree on
// naming for the same widget (AXButton / UIA Button / ATSPI push button), so
// everything is mapped onto this set and the raw role is kept alongside.
enum class Role : std::uint16_t {
    Unknown = 0,
    Application,
    Window,
    Dialog,
    Sheet,
    Popover,
    Menu,
    MenuItem,
    MenuBar,
    Button,
    ToggleButton,
    RadioButton,
    CheckBox,
    Link,
    Tab,
    TabList,
    TextField,
    TextArea,
    SearchField,
    SecureTextField,
    ComboBox,
    List,
    ListItem,
    Table,
    Row,
    Cell,
    ColumnHeader,
    Tree,
    TreeItem,
    Slider,
    ProgressBar,
    Stepper,
    ScrollArea,
    ScrollBar,
    Group,
    Toolbar,
    StatusBar,
    Image,
    StaticText,
    Separator,
    Canvas,
    WebView,
    Document,
    Disclosure,
    DatePicker,
    ColorWell,
    Unknown_Max,
};

const char* to_string(Role r) noexcept;

struct Node {
    std::int32_t label = -1;  // stable index assigned per-snapshot, -1 = not interactive
    Role role = Role::Unknown;
    std::string raw_role;
    std::string name;  // accessible name / title
    std::string value;
    std::string description;
    std::string help;
    std::string automation_id;  // UIA AutomationId / AXIdentifier / ATSPI id
    Rect bounds;                // logical space, absolute
    bool enabled = true;
    bool focused = false;
    bool visible = true;
    bool interactive = false;  // clickable / editable / selectable
    bool scrollable = false;
    std::optional<bool> checked;
    std::optional<bool> expanded;
    std::optional<bool> selected;
    std::int64_t pid = 0;
    std::uint64_t window_id = 0;
    std::string app_name;
    std::vector<std::string> actions;  // "press", "showMenu", "increment", ...
    std::vector<Node> children;

    Point click_point() const { return bounds.center(); }
};

struct TreeOptions {
    // Depth and node budgets exist because a full accessibility walk of a
    // modern Electron app can return tens of thousands of nodes and take
    // seconds. The defaults keep a snapshot under ~100ms on typical desktops.
    int max_depth = 60;
    int max_nodes = 4000;
    std::chrono::milliseconds budget{1200};
    bool interactive_only = true;
    bool include_offscreen = false;
    bool include_static_text = true;
    // Restrict the walk to one process or window. Much faster and usually what
    // you want once you know which app you are driving.
    std::optional<std::int64_t> pid;
    std::optional<std::uint64_t> window_id;
    // Clip to this region (logical). Nodes fully outside are dropped.
    std::optional<Rect> region;
};

struct Tree {
    std::vector<Node> roots;
    std::vector<const Node*> interactive;  // flattened, label order
    int node_count = 0;
    bool truncated = false;
    std::string truncation_reason;
    std::chrono::milliseconds elapsed{0};
};

// A scrollable region. `vertical` is 0 at the top and 1 at the bottom;
// -1 means the application exposes no scroll bar to read.
struct ScrollRegion {
    Rect bounds;
    double vertical = -1;
    bool at_end = false;
};

// One entry in an application's menu bar. `path` is what invoke_menu takes.
struct MenuEntry {
    std::vector<std::string> path;  // e.g. {"File", "Save As\u2026"}
    std::string title;
    std::string shortcut;  // as shown to a user, e.g. "cmd+shift+s"
    bool enabled = true;
    bool has_submenu = false;
    bool separator = false;
};

class AccessibilityBackend {
public:
    virtual ~AccessibilityBackend() = default;

    static Result<std::unique_ptr<AccessibilityBackend>> create(
        std::shared_ptr<DisplayGraph> displays);

    virtual std::string name() const = 0;
    virtual Status initialize() = 0;
    // False when the OS has not granted accessibility access. `remedy` on the
    // error explains exactly which setting to flip.
    virtual Status check_permission(bool prompt) = 0;

    virtual Result<Tree> snapshot(const TreeOptions& opts) = 0;
    virtual Result<Node> element_at(const Point& p) = 0;
    virtual Result<Node> focused_element() = 0;
    virtual Status perform_action(const Node& node, std::string_view action) = 0;
    virtual Status set_value(const Node& node, std::string_view value) = 0;

    // The menu bar, which the ordinary tree walk does not reach: a menu's
    // contents are a separate hierarchy that most toolkits populate only when
    // the menu opens. Reading it through the accessibility API gets the whole
    // command surface of an application without opening anything, and
    // invoking a path is far more reliable than driving menus by pixel.
    virtual Result<std::vector<MenuEntry>> menu_bar(int pid, int depth) = 0;
    virtual Status invoke_menu(int pid, const std::vector<std::string>& path) = 0;

    // Scrollable regions and how far through each one the view currently is.
    // Without this, anything that scrolls has to guess when it has reached the
    // bottom - usually by capturing the screen and comparing, which costs a
    // screenshot per step and is fooled by a blinking cursor.
    virtual Result<std::vector<ScrollRegion>> scroll_regions(int pid) = 0;

protected:
    std::shared_ptr<DisplayGraph> displays_;
};

// Flattens, assigns labels, and filters. Shared by all backends so labelling
// is identical across platforms.
void assign_labels(Tree& tree, const TreeOptions& opts);

}  // namespace cc
