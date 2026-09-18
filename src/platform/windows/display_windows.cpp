// SPDX-License-Identifier: MIT
// <windows.h> must come before every other Windows SDK header: psapi.h and
// friends use BOOL, DWORD and WINAPI without declaring them. The blank lines
// keep clang-format from sorting these groups into one another.
#include <windows.h>

#include <shellscalingapi.h>

#include <algorithm>
#include <vector>

#include "cc/display.hpp"

namespace cc {
namespace {

// Per-monitor DPI awareness has to be set before the first window or monitor
// query, and it changes what every coordinate in the process means. Without
// it Windows lies to us: GetSystemMetrics reports a virtualised 96-DPI desktop
// and every click on a 150%-scaled monitor lands in the wrong place.
//
// The manifest is the supported way to declare this, but a library cannot ship
// one for its host, so it is set programmatically at first use and the newest
// available API is preferred.
void ensure_dpi_awareness() {
    static const bool done = [] {
        if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
            using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            if (auto fn = reinterpret_cast<SetCtxFn>(
                    ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
                // PER_MONITOR_AWARE_V2 also fixes non-client area scaling,
                // which V1 leaves broken on mixed-DPI setups.
                if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;
                if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return true;
            }
        }
        if (HMODULE shcore = ::LoadLibraryW(L"Shcore.dll")) {
            using SetAwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);
            if (auto fn = reinterpret_cast<SetAwarenessFn>(
                    ::GetProcAddress(shcore, "SetProcessDpiAwareness"))) {
                if (SUCCEEDED(fn(PROCESS_PER_MONITOR_DPI_AWARE))) return true;
            }
        }
        // Last resort on Windows 7: system-wide awareness. Better than
        // virtualised, wrong on mixed-DPI.
        ::SetProcessDPIAware();
        return true;
    }();
    (void)done;
}

double dpi_for_monitor(HMONITOR monitor) {
    if (HMODULE shcore = ::LoadLibraryW(L"Shcore.dll")) {
        using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
        if (auto fn = reinterpret_cast<GetDpiFn>(::GetProcAddress(shcore, "GetDpiForMonitor"))) {
            UINT x = 96, y = 96;
            if (SUCCEEDED(fn(monitor, MDT_EFFECTIVE_DPI, &x, &y))) {
                return static_cast<double>(x);
            }
        }
    }
    return 96.0;
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

Orientation orientation_from(DWORD rotation) {
    switch (rotation) {
        case DMDO_90: return Orientation::Portrait;
        case DMDO_180: return Orientation::LandscapeFlipped;
        case DMDO_270: return Orientation::PortraitFlipped;
        default: return Orientation::Landscape;
    }
}

struct EnumState {
    std::vector<Display>* out;
};

BOOL CALLBACK enum_proc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* state = reinterpret_cast<EnumState*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) return TRUE;

    Display d;
    d.id = reinterpret_cast<std::uint64_t>(monitor);
    d.name = narrow(info.szDevice);
    d.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    // With per-monitor awareness, the rects Windows reports are already
    // physical pixels. Logical points are physical divided by the scale, which
    // is the convention the rest of the library uses.
    const double dpi = dpi_for_monitor(monitor);
    const double scale = dpi / 96.0;

    const auto& r = info.rcMonitor;
    const auto& w = info.rcWork;
    d.bounds_physical = Rect{static_cast<double>(r.left), static_cast<double>(r.top),
                             static_cast<double>(r.right - r.left),
                             static_cast<double>(r.bottom - r.top), Space::Physical};
    d.bounds_logical = Rect{r.left / scale, r.top / scale, (r.right - r.left) / scale,
                            (r.bottom - r.top) / scale, Space::Logical};
    d.work_area_logical = Rect{w.left / scale, w.top / scale, (w.right - w.left) / scale,
                               (w.bottom - w.top) / scale, Space::Logical};
    d.scale = scale;
    d.dpi = dpi;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        d.refresh_hz = static_cast<double>(mode.dmDisplayFrequency);
        if (mode.dmFields & DM_DISPLAYORIENTATION) {
            d.orientation = orientation_from(mode.dmDisplayOrientation);
        }
    }
    if (d.refresh_hz <= 1) d.refresh_hz = 60.0;

    state->out->push_back(std::move(d));
    return TRUE;
}

}  // namespace

Status platform_enumerate_displays(std::vector<Display>& out) {
    ensure_dpi_awareness();
    out.clear();

    EnumState state{&out};
    if (!::EnumDisplayMonitors(nullptr, nullptr, enum_proc, reinterpret_cast<LPARAM>(&state))) {
        return err(ErrorCode::BackendFailure, "EnumDisplayMonitors failed");
    }
    if (out.empty()) {
        return err(ErrorCode::BackendFailure, "no monitors reported",
                   "On a headless Windows host (a CI runner or a Server Core install) there is "
                   "no display to capture. Attach a virtual display driver or run in an RDP "
                   "session, which creates one.");
    }
    return ok();
}

}  // namespace cc
