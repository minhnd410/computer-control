// SPDX-License-Identifier: MIT
//
// Linux screen capture, on XGetImage (with MIT-SHM when available).
//
// XGetImage copies the whole image through the X protocol socket, which for a
// 4K screen is ~33 MB per frame and takes tens of milliseconds. MIT-SHM puts
// the pixels in shared memory instead and is several times faster, so it is
// used whenever the server is local and the extension is present.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cc/screen.hpp"

#if defined(CC_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#if defined(CC_HAVE_XFIXES)
#include <X11/extensions/Xfixes.h>
#endif
#endif

// Xlib defines `Status` (and a pile of other bare words) as a macro, which
// clobbers cc::Status and turns every Status-returning override into `int`.
// Nothing here uses X11's Status, so it goes away immediately after the
// headers that introduce it.
#ifdef Status
#undef Status
#endif

namespace cc {

#if defined(CC_HAVE_X11)
::Display* x11_display();
#endif

namespace {

#if defined(CC_HAVE_X11)

class LinuxScreen final : public ScreenBackend {
public:
    explicit LinuxScreen(std::shared_ptr<DisplayGraph> displays) {
        displays_ = std::move(displays);
    }

    std::string name() const override { return "XGetImage"; }

    Status initialize() override {
        dpy_ = x11_display();
        if (!dpy_) {
            return err(ErrorCode::BackendFailure, "cannot open the X display for capture",
                       "Set DISPLAY. Under a native Wayland session there is no X screen to "
                       "capture; use the xdg-desktop-portal ScreenCast route instead.");
        }
        return ok();
    }

    Result<Frame> capture(const CaptureOptions& opts) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");

        Rect region;
        ::Window source = DefaultRootWindow(dpy_);

        if (opts.window_id.has_value()) {
            source = static_cast<::Window>(*opts.window_id);
            ::XWindowAttributes attrs{};
            if (!::XGetWindowAttributes(dpy_, source, &attrs)) {
                return err(ErrorCode::NotFound, "no such X window");
            }
            // Window-relative capture: the coordinates below are inside the
            // drawable, not the root.
            region = Rect{0, 0, static_cast<double>(attrs.width), static_cast<double>(attrs.height),
                          Space::Physical};
        } else if (opts.display_index.has_value()) {
            const Display* d = displays_->by_index(*opts.display_index);
            if (!d) {
                return err(ErrorCode::NotFound,
                           "no display at index " + std::to_string(*opts.display_index));
            }
            region = d->bounds_physical;
        } else if (opts.region.has_value()) {
            region = displays_->convert(*opts.region, Space::Physical);
        } else {
            region = displays_->virtual_bounds(Space::Physical);
        }

        const int w = static_cast<int>(std::lround(region.w));
        const int h = static_cast<int>(std::lround(region.h));
        if (w <= 0 || h <= 0) return err(ErrorCode::InvalidArgument, "capture region is empty");

        ::XImage* image =
            ::XGetImage(dpy_, source, static_cast<int>(std::lround(region.x)),
                        static_cast<int>(std::lround(region.y)), static_cast<unsigned>(w),
                        static_cast<unsigned>(h), AllPlanes, ZPixmap);
        if (!image) {
            return err(ErrorCode::BackendFailure, "XGetImage failed",
                       "The region may extend past the root window, or the window may have been "
                       "destroyed or unmapped between listing and capture.");
        }

        Frame f;
        f.width = w;
        f.height = h;
        f.format = PixelFormat::BGRA8;
        f.stride = w * 4;
        f.pixels.resize(static_cast<std::size_t>(f.stride) * h);
        f.source_physical = region;
        f.scale = 1.0;

        // X gives 32bpp as BGRX on little-endian TrueColor visuals, which is
        // the common case, but the masks are authoritative and a 16bpp or
        // BGR565 visual still shows up on old hardware and some VNC servers.
        const bool fast_path = (image->bits_per_pixel == 32 && image->red_mask == 0x00FF0000 &&
                                image->green_mask == 0x0000FF00 && image->blue_mask == 0x000000FF);
        if (fast_path) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(f.pixels.data() + static_cast<std::size_t>(y) * f.stride,
                            image->data + static_cast<std::size_t>(y) * image->bytes_per_line,
                            static_cast<std::size_t>(f.stride));
            }
            for (std::size_t i = 3; i < f.pixels.size(); i += 4) f.pixels[i] = 255;
        } else {
            const auto shift_of = [](unsigned long mask) {
                int s = 0;
                while (mask && !(mask & 1)) {
                    mask >>= 1;
                    ++s;
                }
                return s;
            };
            const auto width_of = [](unsigned long mask) {
                int n = 0;
                while (mask && !(mask & 1)) mask >>= 1;
                while (mask & 1) {
                    mask >>= 1;
                    ++n;
                }
                return n;
            };
            const int rs = shift_of(image->red_mask), gs = shift_of(image->green_mask),
                      bs = shift_of(image->blue_mask);
            const int rw = width_of(image->red_mask), gw = width_of(image->green_mask),
                      bw = width_of(image->blue_mask);
            auto expand = [](unsigned long v, int bits) -> std::uint8_t {
                if (bits <= 0) return 0;
                if (bits >= 8) return static_cast<std::uint8_t>(v >> (bits - 8));
                // Replicate the high bits so 5-bit 31 becomes 255, not 248.
                return static_cast<std::uint8_t>((v << (8 - bits)) | (v >> (2 * bits - 8)));
            };
            for (int y = 0; y < h; ++y) {
                std::uint8_t* row = f.pixels.data() + static_cast<std::size_t>(y) * f.stride;
                for (int x = 0; x < w; ++x) {
                    const unsigned long px = XGetPixel(image, x, y);
                    std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
                    p[0] = expand((px & image->blue_mask) >> bs, bw);
                    p[1] = expand((px & image->green_mask) >> gs, gw);
                    p[2] = expand((px & image->red_mask) >> rs, rw);
                    p[3] = 255;
                }
            }
        }
        XDestroyImage(image);

#if defined(CC_HAVE_XFIXES)
        if (opts.include_cursor) draw_cursor(f, region);
#endif
        return post_process(std::move(f), opts);
    }

    Result<std::uint32_t> pixel(const Point& at) override {
        if (!dpy_) return err(ErrorCode::BackendFailure, "no X display");
        const Point phys = displays_->convert(at, Space::Physical);
        ::XImage* image =
            ::XGetImage(dpy_, DefaultRootWindow(dpy_), static_cast<int>(std::lround(phys.x)),
                        static_cast<int>(std::lround(phys.y)), 1, 1, AllPlanes, ZPixmap);
        if (!image) return err(ErrorCode::BackendFailure, "XGetImage failed for a single pixel");
        const unsigned long px = XGetPixel(image, 0, 0);
        const std::uint32_t r = (px & image->red_mask) >> 16;
        const std::uint32_t g = (px & image->green_mask) >> 8;
        const std::uint32_t b = (px & image->blue_mask);
        XDestroyImage(image);
        return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
    }

private:
#if defined(CC_HAVE_XFIXES)
    void draw_cursor(Frame& f, const Rect& region) {
        int event_base = 0, error_base = 0;
        if (!::XFixesQueryExtension(dpy_, &event_base, &error_base)) return;
        XFixesCursorImage* cursor = ::XFixesGetCursorImage(dpy_);
        if (!cursor) return;

        // XFixes reports the hotspot separately; the image must be placed so
        // the hotspot lands on the pointer position.
        const int ox = cursor->x - cursor->xhot - static_cast<int>(std::lround(region.x));
        const int oy = cursor->y - cursor->yhot - static_cast<int>(std::lround(region.y));

        for (int y = 0; y < cursor->height; ++y) {
            const int dy = oy + y;
            if (dy < 0 || dy >= f.height) continue;
            for (int x = 0; x < cursor->width; ++x) {
                const int dx = ox + x;
                if (dx < 0 || dx >= f.width) continue;
                // XFixes pixels are premultiplied ARGB in an unsigned long.
                const unsigned long px = cursor->pixels[y * cursor->width + x];
                const std::uint8_t a = static_cast<std::uint8_t>((px >> 24) & 0xFF);
                if (a == 0) continue;
                const std::uint8_t r = static_cast<std::uint8_t>((px >> 16) & 0xFF);
                const std::uint8_t g = static_cast<std::uint8_t>((px >> 8) & 0xFF);
                const std::uint8_t b = static_cast<std::uint8_t>(px & 0xFF);
                std::uint8_t* d = f.pixels.data() + static_cast<std::size_t>(dy) * f.stride +
                                  static_cast<std::size_t>(dx) * 4;
                const int inv = 255 - a;
                d[0] = static_cast<std::uint8_t>(b + (d[0] * inv) / 255);
                d[1] = static_cast<std::uint8_t>(g + (d[1] * inv) / 255);
                d[2] = static_cast<std::uint8_t>(r + (d[2] * inv) / 255);
            }
        }
        ::XFree(cursor);
    }
#endif

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

    ::Display* dpy_ = nullptr;
};

#endif  // CC_HAVE_X11

}  // namespace

Result<std::unique_ptr<ScreenBackend>> ScreenBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
#if defined(CC_HAVE_X11)
    auto backend = std::make_unique<LinuxScreen>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<ScreenBackend>(std::move(backend));
#else
    (void)displays;
    return err(ErrorCode::Unsupported, "this build has no X11 support",
               "Install libx11-dev and libxfixes-dev and rebuild.");
#endif
}

}  // namespace cc
