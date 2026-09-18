// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cc/display.hpp"

#if defined(CC_HAVE_X11)
#include <X11/Xlib.h>
#if defined(CC_HAVE_XRANDR)
#include <X11/extensions/Xrandr.h>
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
// Shared across the X11 backends. One connection per process: XOpenDisplay is
// expensive and Xlib is only thread-safe after XInitThreads, which is called
// once here.
//
// The leading :: is load-bearing: X11's Display is a global typedef and this
// namespace has its own cc::Display struct, so a bare Display* here compiles
// as the wrong type.
::Display* x11_display();
#endif

namespace {

// X11 has no per-monitor scale factor. Toolkits derive one from Xft.dpi, from
// GDK_SCALE/QT_SCALE_FACTOR, or from the physical size RandR reports. Guessing
// wrong means every coordinate is off by that factor, so the order of
// precedence here matches what GTK and Qt actually do, and the result is
// reported rather than silently applied.
double environment_scale() {
    for (const char* var : {"GDK_SCALE", "QT_SCALE_FACTOR", "CC_SCALE"}) {
        if (const char* v = std::getenv(var)) {
            const double d = std::strtod(v, nullptr);
            if (d > 0.1 && d < 10.0) return d;
        }
    }
    return 0.0;
}

#if defined(CC_HAVE_X11)
double xft_dpi_scale(::Display* dpy) {
    // Xft.dpi in the X resource database is what most desktops set when the
    // user picks a scaling factor.
    if (char* resources = ::XResourceManagerString(dpy)) {
        const char* key = "Xft.dpi:";
        if (const char* found = std::strstr(resources, key)) {
            const double dpi = std::strtod(found + std::strlen(key), nullptr);
            if (dpi > 40 && dpi < 800) return dpi / 96.0;
        }
    }
    return 0.0;
}
#endif

}  // namespace

#if defined(CC_HAVE_X11)

Status platform_enumerate_displays(std::vector<Display>& out) {
    out.clear();
    ::Display* dpy = x11_display();
    if (!dpy) {
        return err(ErrorCode::BackendFailure, "cannot open the X display",
                   "Set DISPLAY (for example :0) and make sure the process can reach the X "
                   "server. Under Wayland, run with XWAYLAND or use a portal-based backend; "
                   "see the README's Linux section.");
    }

    const int screen = DefaultScreen(dpy);
    ::Window root = RootWindow(dpy, screen);

    double scale = environment_scale();
    if (scale <= 0) scale = xft_dpi_scale(dpy);
    if (scale <= 0) scale = 1.0;

#if defined(CC_HAVE_XRANDR)
    if (XRRScreenResources* res = ::XRRGetScreenResourcesCurrent(dpy, root)) {
        RROutput primary = ::XRRGetOutputPrimary(dpy, root);
        for (int i = 0; i < res->noutput; ++i) {
            XRROutputInfo* info = ::XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (!info) continue;
            if (info->connection != RR_Connected || info->crtc == 0) {
                ::XRRFreeOutputInfo(info);
                continue;
            }
            XRRCrtcInfo* crtc = ::XRRGetCrtcInfo(dpy, res, info->crtc);
            if (!crtc) {
                ::XRRFreeOutputInfo(info);
                continue;
            }

            Display d;
            d.id = static_cast<std::uint64_t>(res->outputs[i]);
            d.name = info->name ? info->name : "output";
            d.primary = (res->outputs[i] == primary);

            // RandR reports device pixels.
            d.bounds_physical = Rect{static_cast<double>(crtc->x), static_cast<double>(crtc->y),
                                     static_cast<double>(crtc->width),
                                     static_cast<double>(crtc->height), Space::Physical};
            d.bounds_logical = Rect{crtc->x / scale, crtc->y / scale, crtc->width / scale,
                                    crtc->height / scale, Space::Logical};
            d.work_area_logical = d.bounds_logical;  // panels are not queryable portably
            d.scale = scale;
            d.dpi = 96.0 * scale;

            // Physical size lets us report a true DPI when the compositor has
            // not set a scale at all.
            if (info->mm_width > 0 && scale == 1.0) {
                const double real_dpi = crtc->width * 25.4 / info->mm_width;
                if (real_dpi > 40 && real_dpi < 800) d.dpi = real_dpi;
            }

            switch (crtc->rotation & 0xF) {
                case RR_Rotate_90: d.orientation = Orientation::Portrait; break;
                case RR_Rotate_180: d.orientation = Orientation::LandscapeFlipped; break;
                case RR_Rotate_270: d.orientation = Orientation::PortraitFlipped; break;
                default: d.orientation = Orientation::Landscape; break;
            }
            d.refresh_hz = 60.0;
            for (int m = 0; m < res->nmode; ++m) {
                if (res->modes[m].id == crtc->mode && res->modes[m].hTotal &&
                    res->modes[m].vTotal) {
                    d.refresh_hz =
                        static_cast<double>(res->modes[m].dotClock) /
                        (static_cast<double>(res->modes[m].hTotal) * res->modes[m].vTotal);
                    break;
                }
            }

            out.push_back(std::move(d));
            ::XRRFreeCrtcInfo(crtc);
            ::XRRFreeOutputInfo(info);
        }
        ::XRRFreeScreenResources(res);
    }
#endif

    if (out.empty()) {
        // No RandR, or a server that reports no outputs: fall back to the
        // single X screen. Correct for Xvfb and most CI containers.
        Display d;
        d.id = 0;
        d.name = "X11 screen " + std::to_string(screen);
        d.primary = true;
        const double w = DisplayWidth(dpy, screen);
        const double h = DisplayHeight(dpy, screen);
        d.bounds_physical = Rect{0, 0, w, h, Space::Physical};
        d.bounds_logical = Rect{0, 0, w / scale, h / scale, Space::Logical};
        d.work_area_logical = d.bounds_logical;
        d.scale = scale;
        d.dpi = 96.0 * scale;
        d.refresh_hz = 60.0;
        out.push_back(std::move(d));
    }
    return ok();
}

#else  // no X11 at build time

Status platform_enumerate_displays(std::vector<Display>& out) {
    out.clear();
    return err(ErrorCode::Unsupported, "this build has no X11 support",
               "Install libx11-dev, libxtst-dev, libxrandr-dev and libxfixes-dev, then "
               "rebuild. See the README's Linux section.");
}

#endif

}  // namespace cc
