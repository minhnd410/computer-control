// SPDX-License-Identifier: MIT
#pragma once

// zlib-compatible compression.
//
// The build links system zlib when CMake finds it (better ratios, SIMD CRC on
// most distributions). When it does not — which in practice means a minimal
// Windows or musl toolchain — this fixed-Huffman LZ77 encoder takes over. It
// is ~3-5x on screenshot data, which is the difference between a 24 MB and a
// 5 MB 4K PNG, and it keeps the library dependency-free by default.

#include <cstdint>
#include <vector>

namespace cc::compress {

std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed = 0);
std::uint32_t adler32(const std::uint8_t* data, std::size_t len);

// Raw DEFLATE stream (no zlib header).
std::vector<std::uint8_t> deflate_raw(const std::uint8_t* data, std::size_t len, int level);

// zlib container: 2-byte header + deflate + adler32. This is what PNG IDAT
// chunks carry.
std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t len, int level);

// True when the system zlib is in use.
bool using_system_zlib();

}  // namespace cc::compress

namespace cc::compress {
// Inflate a zlib stream (2-byte header + deflate + adler32). Needed to decode
// the PNGs that simctl and adb hand back for device screenshots.
// `expected_size` is a hint; the buffer grows if the stream is larger.
bool zlib_uncompress(const std::uint8_t* data, std::size_t len, std::vector<std::uint8_t>& out,
                     std::size_t expected_size);
}  // namespace cc::compress
