// SPDX-License-Identifier: MIT
#include <string>

#include "core/json.hpp"
#include "core/text.hpp"
#include "test_framework.hpp"

using namespace cc;

namespace {
// Multi-byte samples: 2-byte (é), 3-byte (日本語), 4-byte (emoji).
const std::string kAccented = "caf\xC3\xA9";                           // café
const std::string kJapanese = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";  // 日本語
const std::string kEmoji = "\xF0\x9F\x98\x80";                         // grinning face
}  // namespace

TEST(utf8_validation_accepts_wellformed) {
    CHECK(text::is_valid_utf8(""));
    CHECK(text::is_valid_utf8("plain ascii"));
    CHECK(text::is_valid_utf8(kAccented));
    CHECK(text::is_valid_utf8(kJapanese));
    CHECK(text::is_valid_utf8(kEmoji));
}

TEST(utf8_validation_rejects_malformed) {
    CHECK(!text::is_valid_utf8("\xC3"));          // truncated 2-byte
    CHECK(!text::is_valid_utf8("\xE6\x97"));      // truncated 3-byte
    CHECK(!text::is_valid_utf8("\xF0\x9F\x98"));  // truncated 4-byte
    CHECK(!text::is_valid_utf8("\x80"));          // lone continuation
    CHECK(!text::is_valid_utf8("\xFF\xFE"));      // not UTF-8 at all
    // Overlong encodings and surrogates are structurally plausible but
    // illegal, and are exactly what security advisories are written about.
    CHECK(!text::is_valid_utf8("\xC0\xAF"));          // overlong '/'
    CHECK(!text::is_valid_utf8("\xE0\x80\xAF"));      // overlong
    CHECK(!text::is_valid_utf8("\xED\xA0\x80"));      // UTF-16 surrogate D800
    CHECK(!text::is_valid_utf8("\xF5\x80\x80\x80"));  // > U+10FFFF
}

TEST(truncate_never_splits_a_character) {
    // This is the bug that produced invalid JSON: "%.28s" cuts by bytes.
    // Truncating "日本語" to 4 bytes must yield one whole character, not one
    // and a third.
    for (std::size_t n = 0; n <= kJapanese.size() + 2; ++n) {
        const std::string out = text::truncate_utf8(kJapanese, n);
        CHECK(text::is_valid_utf8(out));
        CHECK(out.size() <= n || n > kJapanese.size());
    }
    CHECK_EQ(text::truncate_utf8(kJapanese, 4).size(), std::size_t(3));
    CHECK_EQ(text::truncate_utf8(kJapanese, 2).size(), std::size_t(0));
    CHECK_EQ(text::truncate_utf8(kEmoji, 3).size(), std::size_t(0));
    CHECK_EQ(text::truncate_utf8(kEmoji, 4).size(), std::size_t(4));
}

TEST(truncate_leaves_short_input_alone) {
    CHECK_EQ(text::truncate_utf8("abc", 10), std::string("abc"));
    CHECK_EQ(text::truncate_utf8(kEmoji, 100), kEmoji);
}

TEST(pad_aligns_and_stays_valid) {
    const std::string padded = text::pad_utf8(kAccented, 10);
    CHECK_EQ(padded.size(), std::size_t(10));
    CHECK(text::is_valid_utf8(padded));
    // Padding to a width that would split a character truncates first.
    const std::string tight = text::pad_utf8(kJapanese, 4);
    CHECK_EQ(tight.size(), std::size_t(4));
    CHECK(text::is_valid_utf8(tight));
}

TEST(sanitize_repairs_malformed_input) {
    const std::string repaired = text::sanitize_utf8("ok\xC3 bad\x80 end");
    CHECK(text::is_valid_utf8(repaired));
    CHECK(repaired.find("ok") != std::string::npos);
    CHECK(repaired.find("end") != std::string::npos);
    CHECK(repaired.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD
}

TEST(sanitize_leaves_valid_input_untouched) {
    CHECK_EQ(text::sanitize_utf8(kJapanese), kJapanese);
    CHECK_EQ(text::sanitize_utf8(kEmoji), kEmoji);
    CHECK_EQ(text::sanitize_utf8("plain"), std::string("plain"));
}

TEST(json_output_is_always_valid_utf8) {
    // The real defence: whatever mangled bytes an OS API hands back, what goes
    // on the wire must still parse. A split character used to invalidate the
    // whole document and drop the MCP connection far from the cause.
    json::Value v = json::Value::object();
    v.set("truncated_emoji", std::string("\xF0\x9F\x98"));  // 3 of 4 bytes
    v.set("lone_continuation", std::string("\x80\x80"));
    v.set("surrogate", std::string("\xED\xA0\x80"));
    v.set("good", kJapanese);

    const std::string dumped = v.dump();
    CHECK(text::is_valid_utf8(dumped));

    json::ParseError pe;
    json::Value again = json::parse(dumped, &pe);
    CHECK(pe.ok);
    CHECK_EQ(again["good"].as_string(), kJapanese);
}

TEST(json_roundtrips_multibyte_text) {
    json::Value v = json::Value::object();
    v.set("jp", kJapanese);
    v.set("emoji", kEmoji);
    v.set("accent", kAccented);

    json::ParseError pe;
    json::Value again = json::parse(v.dump(), &pe);
    CHECK(pe.ok);
    CHECK_EQ(again["jp"].as_string(), kJapanese);
    CHECK_EQ(again["emoji"].as_string(), kEmoji);
    CHECK_EQ(again["accent"].as_string(), kAccented);
}
