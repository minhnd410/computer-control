// SPDX-License-Identifier: MIT
#include <algorithm>
#include <chrono>
#include <functional>

#include "cc/accessibility.hpp"

namespace cc {

const char* to_string(Role r) noexcept {
    switch (r) {
        case Role::Application: return "application";
        case Role::Window: return "window";
        case Role::Dialog: return "dialog";
        case Role::Sheet: return "sheet";
        case Role::Popover: return "popover";
        case Role::Menu: return "menu";
        case Role::MenuItem: return "menuitem";
        case Role::MenuBar: return "menubar";
        case Role::Button: return "button";
        case Role::ToggleButton: return "togglebutton";
        case Role::RadioButton: return "radio";
        case Role::CheckBox: return "checkbox";
        case Role::Link: return "link";
        case Role::Tab: return "tab";
        case Role::TabList: return "tablist";
        case Role::TextField: return "textfield";
        case Role::TextArea: return "textarea";
        case Role::SearchField: return "searchfield";
        case Role::SecureTextField: return "securefield";
        case Role::ComboBox: return "combobox";
        case Role::List: return "list";
        case Role::ListItem: return "listitem";
        case Role::Table: return "table";
        case Role::Row: return "row";
        case Role::Cell: return "cell";
        case Role::ColumnHeader: return "columnheader";
        case Role::Tree: return "tree";
        case Role::TreeItem: return "treeitem";
        case Role::Slider: return "slider";
        case Role::ProgressBar: return "progressbar";
        case Role::Stepper: return "stepper";
        case Role::ScrollArea: return "scrollarea";
        case Role::ScrollBar: return "scrollbar";
        case Role::Group: return "group";
        case Role::Toolbar: return "toolbar";
        case Role::StatusBar: return "statusbar";
        case Role::Image: return "image";
        case Role::StaticText: return "text";
        case Role::Separator: return "separator";
        case Role::Canvas: return "canvas";
        case Role::WebView: return "webview";
        case Role::Document: return "document";
        case Role::Disclosure: return "disclosure";
        case Role::DatePicker: return "datepicker";
        case Role::ColorWell: return "colorwell";
        default: return "unknown";
    }
}

void assign_labels(Tree& tree, const TreeOptions& opts) {
    tree.interactive.clear();
    tree.node_count = 0;
    std::int32_t next = 0;

    // Depth-first in document order: labels then read top-to-bottom,
    // left-to-right within a container, which is how a person describes a UI
    // and therefore how an agent's reasoning about "the third button" lands.
    std::function<void(Node&)> walk = [&](Node& n) {
        ++tree.node_count;
        const bool clipped = opts.region && !opts.region->intersects(n.bounds);
        const bool visible_enough = n.visible || opts.include_offscreen;
        if (n.interactive && visible_enough && !clipped && !n.bounds.empty()) {
            n.label = next++;
        } else {
            n.label = -1;
        }
        for (auto& c : n.children) walk(c);
    };
    for (auto& r : tree.roots) walk(r);

    // Second pass collects pointers, after the tree has stopped moving. Doing
    // it during the walk would leave dangling pointers as vectors reallocate.
    std::function<void(const Node&)> collect = [&](const Node& n) {
        if (n.label >= 0) tree.interactive.push_back(&n);
        for (const auto& c : n.children) collect(c);
    };
    for (const auto& r : tree.roots) collect(r);

    std::sort(tree.interactive.begin(), tree.interactive.end(),
              [](const Node* a, const Node* b) { return a->label < b->label; });
}

}  // namespace cc
