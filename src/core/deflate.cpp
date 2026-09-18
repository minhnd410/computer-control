// SPDX-License-Identifier: MIT
#include "core/deflate.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(CC_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace cc::compress {
namespace {

std::uint32_t crc_table_entry(std::uint32_t n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}

const std::uint32_t* crc_table() {
    static std::uint32_t t[256];
    static bool init = [] {
        for (std::uint32_t i = 0; i < 256; ++i) t[i] = crc_table_entry(i);
        return true;
    }();
    (void)init;
    return t;
}

// ---- bit writer (DEFLATE is LSB-first) ------------------------------------
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}

    void bits(std::uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            acc_ |= ((value >> i) & 1u) << nbits_;
            if (++nbits_ == 8) {
                out_.push_back(static_cast<std::uint8_t>(acc_));
                acc_ = 0;
                nbits_ = 0;
            }
        }
    }
    // Huffman codes are stored MSB-first and must be emitted in that order.
    void huff(std::uint32_t code, int count) {
        for (int i = count - 1; i >= 0; --i) {
            acc_ |= ((code >> i) & 1u) << nbits_;
            if (++nbits_ == 8) {
                out_.push_back(static_cast<std::uint8_t>(acc_));
                acc_ = 0;
                nbits_ = 0;
            }
        }
    }
    void flush() {
        if (nbits_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(acc_));
            acc_ = 0;
            nbits_ = 0;
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint32_t acc_ = 0;
    int nbits_ = 0;
};

// Fixed Huffman literal/length table, RFC 1951 section 3.2.6.
void fixed_literal_code(int sym, std::uint32_t& code, int& len) {
    if (sym < 144) {
        code = 0x30 + sym;
        len = 8;
    } else if (sym < 256) {
        code = 0x190 + (sym - 144);
        len = 9;
    } else if (sym < 280) {
        code = 0x00 + (sym - 256);
        len = 7;
    } else {
        code = 0xC0 + (sym - 280);
        len = 8;
    }
}

struct LenCode {
    int code;
    int extra_bits;
    int base;
};
const LenCode kLen[] = {{257, 0, 3},   {258, 0, 4},   {259, 0, 5},   {260, 0, 6},   {261, 0, 7},
                        {262, 0, 8},   {263, 0, 9},   {264, 0, 10},  {265, 1, 11},  {266, 1, 13},
                        {267, 1, 15},  {268, 1, 17},  {269, 2, 19},  {270, 2, 23},  {271, 2, 27},
                        {272, 2, 31},  {273, 3, 35},  {274, 3, 43},  {275, 3, 51},  {276, 3, 59},
                        {277, 4, 67},  {278, 4, 83},  {279, 4, 99},  {280, 4, 115}, {281, 5, 131},
                        {282, 5, 163}, {283, 5, 195}, {284, 5, 227}, {285, 0, 258}};

struct DistCode {
    int code;
    int extra_bits;
    int base;
};
const DistCode kDist[] = {
    {0, 0, 1},      {1, 0, 2},      {2, 0, 3},       {3, 0, 4},       {4, 1, 5},
    {5, 1, 7},      {6, 2, 9},      {7, 2, 13},      {8, 3, 17},      {9, 3, 25},
    {10, 4, 33},    {11, 4, 49},    {12, 5, 65},     {13, 5, 97},     {14, 6, 129},
    {15, 6, 193},   {16, 7, 257},   {17, 7, 385},    {18, 8, 513},    {19, 8, 769},
    {20, 9, 1025},  {21, 9, 1537},  {22, 10, 2049},  {23, 10, 3073},  {24, 11, 4097},
    {25, 11, 6145}, {26, 12, 8193}, {27, 12, 12289}, {28, 13, 16385}, {29, 13, 24577}};

const LenCode& length_code(int len) {
    for (int i = 28; i >= 0; --i)
        if (len >= kLen[i].base) return kLen[i];
    return kLen[0];
}
const DistCode& distance_code(int d) {
    for (int i = 29; i >= 0; --i)
        if (d >= kDist[i].base) return kDist[i];
    return kDist[0];
}

constexpr std::size_t kWindow = 32768;
constexpr std::size_t kMinMatch = 3;
constexpr std::size_t kMaxMatch = 258;
constexpr std::size_t kHashBits = 15;
constexpr std::size_t kHashSize = 1u << kHashBits;

inline std::uint32_t hash3(const std::uint8_t* p) {
    return ((static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) |
            p[2]) *
               2654435761u >>
           (32 - kHashBits);
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed) {
    const std::uint32_t* t = crc_table();
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t len) {
    std::uint32_t a = 1, b = 0;
    constexpr std::uint32_t kMod = 65521;
    // Chunked so the accumulators cannot overflow before the modulo.
    while (len > 0) {
        const std::size_t n = std::min<std::size_t>(len, 5552);
        for (std::size_t i = 0; i < n; ++i) {
            a += data[i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        data += n;
        len -= n;
    }
    return (b << 16) | a;
}

bool using_system_zlib() {
#if defined(CC_HAVE_ZLIB)
    return true;
#else
    return false;
#endif
}

std::vector<std::uint8_t> deflate_raw(const std::uint8_t* data, std::size_t len, int level) {
    std::vector<std::uint8_t> out;
    out.reserve(len / 2 + 64);
    BitWriter bw(out);

    // Single fixed-Huffman block for the whole stream. Dynamic Huffman would
    // gain another ~10-15% at a large complexity cost; the system-zlib path
    // covers the case where that matters.
    bw.bits(1, 1);  // BFINAL
    bw.bits(1, 2);  // BTYPE = 01, fixed Huffman

    auto emit_literal = [&](std::uint8_t b) {
        std::uint32_t code;
        int n;
        fixed_literal_code(b, code, n);
        bw.huff(code, n);
    };

    if (level <= 0 || len < kMinMatch) {
        for (std::size_t i = 0; i < len; ++i) emit_literal(data[i]);
    } else {
        // Hash-chain matcher. `max_chain` is the speed/ratio dial.
        const int max_chain = (level >= 7) ? 256 : (level >= 4 ? 64 : 16);
        std::vector<std::int32_t> head(kHashSize, -1);
        std::vector<std::int32_t> prev(len, -1);

        std::size_t i = 0;
        while (i < len) {
            std::size_t best_len = 0, best_dist = 0;
            if (i + kMinMatch <= len) {
                const std::uint32_t h = hash3(data + i);
                std::int32_t cand = head[h];
                int chain = max_chain;
                const std::size_t max_here = std::min(kMaxMatch, len - i);
                while (cand >= 0 && chain-- > 0) {
                    const std::size_t d = i - static_cast<std::size_t>(cand);
                    if (d == 0 || d > kWindow) break;
                    const std::uint8_t* a = data + cand;
                    const std::uint8_t* b = data + i;
                    // Cheap reject before the full compare.
                    if (best_len >= kMinMatch && a[best_len] != b[best_len]) {
                        cand = prev[static_cast<std::size_t>(cand)];
                        continue;
                    }
                    std::size_t m = 0;
                    while (m < max_here && a[m] == b[m]) ++m;
                    if (m > best_len) {
                        best_len = m;
                        best_dist = d;
                        if (m >= max_here) break;
                    }
                    cand = prev[static_cast<std::size_t>(cand)];
                }
                prev[i] = head[h];
                head[h] = static_cast<std::int32_t>(i);
            }

            if (best_len >= kMinMatch) {
                const LenCode& lc = length_code(static_cast<int>(best_len));
                std::uint32_t code;
                int n;
                fixed_literal_code(lc.code, code, n);
                bw.huff(code, n);
                if (lc.extra_bits)
                    bw.bits(static_cast<std::uint32_t>(best_len - lc.base), lc.extra_bits);

                const DistCode& dc = distance_code(static_cast<int>(best_dist));
                bw.huff(static_cast<std::uint32_t>(dc.code), 5);
                if (dc.extra_bits)
                    bw.bits(static_cast<std::uint32_t>(best_dist - dc.base), dc.extra_bits);

                // Insert the skipped positions so later matches can find them.
                for (std::size_t k = 1; k < best_len && i + k + kMinMatch <= len; ++k) {
                    const std::uint32_t h2 = hash3(data + i + k);
                    prev[i + k] = head[h2];
                    head[h2] = static_cast<std::int32_t>(i + k);
                }
                i += best_len;
            } else {
                emit_literal(data[i]);
                ++i;
            }
        }
    }

    std::uint32_t code;
    int n;
    fixed_literal_code(256, code, n);  // end-of-block
    bw.huff(code, n);
    bw.flush();
    return out;
}

std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t len, int level) {
#if defined(CC_HAVE_ZLIB)
    {
        uLongf bound = compressBound(static_cast<uLong>(len));
        std::vector<std::uint8_t> zout(bound);
        const int lv = std::clamp(level, 0, 9);
        if (::compress2(zout.data(), &bound, data, static_cast<uLong>(len), lv) == Z_OK) {
            zout.resize(bound);
            return zout;
        }
        // Fall through to the bundled encoder if zlib refused for any reason.
    }
#endif
    std::vector<std::uint8_t> out;
    out.push_back(0x78);  // CM=8 (deflate), CINFO=7 (32K window)
    out.push_back(0x01);  // FCHECK so (0x78<<8 | 0x01) % 31 == 0, no dict
    auto body = deflate_raw(data, len, level);
    out.insert(out.end(), body.begin(), body.end());
    const std::uint32_t a = adler32(data, len);
    out.push_back(static_cast<std::uint8_t>(a >> 24));
    out.push_back(static_cast<std::uint8_t>(a >> 16));
    out.push_back(static_cast<std::uint8_t>(a >> 8));
    out.push_back(static_cast<std::uint8_t>(a));
    return out;
}

}  // namespace cc::compress

// ---------------------------------------------------------------------------
// Inflate
// ---------------------------------------------------------------------------

namespace cc::compress {
namespace {

// Canonical Huffman decoder. Small and allocation-free per symbol; PNG rows
// are the hot path and a table-driven decoder is not worth the complexity at
// screenshot sizes.
struct Huffman {
    // counts[len] = number of codes of that length; symbols in canonical order.
    std::array<std::uint16_t, 16> counts{};
    std::vector<std::uint16_t> symbols;

    void build(const std::uint8_t* lengths, std::size_t n) {
        counts.fill(0);
        for (std::size_t i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        std::array<std::uint16_t, 16> offsets{};
        std::uint16_t total = 0;
        for (int len = 1; len < 16; ++len) {
            offsets[len] = total;
            total = static_cast<std::uint16_t>(total + counts[len]);
        }
        symbols.assign(total, 0);
        for (std::size_t i = 0; i < n; ++i) {
            if (lengths[i]) symbols[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
        }
    }
};

class BitReader {
public:
    BitReader(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}

    bool bits(int count, std::uint32_t* out) {
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            if (nbits_ == 0) {
                if (pos_ >= n_) return false;
                acc_ = d_[pos_++];
                nbits_ = 8;
            }
            v |= static_cast<std::uint32_t>(acc_ & 1) << i;
            acc_ >>= 1;
            --nbits_;
        }
        *out = v;
        return true;
    }

    bool decode(const Huffman& h, int* out) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            std::uint32_t b = 0;
            if (!bits(1, &b)) return false;
            code |= static_cast<int>(b);
            const int count = h.counts[len];
            if (code - first < count) {
                *out = h.symbols[static_cast<std::size_t>(index + (code - first))];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }

    void align() { nbits_ = 0; }
    bool read_bytes(std::size_t count, std::vector<std::uint8_t>& out) {
        if (pos_ + count > n_) return false;
        out.insert(out.end(), d_ + pos_, d_ + pos_ + count);
        pos_ += count;
        return true;
    }
    std::size_t pos() const { return pos_; }

private:
    const std::uint8_t* d_;
    std::size_t n_;
    std::size_t pos_ = 0;
    std::uint8_t acc_ = 0;
    int nbits_ = 0;
};

const std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                     33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_block_data(BitReader& br, const Huffman& lit, const Huffman& dist,
                        std::vector<std::uint8_t>& out) {
    for (;;) {
        int sym = 0;
        if (!br.decode(lit, &sym)) return false;
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            std::uint32_t extra = 0;
            if (!br.bits(kLenExtra[sym], &extra)) return false;
            const std::size_t length = kLenBase[sym] + extra;

            int dsym = 0;
            if (!br.decode(dist, &dsym)) return false;
            if (dsym >= 30) return false;
            if (!br.bits(kDistExtra[dsym], &extra)) return false;
            const std::size_t distance = kDistBase[dsym] + extra;
            if (distance > out.size()) return false;

            // Overlapping copies are legal and common, so copy byte by byte
            // rather than memcpy.
            const std::size_t start = out.size() - distance;
            for (std::size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
        }
    }
}

bool inflate_raw(const std::uint8_t* data, std::size_t len, std::vector<std::uint8_t>& out) {
    BitReader br(data, len);
    for (;;) {
        std::uint32_t final_block = 0, type = 0;
        if (!br.bits(1, &final_block)) return false;
        if (!br.bits(2, &type)) return false;

        if (type == 0) {
            br.align();
            std::uint32_t lo = 0, hi = 0, nlo = 0, nhi = 0;
            if (!br.bits(8, &lo) || !br.bits(8, &hi) || !br.bits(8, &nlo) || !br.bits(8, &nhi))
                return false;
            const std::size_t n = lo | (hi << 8);
            if (!br.read_bytes(n, out)) return false;
        } else if (type == 1) {
            // Fixed Huffman tables.
            static Huffman lit, dist;
            static bool built = [] {
                std::uint8_t ll[288];
                for (int i = 0; i < 144; ++i) ll[i] = 8;
                for (int i = 144; i < 256; ++i) ll[i] = 9;
                for (int i = 256; i < 280; ++i) ll[i] = 7;
                for (int i = 280; i < 288; ++i) ll[i] = 8;
                lit.build(ll, 288);
                std::uint8_t dl[30];
                for (int i = 0; i < 30; ++i) dl[i] = 5;
                dist.build(dl, 30);
                return true;
            }();
            (void)built;
            if (!inflate_block_data(br, lit, dist, out)) return false;
        } else if (type == 2) {
            std::uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!br.bits(5, &hlit) || !br.bits(5, &hdist) || !br.bits(4, &hclen)) return false;
            hlit += 257;
            hdist += 1;
            hclen += 4;

            static const int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                           11, 4,  12, 3, 13, 2, 14, 1, 15};
            std::uint8_t code_lengths[19] = {0};
            for (std::uint32_t i = 0; i < hclen; ++i) {
                std::uint32_t v = 0;
                if (!br.bits(3, &v)) return false;
                code_lengths[kOrder[i]] = static_cast<std::uint8_t>(v);
            }
            Huffman clh;
            clh.build(code_lengths, 19);

            std::vector<std::uint8_t> lengths(hlit + hdist, 0);
            std::size_t i = 0;
            while (i < lengths.size()) {
                int sym = 0;
                if (!br.decode(clh, &sym)) return false;
                if (sym < 16) {
                    lengths[i++] = static_cast<std::uint8_t>(sym);
                } else if (sym == 16) {
                    if (i == 0) return false;
                    std::uint32_t rep = 0;
                    if (!br.bits(2, &rep)) return false;
                    const std::uint8_t prev = lengths[i - 1];
                    for (std::uint32_t k = 0; k < rep + 3 && i < lengths.size(); ++k)
                        lengths[i++] = prev;
                } else if (sym == 17) {
                    std::uint32_t rep = 0;
                    if (!br.bits(3, &rep)) return false;
                    for (std::uint32_t k = 0; k < rep + 3 && i < lengths.size(); ++k)
                        lengths[i++] = 0;
                } else {
                    std::uint32_t rep = 0;
                    if (!br.bits(7, &rep)) return false;
                    for (std::uint32_t k = 0; k < rep + 11 && i < lengths.size(); ++k)
                        lengths[i++] = 0;
                }
            }
            Huffman lit, dist;
            lit.build(lengths.data(), hlit);
            dist.build(lengths.data() + hlit, hdist);
            if (!inflate_block_data(br, lit, dist, out)) return false;
        } else {
            return false;  // reserved block type
        }
        if (final_block) return true;
    }
}

}  // namespace

bool zlib_uncompress(const std::uint8_t* data, std::size_t len, std::vector<std::uint8_t>& out,
                     std::size_t expected_size) {
    if (len < 2) return false;
#if defined(CC_HAVE_ZLIB)
    if (expected_size > 0) {
        out.assign(expected_size, 0);
        uLongf dest_len = static_cast<uLongf>(expected_size);
        const int rc = ::uncompress(out.data(), &dest_len, data, static_cast<uLong>(len));
        if (rc == Z_OK) {
            out.resize(dest_len);
            return true;
        }
    }
#endif
    out.clear();
    if (expected_size) out.reserve(expected_size);
    // Skip the 2-byte zlib header; a preset dictionary (FDICT) is never used
    // by PNG and is not supported here.
    if ((data[1] & 0x20) != 0) return false;
    return inflate_raw(data + 2, len - 2, out);
}

}  // namespace cc::compress
