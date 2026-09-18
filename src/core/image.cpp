// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <cstring>

#include "cc/screen.hpp"
#include "core/deflate.hpp"

namespace cc {
namespace {

int bytes_per_pixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::BGRA8:
        case PixelFormat::RGBA8: return 4;
        case PixelFormat::RGB8: return 3;
    }
    return 4;
}

void read_rgb(const Frame& f, int x, int y, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b,
              std::uint8_t& a) {
    const int bpp = bytes_per_pixel(f.format);
    const std::uint8_t* p = f.pixels.data() + static_cast<std::size_t>(y) * f.stride +
                            static_cast<std::size_t>(x) * bpp;
    switch (f.format) {
        case PixelFormat::BGRA8:
            b = p[0];
            g = p[1];
            r = p[2];
            a = p[3];
            break;
        case PixelFormat::RGBA8:
            r = p[0];
            g = p[1];
            b = p[2];
            a = p[3];
            break;
        case PixelFormat::RGB8:
            r = p[0];
            g = p[1];
            b = p[2];
            a = 255;
            break;
    }
}

void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void png_chunk(std::vector<std::uint8_t>& out, const char tag[4], const std::uint8_t* data,
               std::size_t len) {
    be32(out, static_cast<std::uint32_t>(len));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (len) out.insert(out.end(), data, data + len);
    const std::uint32_t crc = compress::crc32(out.data() + crc_start, out.size() - crc_start);
    be32(out, crc);
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    if (pb <= pc) return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

// Per-row adaptive filtering, the heuristic from the PNG spec: pick the filter
// whose output has the smallest sum of absolute signed values. On screenshots
// this typically halves the compressed size versus filter 0 everywhere.
Result<std::vector<std::uint8_t>> encode_png(const Frame& f, int level) {
    const int w = f.width, h = f.height;
    if (w <= 0 || h <= 0) return err(ErrorCode::InvalidArgument, "empty frame");

    const bool has_alpha = (f.format != PixelFormat::RGB8);
    const int out_bpp = has_alpha ? 4 : 3;
    const std::size_t row_bytes = static_cast<std::size_t>(w) * out_bpp;

    std::vector<std::uint8_t> raw;
    raw.reserve((row_bytes + 1) * static_cast<std::size_t>(h));

    std::vector<std::uint8_t> cur(row_bytes), prev(row_bytes, 0);
    std::vector<std::uint8_t> cand[5];
    for (auto& c : cand) c.resize(row_bytes);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t r, g, b, a;
            read_rgb(f, x, y, r, g, b, a);
            std::uint8_t* d = cur.data() + static_cast<std::size_t>(x) * out_bpp;
            d[0] = r;
            d[1] = g;
            d[2] = b;
            if (has_alpha) d[3] = a;
        }

        long best_score = -1;
        int best_filter = 0;
        for (int ft = 0; ft < 5; ++ft) {
            long score = 0;
            for (std::size_t i = 0; i < row_bytes; ++i) {
                const int A = (i >= static_cast<std::size_t>(out_bpp)) ? cur[i - out_bpp] : 0;
                const int B = prev[i];
                const int C = (i >= static_cast<std::size_t>(out_bpp)) ? prev[i - out_bpp] : 0;
                int v = 0;
                switch (ft) {
                    case 0: v = cur[i]; break;
                    case 1: v = cur[i] - A; break;
                    case 2: v = cur[i] - B; break;
                    case 3: v = cur[i] - ((A + B) >> 1); break;
                    case 4: v = cur[i] - paeth(A, B, C); break;
                }
                cand[ft][i] = static_cast<std::uint8_t>(v & 0xFF);
                const int s = static_cast<std::int8_t>(cand[ft][i]);
                score += std::abs(s);
            }
            if (best_score < 0 || score < best_score) {
                best_score = score;
                best_filter = ft;
            }
        }

        raw.push_back(static_cast<std::uint8_t>(best_filter));
        raw.insert(raw.end(), cand[best_filter].begin(), cand[best_filter].end());
        prev.swap(cur);
    }

    auto idat = compress::zlib_compress(raw.data(), raw.size(), std::clamp(level, 0, 9));

    std::vector<std::uint8_t> out;
    out.reserve(idat.size() + 128);
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), kSig, kSig + 8);

    std::uint8_t ihdr[13];
    ihdr[0] = static_cast<std::uint8_t>(w >> 24);
    ihdr[1] = static_cast<std::uint8_t>(w >> 16);
    ihdr[2] = static_cast<std::uint8_t>(w >> 8);
    ihdr[3] = static_cast<std::uint8_t>(w);
    ihdr[4] = static_cast<std::uint8_t>(h >> 24);
    ihdr[5] = static_cast<std::uint8_t>(h >> 16);
    ihdr[6] = static_cast<std::uint8_t>(h >> 8);
    ihdr[7] = static_cast<std::uint8_t>(h);
    ihdr[8] = 8;                  // bit depth
    ihdr[9] = has_alpha ? 6 : 2;  // colour type: RGBA / RGB
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;  // deflate, adaptive filter, no interlace
    png_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    png_chunk(out, "IDAT", idat.data(), idat.size());
    png_chunk(out, "IEND", nullptr, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Baseline JPEG
// ---------------------------------------------------------------------------
// Worth carrying because a full-desktop PNG routinely lands at 2-6 MB, which
// blows past the payload budget of most agent transports, while the same frame
// at JPEG q80 is 150-400 KB and stays perfectly readable for UI work.

const int kZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

const int kQuantLum[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                           14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                           18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                           49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

const int kQuantChr[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                           24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                           99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                           99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Standard Annex-K Huffman tables.
const std::uint8_t kDcLumBits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const std::uint8_t kDcLumVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const std::uint8_t kDcChrBits[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const std::uint8_t kDcChrVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const std::uint8_t kAcLumBits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const std::uint8_t kAcLumVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const std::uint8_t kAcChrBits[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const std::uint8_t kAcChrVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct HuffTable {
    std::uint16_t code[256] = {};
    std::uint8_t size[256] = {};
};

HuffTable build_huff(const std::uint8_t bits[17], const std::uint8_t* vals, int nvals) {
    HuffTable t;
    std::uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < bits[len]; ++i) {
            if (k >= nvals) break;
            t.code[vals[k]] = code;
            t.size[vals[k]] = static_cast<std::uint8_t>(len);
            ++code;
            ++k;
        }
        code <<= 1;
    }
    return t;
}

class JpegWriter {
public:
    explicit JpegWriter(std::vector<std::uint8_t>& out) : out_(out) {}

    void byte(std::uint8_t b) { out_.push_back(b); }
    void word(std::uint16_t v) {
        byte(static_cast<std::uint8_t>(v >> 8));
        byte(static_cast<std::uint8_t>(v));
    }

    void bits(std::uint16_t code, int length) {
        for (int i = length - 1; i >= 0; --i) {
            acc_ = static_cast<std::uint8_t>((acc_ << 1) | ((code >> i) & 1));
            if (++nbits_ == 8) {
                out_.push_back(acc_);
                // 0xFF in entropy-coded data must be byte-stuffed with 0x00.
                if (acc_ == 0xFF) out_.push_back(0x00);
                acc_ = 0;
                nbits_ = 0;
            }
        }
    }
    void flush_bits() {
        while (nbits_ != 0) bits(1, 1);
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint8_t acc_ = 0;
    int nbits_ = 0;
};

void fdct_8x8(double* b) {
    // Separable float DCT-II. Not the fastest possible, but a 4K screenshot is
    // ~130k blocks and this runs in well under 100 ms; the capture itself
    // dominates.
    static double c[8][8];
    static bool init = [] {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = ((u == 0) ? std::sqrt(0.125) : 0.5) *
                          std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
        return true;
    }();
    (void)init;

    double tmp[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            double s = 0;
            for (int x = 0; x < 8; ++x) s += b[y * 8 + x] * c[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int x = 0; x < 8; ++x)
        for (int v = 0; v < 8; ++v) {
            double s = 0;
            for (int y = 0; y < 8; ++y) s += tmp[y * 8 + x] * c[v][y];
            b[v * 8 + x] = s;
        }
}

int magnitude_category(int v) {
    int a = std::abs(v), n = 0;
    while (a) {
        ++n;
        a >>= 1;
    }
    return n;
}

Result<std::vector<std::uint8_t>> encode_jpeg(const Frame& f, int quality) {
    const int w = f.width, h = f.height;
    if (w <= 0 || h <= 0) return err(ErrorCode::InvalidArgument, "empty frame");

    quality = std::clamp(quality, 1, 100);
    const int q_scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);

    int qlum[64], qchr[64];
    for (int i = 0; i < 64; ++i) {
        qlum[i] = std::clamp((kQuantLum[i] * q_scale + 50) / 100, 1, 255);
        qchr[i] = std::clamp((kQuantChr[i] * q_scale + 50) / 100, 1, 255);
    }

    const HuffTable dc_l = build_huff(kDcLumBits, kDcLumVal, 12);
    const HuffTable ac_l = build_huff(kAcLumBits, kAcLumVal, 162);
    const HuffTable dc_c = build_huff(kDcChrBits, kDcChrVal, 12);
    const HuffTable ac_c = build_huff(kAcChrBits, kAcChrVal, 162);

    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(w) * h / 6 + 1024);
    JpegWriter jw(out);

    jw.word(0xFFD8);  // SOI

    // DQT
    jw.word(0xFFDB);
    jw.word(2 + 65);
    jw.byte(0);
    for (int i = 0; i < 64; ++i) jw.byte(static_cast<std::uint8_t>(qlum[kZigZag[i]]));
    jw.word(0xFFDB);
    jw.word(2 + 65);
    jw.byte(1);
    for (int i = 0; i < 64; ++i) jw.byte(static_cast<std::uint8_t>(qchr[kZigZag[i]]));

    // SOF0, 3 components, 4:4:4 (no chroma subsampling: UI screenshots are
    // full of thin coloured text and 4:2:0 smears it).
    jw.word(0xFFC0);
    jw.word(8 + 3 * 3);
    jw.byte(8);
    jw.word(static_cast<std::uint16_t>(h));
    jw.word(static_cast<std::uint16_t>(w));
    jw.byte(3);
    jw.byte(1);
    jw.byte(0x11);
    jw.byte(0);
    jw.byte(2);
    jw.byte(0x11);
    jw.byte(1);
    jw.byte(3);
    jw.byte(0x11);
    jw.byte(1);

    auto write_dht = [&](int cls, int id, const std::uint8_t bits[17], const std::uint8_t* vals,
                         int nvals) {
        jw.word(0xFFC4);
        jw.word(static_cast<std::uint16_t>(2 + 1 + 16 + nvals));
        jw.byte(static_cast<std::uint8_t>((cls << 4) | id));
        for (int i = 1; i <= 16; ++i) jw.byte(bits[i]);
        for (int i = 0; i < nvals; ++i) jw.byte(vals[i]);
    };
    write_dht(0, 0, kDcLumBits, kDcLumVal, 12);
    write_dht(1, 0, kAcLumBits, kAcLumVal, 162);
    write_dht(0, 1, kDcChrBits, kDcChrVal, 12);
    write_dht(1, 1, kAcChrBits, kAcChrVal, 162);

    // SOS
    jw.word(0xFFDA);
    jw.word(6 + 2 * 3);
    jw.byte(3);
    jw.byte(1);
    jw.byte(0x00);
    jw.byte(2);
    jw.byte(0x11);
    jw.byte(3);
    jw.byte(0x11);
    jw.byte(0);
    jw.byte(63);
    jw.byte(0);

    int prev_dc[3] = {0, 0, 0};

    auto encode_block = [&](double* blk, const int* qt, const HuffTable& dct, const HuffTable& act,
                            int& pdc) {
        fdct_8x8(blk);
        int zz[64];
        for (int i = 0; i < 64; ++i) {
            const int idx = kZigZag[i];
            zz[i] = static_cast<int>(std::lround(blk[idx] / qt[idx]));
        }
        // DC: differential
        const int diff = zz[0] - pdc;
        pdc = zz[0];
        const int s = magnitude_category(diff);
        jw.bits(dct.code[s], dct.size[s]);
        if (s) {
            const int v = (diff < 0) ? diff - 1 + (1 << s) : diff;
            jw.bits(static_cast<std::uint16_t>(v), s);
        }
        // AC: run-length + magnitude
        int run = 0;
        for (int k = 1; k < 64; ++k) {
            if (zz[k] == 0) {
                ++run;
                continue;
            }
            while (run > 15) {
                jw.bits(act.code[0xF0], act.size[0xF0]);  // ZRL
                run -= 16;
            }
            const int sz = magnitude_category(zz[k]);
            const int sym = (run << 4) | sz;
            jw.bits(act.code[sym], act.size[sym]);
            const int v = (zz[k] < 0) ? zz[k] - 1 + (1 << sz) : zz[k];
            jw.bits(static_cast<std::uint16_t>(v), sz);
            run = 0;
        }
        if (run > 0) jw.bits(act.code[0x00], act.size[0x00]);  // EOB
    };

    double by[64], bcb[64], bcr[64];
    for (int my = 0; my < h; my += 8) {
        for (int mx = 0; mx < w; mx += 8) {
            for (int y = 0; y < 8; ++y) {
                const int sy = std::min(my + y, h - 1);
                for (int x = 0; x < 8; ++x) {
                    const int sx = std::min(mx + x, w - 1);
                    std::uint8_t r, g, b, a;
                    read_rgb(f, sx, sy, r, g, b, a);
                    // BT.601, level-shifted by -128 as the spec requires.
                    by[y * 8 + x] = 0.299 * r + 0.587 * g + 0.114 * b - 128.0;
                    bcb[y * 8 + x] = -0.168736 * r - 0.331264 * g + 0.5 * b;
                    bcr[y * 8 + x] = 0.5 * r - 0.418688 * g - 0.081312 * b;
                }
            }
            encode_block(by, qlum, dc_l, ac_l, prev_dc[0]);
            encode_block(bcb, qchr, dc_c, ac_c, prev_dc[1]);
            encode_block(bcr, qchr, dc_c, ac_c, prev_dc[2]);
        }
    }

    jw.flush_bits();
    jw.word(0xFFD9);  // EOI
    return out;
}

}  // namespace

DisplayGraph::ImageTransform Frame::transform() const {
    DisplayGraph::ImageTransform t;
    t.valid = !empty();
    t.source_physical = source_physical;
    t.scale = scale;
    t.width = width;
    t.height = height;
    return t;
}

Result<Frame> resize(const Frame& src, std::int32_t width, std::int32_t height) {
    if (src.empty()) return err(ErrorCode::InvalidArgument, "empty source frame");
    if (width <= 0 || height <= 0) return err(ErrorCode::InvalidArgument, "bad target size");
    if (width == src.width && height == src.height) return src;

    Frame dst;
    dst.width = width;
    dst.height = height;
    dst.format = src.format;
    const int bpp = bytes_per_pixel(src.format);
    dst.stride = width * bpp;
    dst.pixels.assign(static_cast<std::size_t>(dst.stride) * height, 0);
    dst.source_physical = src.source_physical;
    dst.scale = src.scale * (static_cast<double>(width) / src.width);

    const double fx = static_cast<double>(src.width) / width;
    const double fy = static_cast<double>(src.height) / height;

    // Box filter when downscaling, bilinear when upscaling. Box averaging is
    // what keeps small UI text legible after a 2x reduction; bilinear alone
    // aliases it into mush.
    const bool box = (fx > 1.2 || fy > 1.2);

    for (int y = 0; y < height; ++y) {
        std::uint8_t* drow = dst.pixels.data() + static_cast<std::size_t>(y) * dst.stride;
        for (int x = 0; x < width; ++x) {
            std::uint8_t* d = drow + static_cast<std::size_t>(x) * bpp;
            if (box) {
                const int x0 = static_cast<int>(x * fx);
                const int x1 = std::min(static_cast<int>((x + 1) * fx), src.width);
                const int y0 = static_cast<int>(y * fy);
                const int y1 = std::min(static_cast<int>((y + 1) * fy), src.height);
                std::uint32_t acc[4] = {0, 0, 0, 0};
                std::uint32_t n = 0;
                for (int sy = y0; sy < std::max(y1, y0 + 1); ++sy) {
                    const std::uint8_t* srow =
                        src.pixels.data() +
                        static_cast<std::size_t>(std::min(sy, src.height - 1)) * src.stride;
                    for (int sx = x0; sx < std::max(x1, x0 + 1); ++sx) {
                        const std::uint8_t* s =
                            srow + static_cast<std::size_t>(std::min(sx, src.width - 1)) * bpp;
                        for (int c = 0; c < bpp; ++c) acc[c] += s[c];
                        ++n;
                    }
                }
                if (n == 0) n = 1;
                for (int c = 0; c < bpp; ++c) d[c] = static_cast<std::uint8_t>(acc[c] / n);
            } else {
                const double sxf = (x + 0.5) * fx - 0.5;
                const double syf = (y + 0.5) * fy - 0.5;
                const int x0 = std::clamp(static_cast<int>(std::floor(sxf)), 0, src.width - 1);
                const int y0 = std::clamp(static_cast<int>(std::floor(syf)), 0, src.height - 1);
                const int x1 = std::min(x0 + 1, src.width - 1);
                const int y1 = std::min(y0 + 1, src.height - 1);
                const double tx = std::clamp(sxf - x0, 0.0, 1.0);
                const double ty = std::clamp(syf - y0, 0.0, 1.0);
                for (int c = 0; c < bpp; ++c) {
                    const double p00 = src.pixels[static_cast<std::size_t>(y0) * src.stride +
                                                  static_cast<std::size_t>(x0) * bpp + c];
                    const double p10 = src.pixels[static_cast<std::size_t>(y0) * src.stride +
                                                  static_cast<std::size_t>(x1) * bpp + c];
                    const double p01 = src.pixels[static_cast<std::size_t>(y1) * src.stride +
                                                  static_cast<std::size_t>(x0) * bpp + c];
                    const double p11 = src.pixels[static_cast<std::size_t>(y1) * src.stride +
                                                  static_cast<std::size_t>(x1) * bpp + c];
                    const double v = p00 * (1 - tx) * (1 - ty) + p10 * tx * (1 - ty) +
                                     p01 * (1 - tx) * ty + p11 * tx * ty;
                    d[c] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
                }
            }
        }
    }
    return dst;
}

Result<Frame> crop(const Frame& src, const Rect& region_in_frame) {
    if (src.empty()) return err(ErrorCode::InvalidArgument, "empty source frame");
    const int x0 = std::clamp(static_cast<int>(std::lround(region_in_frame.x)), 0, src.width);
    const int y0 = std::clamp(static_cast<int>(std::lround(region_in_frame.y)), 0, src.height);
    const int x1 =
        std::clamp(static_cast<int>(std::lround(region_in_frame.right())), x0, src.width);
    const int y1 =
        std::clamp(static_cast<int>(std::lround(region_in_frame.bottom())), y0, src.height);
    if (x1 <= x0 || y1 <= y0)
        return err(ErrorCode::InvalidArgument, "crop region is empty after clamping to the frame");

    const int bpp = bytes_per_pixel(src.format);
    Frame dst;
    dst.width = x1 - x0;
    dst.height = y1 - y0;
    dst.format = src.format;
    dst.stride = dst.width * bpp;
    dst.pixels.resize(static_cast<std::size_t>(dst.stride) * dst.height);
    for (int y = 0; y < dst.height; ++y) {
        std::memcpy(dst.pixels.data() + static_cast<std::size_t>(y) * dst.stride,
                    src.pixels.data() + static_cast<std::size_t>(y + y0) * src.stride +
                        static_cast<std::size_t>(x0) * bpp,
                    static_cast<std::size_t>(dst.stride));
    }
    dst.scale = src.scale;
    dst.source_physical =
        Rect{src.source_physical.x + x0 / src.scale, src.source_physical.y + y0 / src.scale,
             dst.width / src.scale, dst.height / src.scale, Space::Physical};
    return dst;
}

Result<std::vector<std::uint8_t>> encode(const Frame& f, const EncodeOptions& opts) {
    switch (opts.format) {
        case ImageFormat::PNG: return encode_png(f, opts.png_level);
        case ImageFormat::JPEG: return encode_jpeg(f, opts.quality);
        case ImageFormat::RAW: return f.pixels;
        case ImageFormat::WEBP:
            return err(ErrorCode::Unsupported, "WEBP encoding is not built in",
                       "Use PNG for lossless or JPEG for a small payload. WEBP would add a "
                       "libwebp dependency for a marginal gain over JPEG at these sizes.");
    }
    return err(ErrorCode::InvalidArgument, "unknown image format");
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v =
            (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back(kTable[v & 63]);
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t v = std::uint32_t(bytes[i]) << 16;
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t v =
            (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

}  // namespace cc
