// SPDX-License-Identifier: MIT
#include "core/text.hpp"

#include <cstdint>
#include <vector>

#include "cc/screen.hpp"

namespace cc::text {
namespace {

// Length of the UTF-8 sequence starting with this lead byte, or 0 if it is
// not a valid lead byte.
int sequence_length(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// Validates one sequence at `i`, rejecting the encodings that are structurally
// well-formed but illegal: overlong forms, surrogates, and anything past
// U+10FFFF. Those are the ones security advisories are written about.
bool valid_sequence(std::string_view s, std::size_t i, int len) {
    if (i + static_cast<std::size_t>(len) > s.size()) return false;
    const auto b0 = static_cast<unsigned char>(s[i]);

    for (int k = 1; k < len; ++k) {
        if (!is_continuation(static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]))) {
            return false;
        }
    }
    if (len == 1) return true;

    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    if (len == 2) return b0 >= 0xC2;  // C0/C1 are overlong
    if (len == 3) {
        if (b0 == 0xE0 && b1 < 0xA0) return false;   // overlong
        if (b0 == 0xED && b1 >= 0xA0) return false;  // UTF-16 surrogate
        return true;
    }
    if (b0 == 0xF0 && b1 < 0x90) return false;   // overlong
    if (b0 == 0xF4 && b1 >= 0x90) return false;  // > U+10FFFF
    return b0 <= 0xF4;
}

}  // namespace

bool is_valid_utf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const int len = sequence_length(static_cast<unsigned char>(s[i]));
        if (len == 0 || !valid_sequence(s, i, len)) return false;
        i += static_cast<std::size_t>(len);
    }
    return true;
}

std::string truncate_utf8(std::string_view s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    if (max_bytes == 0) return {};

    // Walk forward so the cut always lands on a sequence boundary. Walking
    // backward from max_bytes would also work, but this way a malformed input
    // cannot push the cut past the limit.
    std::size_t end = 0;
    while (end < s.size()) {
        const int len = sequence_length(static_cast<unsigned char>(s[end]));
        const std::size_t step = (len == 0) ? 1 : static_cast<std::size_t>(len);
        if (end + step > max_bytes) break;
        end += step;
    }
    return std::string(s.substr(0, end));
}

std::string pad_utf8(std::string_view s, std::size_t width) {
    std::string out = truncate_utf8(s, width);
    if (out.size() < width) out.append(width - out.size(), ' ');
    return out;
}

std::string utf16le_base64(std::string_view utf8) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(utf8.size() * 2);

    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const int len = sequence_length(lead);
        if (len == 0 || !valid_sequence(utf8, i, len)) {
            // Emit U+FFFD rather than propagating a malformed byte into a
            // command PowerShell will then fail to parse.
            bytes.push_back(0xFD);
            bytes.push_back(0xFF);
            ++i;
            continue;
        }

        std::uint32_t cp = 0;
        switch (len) {
            case 1: cp = lead; break;
            case 2: cp = lead & 0x1Fu; break;
            case 3: cp = lead & 0x0Fu; break;
            default: cp = lead & 0x07u; break;
        }
        for (int k = 1; k < len; ++k) {
            cp = (cp << 6) |
                 (static_cast<unsigned char>(utf8[i + static_cast<std::size_t>(k)]) & 0x3Fu);
        }
        i += static_cast<std::size_t>(len);

        auto push = [&bytes](std::uint16_t unit) {
            bytes.push_back(static_cast<std::uint8_t>(unit & 0xFF));
            bytes.push_back(static_cast<std::uint8_t>((unit >> 8) & 0xFF));
        };
        if (cp < 0x10000) {
            push(static_cast<std::uint16_t>(cp));
        } else {
            // Outside the BMP needs a surrogate pair; UTF-16LE has no other
            // way to carry it.
            cp -= 0x10000;
            push(static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
            push(static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return base64(bytes);
}

std::string sanitize_utf8(std::string_view s) {
    if (is_valid_utf8(s)) return std::string(s);

    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const int len = sequence_length(static_cast<unsigned char>(s[i]));
        if (len > 0 && valid_sequence(s, i, len)) {
            out.append(s.substr(i, static_cast<std::size_t>(len)));
            i += static_cast<std::size_t>(len);
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
            ++i;
        }
    }
    return out;
}

}  // namespace cc::text
