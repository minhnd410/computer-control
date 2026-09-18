// SPDX-License-Identifier: MIT
//
// Windows screen capture, on GDI BitBlt.
//
// DXGI Desktop Duplication is faster for sustained capture (it hands back the
// composited frame without a copy through system memory), but it cannot be
// used from a session that is not the active console session, it needs a D3D
// device, and it fails on the secure desktop. For one-shot screenshots - which
// is what an automation server actually does - BitBlt from the screen DC is
// simpler, works everywhere, and the cost is dominated by PNG encoding anyway.

#include <dwmapi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "cc/screen.hpp"

namespace cc {
namespace {

// RAII for the GDI handles. There are five of them and an early return that
// leaks one leaks it for the lifetime of the process; the desktop eventually
// stops compositing.
struct GdiCapture {
    HDC screen = nullptr;
    HDC mem = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;

    ~GdiCapture() {
        if (mem && previous) ::SelectObject(mem, previous);
        if (bitmap) ::DeleteObject(bitmap);
        if (mem) ::DeleteDC(mem);
        if (screen) ::ReleaseDC(nullptr, screen);
    }
};

void draw_cursor(HDC dc, int origin_x, int origin_y) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!::GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ICONINFO ii{};
    if (!::GetIconInfo(ci.hCursor, &ii)) return;
    // The hotspot offset matters: drawing at the cursor position without it
    // puts the arrow's top-left where its tip should be.
    ::DrawIconEx(dc, ci.ptScreenPos.x - origin_x - static_cast<int>(ii.xHotspot),
                 ci.ptScreenPos.y - origin_y - static_cast<int>(ii.yHotspot), ci.hCursor, 0, 0, 0,
                 nullptr, DI_NORMAL);
    if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
    if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
}

class WinScreen final : public ScreenBackend {
public:
    explicit WinScreen(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    std::string name() const override { return "GDI BitBlt"; }
    Status initialize() override { return ok(); }

    Result<Frame> capture(const CaptureOptions& opts) override {
        Rect region_physical;

        if (opts.window_id.has_value()) {
            HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(*opts.window_id));
            if (!::IsWindow(hwnd)) {
                return err(ErrorCode::NotFound, "window handle is no longer valid");
            }
            RECT r{};
            // DwmGetWindowAttribute gives the visible frame; GetWindowRect
            // includes the invisible resize border on Aero, which shows up as
            // a transparent margin around the capture.
            if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
                ::GetWindowRect(hwnd, &r);
            }
            region_physical = Rect{static_cast<double>(r.left), static_cast<double>(r.top),
                                   static_cast<double>(r.right - r.left),
                                   static_cast<double>(r.bottom - r.top), Space::Physical};
        } else if (opts.display_index.has_value()) {
            const Display* d = displays_->by_index(*opts.display_index);
            if (!d) {
                return err(ErrorCode::NotFound,
                           "no display at index " + std::to_string(*opts.display_index));
            }
            region_physical = d->bounds_physical;
        } else if (opts.region.has_value()) {
            region_physical = displays_->convert(*opts.region, Space::Physical);
        } else {
            region_physical = displays_->virtual_bounds(Space::Physical);
        }

        if (opts.region.has_value() && (opts.window_id || opts.display_index)) {
            // A region inside a chosen source: intersect rather than replace.
            const Rect r = displays_->convert(*opts.region, Space::Physical);
            const double x = std::max(region_physical.x, r.x);
            const double y = std::max(region_physical.y, r.y);
            region_physical =
                Rect{x, y, std::min(region_physical.right(), r.right()) - x,
                     std::min(region_physical.bottom(), r.bottom()) - y, Space::Physical};
        }

        const int w = static_cast<int>(std::lround(region_physical.w));
        const int h = static_cast<int>(std::lround(region_physical.h));
        if (w <= 0 || h <= 0) {
            return err(ErrorCode::InvalidArgument, "capture region is empty");
        }

        GdiCapture g;
        g.screen = ::GetDC(nullptr);
        if (!g.screen) {
            return err(ErrorCode::BackendFailure, "GetDC failed",
                       "A process in session 0 has no desktop to capture. Run it in an "
                       "interactive session.");
        }
        g.mem = ::CreateCompatibleDC(g.screen);
        if (!g.mem) return err(ErrorCode::BackendFailure, "CreateCompatibleDC failed");

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        // Negative height requests a top-down DIB. Without it the rows come
        // back bottom-up and the image is vertically mirrored.
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        g.bitmap = ::CreateDIBSection(g.screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!g.bitmap || !bits) {
            return err(ErrorCode::BackendFailure, "CreateDIBSection failed");
        }
        g.previous = ::SelectObject(g.mem, g.bitmap);

        const int src_x = static_cast<int>(std::lround(region_physical.x));
        const int src_y = static_cast<int>(std::lround(region_physical.y));
        // CAPTUREBLT is required to include layered and transparent windows,
        // which is most of a modern desktop's chrome.
        if (!::BitBlt(g.mem, 0, 0, w, h, g.screen, src_x, src_y, SRCCOPY | CAPTUREBLT)) {
            return err(ErrorCode::BackendFailure, "BitBlt failed",
                       "Capture is blocked on the secure desktop (UAC prompt, lock screen) and "
                       "for windows using SetWindowDisplayAffinity.");
        }
        if (opts.include_cursor) draw_cursor(g.mem, src_x, src_y);

        Frame f;
        f.width = w;
        f.height = h;
        f.format = PixelFormat::BGRA8;
        f.stride = w * 4;
        f.pixels.resize(static_cast<std::size_t>(f.stride) * h);
        std::memcpy(f.pixels.data(), bits, f.pixels.size());
        f.source_physical = region_physical;
        f.scale = 1.0;

        // GDI leaves the alpha channel as zero, which makes a PNG fully
        // transparent. The desktop is opaque, so force it.
        for (std::size_t i = 3; i < f.pixels.size(); i += 4) f.pixels[i] = 255;

        return post_process(std::move(f), opts);
    }

    Result<std::uint32_t> pixel(const Point& at) override {
        const Point phys = displays_->convert(at, Space::Physical);
        HDC dc = ::GetDC(nullptr);
        if (!dc) return err(ErrorCode::BackendFailure, "GetDC failed");
        const COLORREF c = ::GetPixel(dc, static_cast<int>(std::lround(phys.x)),
                                      static_cast<int>(std::lround(phys.y)));
        ::ReleaseDC(nullptr, dc);
        if (c == CLR_INVALID) {
            return err(ErrorCode::BackendFailure, "GetPixel failed; the point may be off-screen");
        }
        return (static_cast<std::uint32_t>(GetRValue(c)) << 24) |
               (static_cast<std::uint32_t>(GetGValue(c)) << 16) |
               (static_cast<std::uint32_t>(GetBValue(c)) << 8) | 0xFFu;
    }

private:
    Result<Frame> post_process(Frame f, const CaptureOptions& opts) {
        double factor = (opts.scale > 0 && opts.scale != 1.0) ? opts.scale : 1.0;
        if (opts.max_dimension > 0) {
            const int longest = std::max(f.width, f.height);
            if (longest * factor > opts.max_dimension) {
                factor = static_cast<double>(opts.max_dimension) / longest;
            }
        }
        if (factor != 1.0) {
            auto r = resize(f, std::max(1, static_cast<int>(std::lround(f.width * factor))),
                            std::max(1, static_cast<int>(std::lround(f.height * factor))));
            if (!r) return r;
            f = std::move(r.value());
        }
        displays_->set_image_transform(f.transform());
        return f;
    }
};

}  // namespace

Result<std::unique_ptr<ScreenBackend>> ScreenBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<WinScreen>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<ScreenBackend>(std::move(backend));
}

}  // namespace cc
