// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "cc/window.hpp"

namespace cc {
namespace {
std::string fold(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}
}  // namespace

// Scoring is tiered rather than a single edit distance, because window titles
// are long and the useful matches are almost always prefix or substring hits:
// "chrome" should match "Google Chrome - Inbox (42)" strongly, and an edit
// distance would score that near zero.
int fuzzy_score(std::string_view needle_in, std::string_view haystack_in) {
    const std::string needle = fold(needle_in);
    const std::string hay = fold(haystack_in);
    if (needle.empty() || hay.empty()) return 0;
    if (needle == hay) return 100;

    if (hay.rfind(needle, 0) == 0) return 95;  // prefix
    const auto pos = hay.find(needle);
    if (pos != std::string::npos) {
        // Word-boundary substrings beat mid-word ones.
        const bool boundary = (pos == 0) || hay[pos - 1] == ' ' || hay[pos - 1] == '-' ||
                              hay[pos - 1] == '_' || hay[pos - 1] == '.';
        return boundary ? 88 : 80;
    }

    // Subsequence match: every needle char appears in order. Score decays with
    // how spread out the matches are, so "gc" prefers "GitHub Copilot" over
    // "Google Chrome ... Calendar" only when it is genuinely tighter.
    std::size_t i = 0, first = std::string::npos, last = 0;
    for (std::size_t j = 0; j < hay.size() && i < needle.size(); ++j) {
        if (hay[j] == needle[i]) {
            if (first == std::string::npos) first = j;
            last = j;
            ++i;
        }
    }
    if (i < needle.size()) return 0;

    const double span = static_cast<double>(last - first + 1);
    const double density = static_cast<double>(needle.size()) / std::max(1.0, span);
    return static_cast<int>(20 + 50 * density);
}

}  // namespace cc
