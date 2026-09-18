// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <cstring>

#include "cc/screen.hpp"
#include "core/deflate.hpp"
#include "test_framework.hpp"

namespace cc {
Result<Frame> decode_png(const std::uint8_t* data, std::size_t len);
}

using namespace cc;

namespace {

Frame make_test_frame(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.format = PixelFormat::BGRA8;
    f.stride = w * 4;
    f.pixels.resize(static_cast<std::size_t>(f.stride) * h);
    f.source_physical = Rect{0, 0, double(w), double(h), Space::Physical};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = f.pixels.data() + static_cast<std::size_t>(y) * f.stride + x * 4;
            p[0] = static_cast<std::uint8_t>(x * 3 % 256);    // B
            p[1] = static_cast<std::uint8_t>(y * 5 % 256);    // G
            p[2] = static_cast<std::uint8_t>((x ^ y) % 256);  // R
            p[3] = 255;
        }
    }
    return f;
}

}  // namespace

TEST(deflate_roundtrips_through_inflate) {
    std::vector<std::uint8_t> data;
    // Mixed content: runs compress well, noise does not, and both must
    // survive.
    for (int i = 0; i < 40000; ++i) {
        data.push_back(static_cast<std::uint8_t>((i / 100) % 7 == 0 ? 0xAB : (i * 31) % 251));
    }
    auto packed = compress::zlib_compress(data.data(), data.size(), 6);
    CHECK(!packed.empty());

    std::vector<std::uint8_t> out;
    CHECK(compress::zlib_uncompress(packed.data(), packed.size(), out, data.size()));
    CHECK_EQ(out.size(), data.size());
    CHECK(std::memcmp(out.data(), data.data(), data.size()) == 0);
}

TEST(deflate_handles_empty_and_tiny_inputs) {
    for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3)}) {
        std::vector<std::uint8_t> data(n, 0x5A);
        auto packed = compress::zlib_compress(data.data(), data.size(), 6);
        std::vector<std::uint8_t> out;
        CHECK(compress::zlib_uncompress(packed.data(), packed.size(), out, n));
        CHECK_EQ(out.size(), n);
    }
}

TEST(deflate_actually_compresses_repetitive_data) {
    std::vector<std::uint8_t> data(100000, 0x42);
    auto packed = compress::zlib_compress(data.data(), data.size(), 6);
    // Highly repetitive input must shrink dramatically; if it does not, the
    // match finder is not working and screenshots would be enormous.
    CHECK(packed.size() < data.size() / 20);
}

TEST(crc32_matches_known_vector) {
    const char* s = "123456789";
    const auto crc = compress::crc32(reinterpret_cast<const std::uint8_t*>(s), 9);
    CHECK_EQ(crc, 0xCBF43926u);
}

TEST(adler32_matches_known_vector) {
    const char* s = "Wikipedia";
    CHECK_EQ(compress::adler32(reinterpret_cast<const std::uint8_t*>(s), 9), 0x11E60398u);
}

TEST(png_encode_decode_roundtrip_is_pixel_exact) {
    const Frame src = make_test_frame(97, 53);  // deliberately not a round size
    EncodeOptions eo;
    eo.format = ImageFormat::PNG;
    auto bytes = encode(src, eo);
    CHECK(bytes.ok());
    if (!bytes) return;

    // Valid PNG signature.
    CHECK(bytes.value().size() > 8);
    CHECK_EQ(bytes.value()[0], std::uint8_t(0x89));
    CHECK_EQ(bytes.value()[1], std::uint8_t('P'));

    auto decoded = decode_png(bytes.value().data(), bytes.value().size());
    CHECK(decoded.ok());
    if (!decoded) return;
    CHECK_EQ(decoded.value().width, src.width);
    CHECK_EQ(decoded.value().height, src.height);

    // The decoder normalises to RGBA; the encoder read BGRA.
    int mismatches = 0;
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            const std::uint8_t* s = src.pixels.data() + y * src.stride + x * 4;
            const std::uint8_t* d =
                decoded.value().pixels.data() + y * decoded.value().stride + x * 4;
            if (d[0] != s[2] || d[1] != s[1] || d[2] != s[0]) ++mismatches;
        }
    }
    CHECK_EQ(mismatches, 0);
}

TEST(jpeg_encode_produces_a_valid_stream) {
    const Frame src = make_test_frame(64, 64);
    EncodeOptions eo;
    eo.format = ImageFormat::JPEG;
    eo.quality = 80;
    auto bytes = encode(src, eo);
    CHECK(bytes.ok());
    if (!bytes) return;
    CHECK(bytes.value().size() > 100);
    // SOI ... EOI
    CHECK_EQ(bytes.value()[0], std::uint8_t(0xFF));
    CHECK_EQ(bytes.value()[1], std::uint8_t(0xD8));
    CHECK_EQ(bytes.value()[bytes.value().size() - 2], std::uint8_t(0xFF));
    CHECK_EQ(bytes.value()[bytes.value().size() - 1], std::uint8_t(0xD9));
}

TEST(jpeg_is_smaller_than_png_for_photographic_content) {
    // make_test_frame() is a synthetic XOR pattern, which PNG's filters model
    // almost perfectly - PNG legitimately wins on it. The claim being tested
    // is about photographic content, so the input has to be photographic:
    // smooth shading plus sensor-like noise.
    Frame src = make_test_frame(256, 256);
    for (int y = 0; y < src.height; ++y) {
        for (int x = 0; x < src.width; ++x) {
            std::uint8_t* p = src.pixels.data() + static_cast<std::size_t>(y) * src.stride + x * 4;
            const double shade = 128 + 90 * std::sin(x * 0.021) * std::cos(y * 0.017);
            const int noise = ((x * 7919 + y * 104729) % 23) - 11;
            const int v = static_cast<int>(shade) + noise;
            p[0] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
            p[1] = static_cast<std::uint8_t>(std::clamp(v + 20, 0, 255));
            p[2] = static_cast<std::uint8_t>(std::clamp(v - 15, 0, 255));
            p[3] = 255;
        }
    }

    EncodeOptions png;
    png.format = ImageFormat::PNG;
    EncodeOptions jpg;
    jpg.format = ImageFormat::JPEG;
    jpg.quality = 70;
    auto a = encode(src, png);
    auto b = encode(src, jpg);
    CHECK(a.ok() && b.ok());
    if (a && b) CHECK(b.value().size() < a.value().size());
}

TEST(jpeg_quality_controls_size) {
    const Frame src = make_test_frame(128, 128);
    EncodeOptions low;
    low.format = ImageFormat::JPEG;
    low.quality = 20;
    EncodeOptions high = low;
    high.quality = 95;
    auto a = encode(src, low);
    auto b = encode(src, high);
    CHECK(a.ok() && b.ok());
    if (a && b) CHECK(a.value().size() < b.value().size());
}

TEST(resize_downscale_uses_box_filter) {
    const Frame src = make_test_frame(200, 100);
    auto small = resize(src, 50, 25);
    CHECK(small.ok());
    if (!small) return;
    CHECK_EQ(small.value().width, 50);
    CHECK_EQ(small.value().height, 25);
    // The image-space scale must track the resize, or coordinates read off the
    // downscaled image map back to the wrong screen position.
    CHECK_NEAR(small.value().scale, 0.25, 1e-9);
}

TEST(resize_upscale_works) {
    const Frame src = make_test_frame(10, 10);
    auto big = resize(src, 40, 40);
    CHECK(big.ok());
    if (big) CHECK_EQ(big.value().width, 40);
}

TEST(crop_clamps_to_the_frame) {
    const Frame src = make_test_frame(100, 100);
    auto c = crop(src, Rect{50, 50, 500, 500, Space::Image});
    CHECK(c.ok());
    if (c) {
        CHECK_EQ(c.value().width, 50);
        CHECK_EQ(c.value().height, 50);
    }
    // Fully outside is an error, not a zero-size frame the caller then
    // encodes into nothing.
    auto bad = crop(src, Rect{500, 500, 10, 10, Space::Image});
    CHECK(!bad.ok());
}

TEST(base64_matches_known_vectors) {
    auto enc = [](const char* s) {
        return base64(std::vector<std::uint8_t>(s, s + std::strlen(s)));
    };
    CHECK_EQ(enc(""), std::string(""));
    CHECK_EQ(enc("f"), std::string("Zg=="));
    CHECK_EQ(enc("fo"), std::string("Zm8="));
    CHECK_EQ(enc("foo"), std::string("Zm9v"));
    CHECK_EQ(enc("foob"), std::string("Zm9vYg=="));
    CHECK_EQ(enc("fooba"), std::string("Zm9vYmE="));
    CHECK_EQ(enc("foobar"), std::string("Zm9vYmFy"));
}

TEST(webp_reports_unsupported_clearly) {
    const Frame src = make_test_frame(8, 8);
    EncodeOptions eo;
    eo.format = ImageFormat::WEBP;
    auto r = encode(src, eo);
    CHECK(!r.ok());
    CHECK(r.error().code == ErrorCode::Unsupported);
    CHECK(!r.error().remedy.empty());
}
