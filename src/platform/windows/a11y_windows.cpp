// SPDX-License-Identifier: MIT
//
// Windows accessibility, on UI Automation.
//
// The thing that makes or breaks UIA performance is the cache request. A naive
// walk issues a cross-process COM call per property per element, and a
// Chromium or Electron window has tens of thousands of elements - that walk
// takes tens of seconds. Building a CacheRequest and using
// FindAllBuildCache fetches everything in one round trip per subtree, which is
// the difference between 50ms and 30s.
// <windows.h> must come before every other Windows SDK header: psapi.h and
// friends use BOOL, DWORD and WINAPI without declaring them. The blank lines
// keep clang-format from sorting these groups into one another.
#include <windows.h>

#include <objbase.h>
#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include "cc/accessibility.hpp"

namespace cc {
namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    explicit ComPtr(T* raw) : p(raw) {}
    ~ComPtr() {
        if (p) p->Release();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            if (p) p->Release();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    T** put() {
        if (p) {
            p->Release();
            p = nullptr;
        }
        return &p;
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string from_bstr(BSTR b) {
    if (!b) return {};
    const int len = static_cast<int>(::SysStringLen(b));
    if (len <= 0) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, b, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, n)), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, b, len, out.data(), n, nullptr, nullptr);
    return out;
}

struct BStr {
    BSTR b = nullptr;
    ~BStr() {
        if (b) ::SysFreeString(b);
    }
    BSTR* put() {
        if (b) {
            ::SysFreeString(b);
            b = nullptr;
        }
        return &b;
    }
    std::string str() const { return from_bstr(b); }
};

Role map_role(CONTROLTYPEID id) {
    switch (id) {
        case UIA_ButtonControlTypeId: return Role::Button;
        case UIA_CheckBoxControlTypeId: return Role::CheckBox;
        case UIA_RadioButtonControlTypeId: return Role::RadioButton;
        case UIA_ComboBoxControlTypeId: return Role::ComboBox;
        case UIA_EditControlTypeId: return Role::TextField;
        case UIA_DocumentControlTypeId: return Role::Document;
        case UIA_HyperlinkControlTypeId: return Role::Link;
        case UIA_ImageControlTypeId: return Role::Image;
        case UIA_ListItemControlTypeId: return Role::ListItem;
        case UIA_ListControlTypeId: return Role::List;
        case UIA_MenuControlTypeId: return Role::Menu;
        case UIA_MenuBarControlTypeId: return Role::MenuBar;
        case UIA_MenuItemControlTypeId: return Role::MenuItem;
        case UIA_ProgressBarControlTypeId: return Role::ProgressBar;
        case UIA_ScrollBarControlTypeId: return Role::ScrollBar;
        case UIA_SliderControlTypeId: return Role::Slider;
        case UIA_SpinnerControlTypeId: return Role::Stepper;
        case UIA_StatusBarControlTypeId: return Role::StatusBar;
        case UIA_TabControlTypeId: return Role::TabList;
        case UIA_TabItemControlTypeId: return Role::Tab;
        case UIA_TextControlTypeId: return Role::StaticText;
        case UIA_ToolBarControlTypeId: return Role::Toolbar;
        case UIA_TreeControlTypeId: return Role::Tree;
        case UIA_TreeItemControlTypeId: return Role::TreeItem;
        case UIA_CustomControlTypeId: return Role::Unknown;
        case UIA_GroupControlTypeId: return Role::Group;
        case UIA_DataGridControlTypeId: return Role::Table;
        case UIA_DataItemControlTypeId: return Role::Row;
        case UIA_TableControlTypeId: return Role::Table;
        case UIA_HeaderItemControlTypeId: return Role::ColumnHeader;
        case UIA_WindowControlTypeId: return Role::Window;
        case UIA_PaneControlTypeId: return Role::Group;
        case UIA_SplitButtonControlTypeId: return Role::Button;
        case UIA_CalendarControlTypeId: return Role::DatePicker;
        case UIA_SeparatorControlTypeId: return Role::Separator;
        default: return Role::Unknown;
    }
}

bool role_is_interactive(Role r) {
    switch (r) {
        case Role::Button:
        case Role::CheckBox:
        case Role::RadioButton:
        case Role::ComboBox:
        case Role::TextField:
        case Role::TextArea:
        case Role::SearchField:
        case Role::Link:
        case Role::ListItem:
        case Role::MenuItem:
        case Role::Tab:
        case Role::TreeItem:
        case Role::Slider:
        case Role::Stepper:
        case Role::Row:
        case Role::Cell:
        case Role::DatePicker: return true;
        default: return false;
    }
}

class WinA11y final : public AccessibilityBackend {
public:
    explicit WinA11y(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    ~WinA11y() override {
        automation_ = ComPtr<IUIAutomation>();
        if (com_initialized_) ::CoUninitialize();
    }

    std::string name() const override { return "UIAutomation"; }

    Status initialize() override {
        if (automation_) return ok();

        // Apartment-threaded: UIA marshals for us, and MTA makes reentrancy
        // bugs in third-party providers far more likely to bite.
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // The host already initialised COM differently; that is fine, we
            // just must not uninitialise it.
            com_initialized_ = false;
        } else if (FAILED(hr)) {
            return err(ErrorCode::BackendFailure, "CoInitializeEx failed");
        } else {
            com_initialized_ = true;
        }

        if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IUIAutomation),
                                      reinterpret_cast<void**>(automation_.put())))) {
            return err(ErrorCode::BackendFailure, "could not create the UIAutomation client",
                       "UIA is part of Windows; failure here usually means the process is "
                       "running in a session with no window station.");
        }
        return ok();
    }

    Status check_permission(bool) override {
        // UIA needs no separate grant. The only real restriction is UIPI:
        // reading an elevated app's tree from a non-elevated process returns
        // nothing, which surfaces as an empty subtree rather than an error.
        return initialize();
    }

    Result<Tree> snapshot(const TreeOptions& opts) override {
        if (auto st = initialize(); !st) return st.error();

        const auto start = Clock::now();
        deadline_ = start + opts.budget;
        nodes_left_ = opts.max_nodes;

        Tree tree;

        ComPtr<IUIAutomationCacheRequest> cache;
        if (auto st = build_cache(cache); !st) return st.error();

        if (opts.window_id.has_value()) {
            HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(*opts.window_id));
            ComPtr<IUIAutomationElement> el;
            if (FAILED(automation_->ElementFromHandleBuildCache(hwnd, cache.get(), el.put())) ||
                !el) {
                return err(ErrorCode::NotFound, "no UIA element for that window handle");
            }
            Node root;
            if (build(el.get(), root, 0, opts, cache.get(), tree))
                tree.roots.push_back(std::move(root));
        } else {
            ComPtr<IUIAutomationElement> root_el;
            if (FAILED(automation_->GetRootElementBuildCache(cache.get(), root_el.put())) ||
                !root_el) {
                return err(ErrorCode::BackendFailure, "GetRootElement failed");
            }
            // Walk the desktop's children (the top-level windows) rather than
            // the desktop itself, so a single unresponsive app cannot stall
            // everything behind it.
            ComPtr<IUIAutomationCondition> all;
            automation_->CreateTrueCondition(all.put());
            ComPtr<IUIAutomationElementArray> children;
            if (SUCCEEDED(root_el->FindAllBuildCache(TreeScope_Children, all.get(), cache.get(),
                                                     children.put())) &&
                children) {
                int count = 0;
                children->get_Length(&count);
                for (int i = 0; i < count; ++i) {
                    if (Clock::now() > deadline_ || nodes_left_ <= 0) {
                        tree.truncated = true;
                        tree.truncation_reason = "budget exhausted";
                        break;
                    }
                    ComPtr<IUIAutomationElement> child;
                    if (FAILED(children->GetElement(i, child.put())) || !child) continue;
                    if (opts.pid.has_value()) {
                        int pid = 0;
                        child->get_CachedProcessId(&pid);
                        if (pid != *opts.pid) continue;
                    }
                    Node node;
                    if (build(child.get(), node, 0, opts, cache.get(), tree)) {
                        tree.roots.push_back(std::move(node));
                    }
                }
            }
        }

        assign_labels(tree, opts);
        tree.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        return tree;
    }

    Result<Node> element_at(const Point& p) override {
        if (auto st = initialize(); !st) return st.error();
        const Point phys = displays_->convert(p, Space::Physical);
        POINT pt{static_cast<LONG>(std::lround(phys.x)), static_cast<LONG>(std::lround(phys.y))};

        ComPtr<IUIAutomationElement> el;
        if (FAILED(automation_->ElementFromPoint(pt, el.put())) || !el) {
            return err(ErrorCode::NotFound, "no UIA element at that point");
        }
        Node n;
        fill_live(el.get(), n);
        return n;
    }

    Result<Node> focused_element() override {
        if (auto st = initialize(); !st) return st.error();
        ComPtr<IUIAutomationElement> el;
        if (FAILED(automation_->GetFocusedElement(el.put())) || !el) {
            return err(ErrorCode::NotFound, "nothing has keyboard focus");
        }
        Node n;
        fill_live(el.get(), n);
        n.focused = true;
        return n;
    }

    Status perform_action(const Node& node, std::string_view action) override {
        if (auto st = initialize(); !st) return st.error();
        const Point phys = displays_->convert(node.bounds.center(), Space::Physical);
        POINT pt{static_cast<LONG>(std::lround(phys.x)), static_cast<LONG>(std::lround(phys.y))};
        ComPtr<IUIAutomationElement> el;
        if (FAILED(automation_->ElementFromPoint(pt, el.put())) || !el) {
            return err(ErrorCode::NotFound, "element is no longer at that position");
        }

        if (action == "press" || action == "invoke" || action == "click") {
            ComPtr<IUIAutomationInvokePattern> invoke;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId,
                                                  __uuidof(IUIAutomationInvokePattern),
                                                  reinterpret_cast<void**>(invoke.put()))) &&
                invoke) {
                return SUCCEEDED(invoke->Invoke())
                           ? ok()
                           : err(ErrorCode::BackendFailure, "Invoke failed");
            }
            ComPtr<IUIAutomationTogglePattern> toggle;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId,
                                                  __uuidof(IUIAutomationTogglePattern),
                                                  reinterpret_cast<void**>(toggle.put()))) &&
                toggle) {
                return SUCCEEDED(toggle->Toggle())
                           ? ok()
                           : err(ErrorCode::BackendFailure, "Toggle failed");
            }
            return err(ErrorCode::Unsupported, "the element supports neither Invoke nor Toggle",
                       "Click it by coordinate instead.");
        }
        if (action == "expand" || action == "collapse") {
            ComPtr<IUIAutomationExpandCollapsePattern> pattern;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                                                  __uuidof(IUIAutomationExpandCollapsePattern),
                                                  reinterpret_cast<void**>(pattern.put()))) &&
                pattern) {
                const HRESULT hr = (action == "expand") ? pattern->Expand() : pattern->Collapse();
                return SUCCEEDED(hr) ? ok()
                                     : err(ErrorCode::BackendFailure, "expand/collapse failed");
            }
        }
        return err(ErrorCode::Unsupported, "unsupported action '" + std::string(action) + "'");
    }

    Status set_value(const Node& node, std::string_view value) override {
        if (auto st = initialize(); !st) return st.error();
        const Point phys = displays_->convert(node.bounds.center(), Space::Physical);
        POINT pt{static_cast<LONG>(std::lround(phys.x)), static_cast<LONG>(std::lround(phys.y))};
        ComPtr<IUIAutomationElement> el;
        if (FAILED(automation_->ElementFromPoint(pt, el.put())) || !el) {
            return err(ErrorCode::NotFound, "element is no longer at that position");
        }
        ComPtr<IUIAutomationValuePattern> pattern;
        if (FAILED(el->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern),
                                           reinterpret_cast<void**>(pattern.put()))) ||
            !pattern) {
            return err(ErrorCode::Unsupported, "the element does not support the Value pattern",
                       "Click it and type instead.");
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(std::max(0, n)), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                              n);
        BSTR b = ::SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
        const HRESULT hr = pattern->SetValue(b);
        ::SysFreeString(b);
        return SUCCEEDED(hr) ? ok() : err(ErrorCode::BackendFailure, "SetValue failed");
    }

private:
    Status build_cache(ComPtr<IUIAutomationCacheRequest>& cache) {
        if (FAILED(automation_->CreateCacheRequest(cache.put())) || !cache) {
            return err(ErrorCode::BackendFailure, "CreateCacheRequest failed");
        }
        // Everything read during the walk must be cached, or each access falls
        // back to a cross-process call and the walk becomes unusably slow.
        for (PROPERTYID id :
             {UIA_NamePropertyId, UIA_ControlTypePropertyId, UIA_AutomationIdPropertyId,
              UIA_BoundingRectanglePropertyId, UIA_IsEnabledPropertyId, UIA_IsOffscreenPropertyId,
              UIA_HasKeyboardFocusPropertyId, UIA_ProcessIdPropertyId, UIA_ClassNamePropertyId,
              UIA_HelpTextPropertyId, UIA_IsInvokePatternAvailablePropertyId,
              UIA_IsValuePatternAvailablePropertyId, UIA_IsTogglePatternAvailablePropertyId,
              UIA_IsScrollPatternAvailablePropertyId, UIA_IsSelectionItemPatternAvailablePropertyId,
              UIA_ValueValuePropertyId, UIA_ToggleToggleStatePropertyId}) {
            cache->AddProperty(id);
        }
        cache->put_TreeScope(TreeScope_Element);
        // The raw tree includes every implementation detail; the control view
        // is what a user perceives and is an order of magnitude smaller.
        cache->put_AutomationElementMode(AutomationElementMode_Full);
        return ok();
    }

    void fill_cached(IUIAutomationElement* el, Node& n) {
        BStr name;
        el->get_CachedName(name.put());
        n.name = name.str();

        BStr automation_id;
        el->get_CachedAutomationId(automation_id.put());
        n.automation_id = automation_id.str();

        BStr class_name;
        el->get_CachedClassName(class_name.put());
        n.raw_role = class_name.str();

        BStr help;
        el->get_CachedHelpText(help.put());
        n.help = help.str();

        CONTROLTYPEID type = 0;
        el->get_CachedControlType(&type);
        n.role = map_role(type);

        RECT r{};
        el->get_CachedBoundingRectangle(&r);
        const Rect physical{static_cast<double>(r.left), static_cast<double>(r.top),
                            static_cast<double>(r.right - r.left),
                            static_cast<double>(r.bottom - r.top), Space::Physical};
        n.bounds = displays_->convert(physical, Space::Logical);

        BOOL enabled = TRUE, offscreen = FALSE, focused = FALSE;
        el->get_CachedIsEnabled(&enabled);
        el->get_CachedIsOffscreen(&offscreen);
        el->get_CachedHasKeyboardFocus(&focused);
        n.enabled = enabled != FALSE;
        n.visible = (offscreen == FALSE) && !n.bounds.empty();
        n.focused = focused != FALSE;

        int pid = 0;
        el->get_CachedProcessId(&pid);
        n.pid = pid;

        VARIANT v{};
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_IsInvokePatternAvailablePropertyId, &v)) &&
            v.vt == VT_BOOL && v.boolVal) {
            n.actions.push_back("press");
        }
        ::VariantClear(&v);
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_IsTogglePatternAvailablePropertyId, &v)) &&
            v.vt == VT_BOOL && v.boolVal) {
            n.actions.push_back("toggle");
            VARIANT state{};
            if (SUCCEEDED(el->GetCachedPropertyValue(UIA_ToggleToggleStatePropertyId, &state)) &&
                state.vt == VT_I4) {
                n.checked = (state.lVal == ToggleState_On);
            }
            ::VariantClear(&state);
        }
        ::VariantClear(&v);
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_IsScrollPatternAvailablePropertyId, &v)) &&
            v.vt == VT_BOOL && v.boolVal) {
            n.scrollable = true;
        }
        ::VariantClear(&v);
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_ValueValuePropertyId, &v)) &&
            v.vt == VT_BSTR) {
            n.value = from_bstr(v.bstrVal);
            n.actions.push_back("setValue");
        }
        ::VariantClear(&v);

        const bool has_invoke =
            std::find(n.actions.begin(), n.actions.end(), "press") != n.actions.end();
        n.interactive = n.enabled && n.visible && (has_invoke || role_is_interactive(n.role));
    }

    // Uncached path for single-element queries, where one round trip is fine.
    void fill_live(IUIAutomationElement* el, Node& n) {
        BStr name;
        el->get_CurrentName(name.put());
        n.name = name.str();

        BStr automation_id;
        el->get_CurrentAutomationId(automation_id.put());
        n.automation_id = automation_id.str();

        BStr class_name;
        el->get_CurrentClassName(class_name.put());
        n.raw_role = class_name.str();

        CONTROLTYPEID type = 0;
        el->get_CurrentControlType(&type);
        n.role = map_role(type);

        RECT r{};
        el->get_CurrentBoundingRectangle(&r);
        const Rect physical{static_cast<double>(r.left), static_cast<double>(r.top),
                            static_cast<double>(r.right - r.left),
                            static_cast<double>(r.bottom - r.top), Space::Physical};
        n.bounds = displays_->convert(physical, Space::Logical);

        BOOL enabled = TRUE, offscreen = FALSE;
        el->get_CurrentIsEnabled(&enabled);
        el->get_CurrentIsOffscreen(&offscreen);
        n.enabled = enabled != FALSE;
        n.visible = (offscreen == FALSE) && !n.bounds.empty();

        int pid = 0;
        el->get_CurrentProcessId(&pid);
        n.pid = pid;
        n.interactive = n.enabled && n.visible && role_is_interactive(n.role);
    }

    bool build(IUIAutomationElement* el, Node& out, int depth, const TreeOptions& opts,
               IUIAutomationCacheRequest* cache, Tree& tree) {
        if (depth > opts.max_depth || nodes_left_ <= 0 || Clock::now() > deadline_) {
            if (!tree.truncated) {
                tree.truncated = true;
                tree.truncation_reason =
                    (Clock::now() > deadline_) ? "time budget exhausted" : "node budget exhausted";
            }
            return false;
        }
        --nodes_left_;
        ++tree.node_count;
        fill_cached(el, out);

        if (!opts.include_offscreen && !out.visible && depth > 0) return true;
        if (opts.region && !out.bounds.empty() && !opts.region->intersects(out.bounds)) return true;

        ComPtr<IUIAutomationCondition> all;
        if (FAILED(automation_->CreateTrueCondition(all.put()))) return true;

        ComPtr<IUIAutomationElementArray> children;
        if (FAILED(el->FindAllBuildCache(TreeScope_Children, all.get(), cache, children.put())) ||
            !children) {
            return true;
        }
        int count = 0;
        children->get_Length(&count);
        out.children.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> child;
            if (FAILED(children->GetElement(i, child.put())) || !child) continue;
            Node node;
            if (build(child.get(), node, depth + 1, opts, cache, tree)) {
                out.children.push_back(std::move(node));
            }
        }
        return true;
    }

    ComPtr<IUIAutomation> automation_;
    bool com_initialized_ = false;
    Clock::time_point deadline_{};
    int nodes_left_ = 0;
};

}  // namespace

Result<std::unique_ptr<AccessibilityBackend>> AccessibilityBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    return std::unique_ptr<AccessibilityBackend>(new WinA11y(std::move(displays)));
}

}  // namespace cc
