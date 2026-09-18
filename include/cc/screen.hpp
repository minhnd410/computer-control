// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cc/display.hpp"
#include "cc/types.hpp"

namespace cc {

enum class PixelFormat : std::uint8_t { BGRA8 = 0, RGBA8 = 1, RGB8 = 2 };
enum class ImageFormat : std::uint8_t { PNG = 0, JPEG = 1, WEBP = 2, RAW = 3 };

struct Frame {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;  // bytes per row, may exceed width * bpp
    PixelFormat format = PixelFormat::BGRA8;
    std::vector<std::uint8_t> pixels;
    // Region of the physical virtual desktop this frame came from, and the
    // factor applied on the way out. Together these define Space::Image.
    Rect source_physical;
    double scale = 1.0;

    bool empty() const noexcept { return width <= 0 || height <= 0 || pixels.empty(); }
    DisplayGraph::ImageTransform transform() const;
};

struct CaptureOptions {
    // Exactly one of these selects the source; leaving all unset captures the
    // whole virtual desktop.
    std::optional<std::int32_t> display_index;
    std::optional<std::uint64_t> window_id;
    std::optional<Rect> region;

    // Downscale so neither dimension exceeds this. Screenshots of a 6K desktop
    // routinely blow past a 1 MB tool-result budget; 1600 keeps a full desktop
    // under ~400 KB as PNG while staying readable.
    std::int32_t max_dimension = 0;  // 0 = no limit
    double scale = 1.0;              // additional explicit scale, applied first

    bool include_cursor = true;
    // Exclude windows not in the allowlist at the compositor level where the
    // platform supports it (macOS SCStream, Windows WDA_EXCLUDEFROMCAPTURE).
    std::vector<std::uint64_t> exclude_window_ids;
};

struct EncodeOptions {
    ImageFormat format = ImageFormat::PNG;
    int quality = 80;   // JPEG/WEBP
    int png_level = 6;  // 0..9
};

class ScreenBackend {
public:
    virtual ~ScreenBackend() = default;

    static Result<std::unique_ptr<ScreenBackend>> create(std::shared_ptr<DisplayGraph> displays);

    virtual std::string name() const = 0;
    virtual Status initialize() = 0;

    virtual Result<Frame> capture(const CaptureOptions& opts) = 0;

    // Colour of a single pixel, without paying for a full-desktop capture.
    virtual Result<std::uint32_t> pixel(const Point& at) = 0;

protected:
    std::shared_ptr<DisplayGraph> displays_;
};

// Format conversion and encoding. Platform-independent, so it lives outside
// the backend.
Result<std::vector<std::uint8_t>> encode(const Frame& f, const EncodeOptions& opts);
Result<Frame> resize(const Frame& src, std::int32_t width, std::int32_t height);
Result<Frame> crop(const Frame& src, const Rect& region_in_frame);
std::string base64(const std::vector<std::uint8_t>& bytes);

}  // namespace cc
