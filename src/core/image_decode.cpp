// SPDX-License-Identifier: MIT
//
// PNG decoding, for the images mobile bridges hand back.
//
// Only what those bridges actually emit is supported: 8-bit RGB/RGBA/grey,
// non-interlaced. `simctl io screenshot` and `adb exec-out screencap -p` both
// produce exactly that. Anything else fails with a clear message rather than
// silently returning garbage pixels.

#include <cstring>

#include "cc/screen.hpp"
#include "core/deflate.hpp"

namespace cc {
namespace {

std::uint32_t read_be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    if (pb <= pc) return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

}  // namespace

Result<Frame> decode_png(const std::uint8_t* data, std::size_t len) {
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (len < 8 || std::memcmp(data, kSig, 8) != 0) {
        return err(ErrorCode::InvalidArgument, "not a PNG");
    }

    std::int32_t width = 0, height = 0;
    int bit_depth = 0, colour_type = 0, interlace = 0;
    std::vector<std::uint8_t> idat;
    bool saw_ihdr = false;

    std::size_t pos = 8;
    while (pos + 8 <= len) {
        const std::uint32_t clen = read_be32(data + pos);
        const char* tag = reinterpret_cast<const char*>(data + pos + 4);
        const std::size_t body = pos + 8;
        if (body + clen + 4 > len) break;

        if (std::memcmp(tag, "IHDR", 4) == 0 && clen >= 13) {
            width = static_cast<std::int32_t>(read_be32(data + body));
            height = static_cast<std::int32_t>(read_be32(data + body + 4));
            bit_depth = data[body + 8];
            colour_type = data[body + 9];
            interlace = data[body + 12];
            saw_ihdr = true;
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data + body, data + body + clen);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        pos = body + clen + 4;  // skip body and CRC
    }

    if (!saw_ihdr || width <= 0 || height <= 0) {
        return err(ErrorCode::InvalidArgument, "PNG has no usable IHDR");
    }
    if (bit_depth != 8) {
        return err(ErrorCode::Unsupported,
                   "only 8-bit PNGs are supported (got " + std::to_string(bit_depth) + "-bit)");
    }
    if (interlace != 0) {
        return err(ErrorCode::Unsupported, "interlaced PNGs are not supported");
    }
    int channels = 0;
    switch (colour_type) {
        case 0: channels = 1; break;  // greyscale
        case 2: channels = 3; break;  // RGB
        case 4: channels = 2; break;  // grey + alpha
        case 6: channels = 4; break;  // RGBA
        default:
            return err(ErrorCode::Unsupported, "palette PNGs are not supported (colour type " +
                                                   std::to_string(colour_type) + ")");
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width) * channels;
    const std::size_t expected = (row_bytes + 1) * static_cast<std::size_t>(height);

    std::vector<std::uint8_t> raw;
    if (!compress::zlib_uncompress(idat.data(), idat.size(), raw, expected) ||
        raw.size() < expected) {
        return err(ErrorCode::IoError, "PNG data stream is corrupt or truncated");
    }

    Frame f;
    f.width = width;
    f.height = height;
    f.format = PixelFormat::RGBA8;
    f.stride = width * 4;
    f.pixels.assign(static_cast<std::size_t>(f.stride) * height, 255);
    f.source_physical = Rect{0, 0, double(width), double(height), Space::Physical};
    f.scale = 1.0;

    std::vector<std::uint8_t> prev(row_bytes, 0);
    std::vector<std::uint8_t> cur(row_bytes, 0);

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = raw.data() + static_cast<std::size_t>(y) * (row_bytes + 1);
        const int filter = src[0];
        std::memcpy(cur.data(), src + 1, row_bytes);

        for (std::size_t i = 0; i < row_bytes; ++i) {
            const int A = (i >= static_cast<std::size_t>(channels)) ? cur[i - channels] : 0;
            const int B = prev[i];
            const int C = (i >= static_cast<std::size_t>(channels)) ? prev[i - channels] : 0;
            switch (filter) {
                case 0: break;
                case 1: cur[i] = static_cast<std::uint8_t>(cur[i] + A); break;
                case 2: cur[i] = static_cast<std::uint8_t>(cur[i] + B); break;
                case 3: cur[i] = static_cast<std::uint8_t>(cur[i] + ((A + B) >> 1)); break;
                case 4: cur[i] = static_cast<std::uint8_t>(cur[i] + paeth(A, B, C)); break;
                default:
                    return err(ErrorCode::IoError, "PNG row " + std::to_string(y) +
                                                       " uses filter " + std::to_string(filter));
            }
        }

        std::uint8_t* dst = f.pixels.data() + static_cast<std::size_t>(y) * f.stride;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* s = cur.data() + static_cast<std::size_t>(x) * channels;
            std::uint8_t* d = dst + static_cast<std::size_t>(x) * 4;
            switch (channels) {
                case 1:
                    d[0] = d[1] = d[2] = s[0];
                    d[3] = 255;
                    break;
                case 2:
                    d[0] = d[1] = d[2] = s[0];
                    d[3] = s[1];
                    break;
                case 3:
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = 255;
                    break;
                default:
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = s[3];
                    break;
            }
        }
        prev.swap(cur);
    }
    return f;
}

}  // namespace cc

namespace cc::devices {

Result<Frame> decode_image(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P') {
        return decode_png(bytes.data(), bytes.size());
    }
    if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        return err(ErrorCode::Unsupported,
                   "the device returned a JPEG, which this build cannot decode",
                   "Both simctl and adb produce PNG by default; a JPEG usually means a "
                   "custom capture command was configured.");
    }
    return err(ErrorCode::InvalidArgument, "unrecognised image format from the device");
}

}  // namespace cc::devices
