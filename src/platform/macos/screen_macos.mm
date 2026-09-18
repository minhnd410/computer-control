// SPDX-License-Identifier: MIT
//
// macOS screen capture, on ScreenCaptureKit.
//
// There is no choice here any more: CGDisplayCreateImage and
// CGWindowListCreateImage are not merely deprecated but *obsoleted* in the
// macOS 15 SDK — they fail to compile, not just to run. Anything that still
// calls them was built against an older SDK. ScreenCaptureKit (macOS 12.3+,
// with SCScreenshotManager from 14.0) is the supported path.
//
// SCShareableContent and SCScreenshotManager are both async, so each call
// blocks on a semaphore. They must not be invoked from the main thread while
// it is also pumping the run loop that delivers the completion, which is why
// the MCP server keeps its request handling off the main thread.

#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cc/screen.hpp"

namespace cc {
namespace {

constexpr int64_t kTimeoutNs = 8LL * NSEC_PER_SEC;

SCShareableContent* shareable_content(NSError** out_error) {
    __block SCShareableContent* result = nil;
    __block NSError* error = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* err) {
          result = content;
          error = err;
          dispatch_semaphore_signal(sem);
        }];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, kTimeoutNs)) != 0) {
        if (out_error) *out_error = nil;
        return nil;
    }
    if (out_error) *out_error = error;
    return result;
}

Result<Frame> cgimage_to_frame(CGImageRef image, const Rect& source_physical, double scale) {
    if (!image) return err(ErrorCode::BackendFailure, "capture returned no image");

    const std::size_t w = CGImageGetWidth(image);
    const std::size_t h = CGImageGetHeight(image);
    if (w == 0 || h == 0) return err(ErrorCode::BackendFailure, "capture returned an empty image");

    Frame f;
    f.width = static_cast<std::int32_t>(w);
    f.height = static_cast<std::int32_t>(h);
    f.format = PixelFormat::BGRA8;
    f.stride = static_cast<std::int32_t>(w * 4);
    f.pixels.assign(static_cast<std::size_t>(f.stride) * h, 0);
    f.source_physical = source_physical;
    f.scale = scale;

    // Draw into our own tightly-packed buffer rather than reading the image's
    // data provider: the provider's layout depends on the capture path (row
    // padding, YCbCr, IOSurface backing) and normalising here is both simpler
    // and correct for every case.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx =
        CGBitmapContextCreate(f.pixels.data(), w, h, 8, static_cast<std::size_t>(f.stride), cs,
                              static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) |
                                  static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(cs);
    if (!ctx) return err(ErrorCode::BackendFailure, "CGBitmapContextCreate failed");

    CGContextDrawImage(ctx, CGRectMake(0, 0, static_cast<CGFloat>(w), static_cast<CGFloat>(h)),
                       image);
    CGContextRelease(ctx);
    return f;
}

Result<Frame> capture_with_filter(SCContentFilter* filter, std::size_t px_w, std::size_t px_h,
                                  bool include_cursor, const Rect& source_physical, double scale) {
    SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
    cfg.width = px_w;
    cfg.height = px_h;
    cfg.showsCursor = include_cursor;
    cfg.capturesAudio = NO;
    cfg.pixelFormat = kCVPixelFormatType_32BGRA;
    cfg.scalesToFit = NO;
    if (@available(macOS 14.0, *)) {
        // Without this the capture is composited onto black, which makes
        // rounded window corners and shadows read as solid dark pixels.
        cfg.backgroundColor = CGColorGetConstantColor(kCGColorClear);
    }

    __block CGImageRef image = nullptr;
    __block NSError* error = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCScreenshotManager captureImageWithFilter:filter
                                  configuration:cfg
                              completionHandler:^(CGImageRef img, NSError* e) {
                                if (img)
                                    image =
                                        static_cast<CGImageRef>(const_cast<void*>(CFRetain(img)));
                                error = e;
                                dispatch_semaphore_signal(sem);
                              }];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, kTimeoutNs)) != 0) {
        return err(ErrorCode::Timeout, "ScreenCaptureKit did not return within 8s",
                   "This usually means the capture completion is blocked behind the main run "
                   "loop. Run captures off the main thread.");
    }
    if (!image) {
        std::string detail = error ? std::string(error.localizedDescription.UTF8String) : "unknown";
        return err(ErrorCode::PermissionDenied, "ScreenCaptureKit capture failed: " + detail,
                   "System Settings > Privacy & Security > Screen & System Audio Recording, "
                   "then enable the binary or the terminal running it and restart the process.");
    }

    auto frame = cgimage_to_frame(image, source_physical, scale);
    CFRelease(image);
    return frame;
}

class MacScreen final : public ScreenBackend {
public:
    explicit MacScreen(std::shared_ptr<DisplayGraph> displays) { displays_ = std::move(displays); }

    std::string name() const override { return "ScreenCaptureKit"; }

    Status initialize() override {
        NSError* error = nil;
        SCShareableContent* content = shareable_content(&error);
        if (!content) {
            return err(ErrorCode::PermissionDenied,
                       "ScreenCaptureKit returned no shareable content",
                       "Grant Screen & System Audio Recording in System Settings > Privacy & "
                       "Security, then restart this process. macOS caches the decision at "
                       "launch, so toggling it while running has no effect.");
        }
        return ok();
    }

    Result<Frame> capture(const CaptureOptions& opts) override {
        @autoreleasepool {
            NSError* error = nil;
            SCShareableContent* content = shareable_content(&error);
            if (!content) {
                return err(ErrorCode::PermissionDenied, "could not enumerate shareable content",
                           "Screen Recording permission is required. See "
                           "`computer-control-mcp --doctor`.");
            }

            if (opts.window_id.has_value()) return capture_window(content, opts);
            return capture_display(content, opts);
        }
    }

    Result<std::uint32_t> pixel(const Point& at) override {
        // Capture a 1x1 region rather than the whole screen. Still a full SCK
        // round trip (~15ms), but it avoids allocating and encoding megabytes
        // for a single colour probe.
        CaptureOptions o;
        const Point phys = displays_->convert(at, Space::Physical);
        o.region = Rect{phys.x, phys.y, 1, 1, Space::Physical};
        auto f = capture(o);
        if (!f) return f.error();
        if (f.value().empty()) return err(ErrorCode::BackendFailure, "empty pixel capture");
        const std::uint8_t* p = f.value().pixels.data();
        // BGRA -> RGBA
        return (static_cast<std::uint32_t>(p[2]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[0]) << 8) | p[3];
    }

private:
    Result<Frame> capture_window(SCShareableContent* content, const CaptureOptions& opts) {
        SCWindow* target = nil;
        for (SCWindow* w in content.windows) {
            if (static_cast<std::uint64_t>(w.windowID) == *opts.window_id) {
                target = w;
                break;
            }
        }
        if (!target) {
            return err(ErrorCode::NotFound,
                       "window " + std::to_string(*opts.window_id) + " is not capturable",
                       "It may have closed, be minimised, or live on another Space. List "
                       "windows again to get a current id.");
        }

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
        const CGRect fr = target.frame;
        // SCWindow.frame is in points. Capture at the owning display's scale so
        // text stays crisp instead of being captured at 1x and upscaled.
        const Display* d = displays_->containing(Point{fr.origin.x, fr.origin.y, Space::Logical});
        const double scale = d ? d->scale : 1.0;
        const auto px_w = static_cast<std::size_t>(std::lround(fr.size.width * scale));
        const auto px_h = static_cast<std::size_t>(std::lround(fr.size.height * scale));
        if (px_w == 0 || px_h == 0) {
            return err(ErrorCode::BackendFailure, "window has zero size");
        }

        const Rect src{fr.origin.x * scale, fr.origin.y * scale, static_cast<double>(px_w),
                       static_cast<double>(px_h), Space::Physical};
        auto frame = capture_with_filter(filter, px_w, px_h, opts.include_cursor, src, 1.0);
        if (!frame) return frame;
        return post_process(std::move(frame.value()), opts);
    }

    Result<Frame> capture_display(SCShareableContent* content, const CaptureOptions& opts) {
        const Display* want = opts.display_index.has_value()
                                  ? displays_->by_index(*opts.display_index)
                                  : displays_->primary();
        if (!want) {
            return err(ErrorCode::NotFound, "display index " +
                                                std::to_string(opts.display_index.value_or(-1)) +
                                                " does not exist");
        }

        SCDisplay* target = nil;
        for (SCDisplay* d in content.displays) {
            if (static_cast<std::uint64_t>(d.displayID) == want->id) {
                target = d;
                break;
            }
        }
        if (!target) target = content.displays.firstObject;
        if (!target) return err(ErrorCode::NotFound, "no capturable display");

        NSMutableArray<SCWindow*>* excluded = [NSMutableArray array];
        for (SCWindow* w in content.windows) {
            for (std::uint64_t id : opts.exclude_window_ids) {
                if (static_cast<std::uint64_t>(w.windowID) == id) {
                    [excluded addObject:w];
                    break;
                }
            }
        }
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:target
                                                          excludingWindows:excluded];

        // SCDisplay.width/height are points. Requesting the display's physical
        // pixel dimensions is what makes a Retina capture actually 2x rather
        // than a 1x image stretched to fit.
        auto px_w = static_cast<std::size_t>(std::lround(want->bounds_physical.w));
        auto px_h = static_cast<std::size_t>(std::lround(want->bounds_physical.h));
        if (px_w == 0 || px_h == 0) {
            px_w = static_cast<std::size_t>(target.width);
            px_h = static_cast<std::size_t>(target.height);
        }

        auto frame = capture_with_filter(filter, px_w, px_h, opts.include_cursor,
                                         want->bounds_physical, 1.0);
        if (!frame) return frame;
        return post_process(std::move(frame.value()), opts);
    }

    // Region crop and downscale happen after capture rather than by asking SCK
    // for a smaller source rect: sourceRect is applied before scaling and
    // interacts badly with multi-display setups, and cropping a full-resolution
    // capture costs a memcpy we were paying for anyway.
    Result<Frame> post_process(Frame f, const CaptureOptions& opts) {
        if (opts.region.has_value()) {
            const Rect phys = displays_->convert(*opts.region, Space::Physical);
            const Rect in_frame{(phys.x - f.source_physical.x) * f.scale,
                                (phys.y - f.source_physical.y) * f.scale, phys.w * f.scale,
                                phys.h * f.scale, Space::Image};
            auto c = crop(f, in_frame);
            if (!c) return c;
            f = std::move(c.value());
        }

        double factor = (opts.scale > 0 && opts.scale != 1.0) ? opts.scale : 1.0;
        if (opts.max_dimension > 0) {
            const int longest = std::max(f.width, f.height);
            if (longest * factor > opts.max_dimension) {
                factor = static_cast<double>(opts.max_dimension) / longest;
            }
        }
        if (factor != 1.0) {
            const auto w = std::max(1, static_cast<int>(std::lround(f.width * factor)));
            const auto h = std::max(1, static_cast<int>(std::lround(f.height * factor)));
            auto r = resize(f, w, h);
            if (!r) return r;
            f = std::move(r.value());
        }

        // Publish the transform so Space::Image coordinates from this frame
        // convert back to real screen positions. Without this step every
        // click derived from a downscaled screenshot lands at the wrong place.
        displays_->set_image_transform(f.transform());
        return f;
    }
};

}  // namespace

Result<std::unique_ptr<ScreenBackend>> ScreenBackend::create(
    std::shared_ptr<DisplayGraph> displays) {
    auto backend = std::make_unique<MacScreen>(std::move(displays));
    if (auto st = backend->initialize(); !st) return st.error();
    return std::unique_ptr<ScreenBackend>(std::move(backend));
}

}  // namespace cc
