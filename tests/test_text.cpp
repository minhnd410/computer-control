// SPDX-License-Identifier: MIT
#include <string>

#include "cc/types.hpp"
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

// --- argument parsing -------------------------------------------------------
// parse_rect's string form is documented in the README and used by the CLI,
// which turns `--region 0,0,800,600` into a bare string. It silently did not
// work.

namespace cc::actions {
Result<Rect> parse_rect(const json::Value& v, const char* field);
Result<Point> parse_point(const json::Value& v, const char* field);
}  // namespace cc::actions

TEST(parse_rect_accepts_the_documented_string_form) {
    auto r = cc::actions::parse_rect(json::Value("10,20,300,400"), "region");
    CHECK(r.ok());
    if (r) {
        CHECK_NEAR(r.value().x, 10, 1e-9);
        CHECK_NEAR(r.value().y, 20, 1e-9);
        CHECK_NEAR(r.value().w, 300, 1e-9);
        CHECK_NEAR(r.value().h, 400, 1e-9);
        CHECK(r.value().space == Space::Logical);
    }
}

TEST(parse_rect_honours_a_space_suffix) {
    auto r = cc::actions::parse_rect(json::Value("0,0,640,480@physical"), "region");
    CHECK(r.ok());
    if (r) CHECK(r.value().space == Space::Physical);
}

TEST(parse_rect_still_accepts_arrays_and_objects) {
    json::Value arr = json::Value::array();
    for (double v : {1.0, 2.0, 3.0, 4.0}) arr.push_back(v);
    auto a = cc::actions::parse_rect(arr, "region");
    CHECK(a.ok());
    if (a) CHECK_NEAR(a.value().w, 3, 1e-9);

    json::Value obj = json::Value::object();
    obj.set("x", 5);
    obj.set("y", 6);
    obj.set("w", 7);
    obj.set("h", 8);
    obj.set("space", "image");
    auto o = cc::actions::parse_rect(obj, "region");
    CHECK(o.ok());
    if (o) {
        CHECK_NEAR(o.value().h, 8, 1e-9);
        CHECK(o.value().space == Space::Image);
    }
}

TEST(parse_rect_rejects_malformed_input) {
    CHECK(!cc::actions::parse_rect(json::Value("1,2,3"), "region").ok());
    CHECK(!cc::actions::parse_rect(json::Value("a,b,c,d"), "region").ok());
    CHECK(!cc::actions::parse_rect(json::Value("0,0,1,1@nonsense"), "region").ok());
    CHECK(!cc::actions::parse_rect(json::Value(42), "region").ok());
}

TEST(parse_point_string_and_space_forms) {
    auto p = cc::actions::parse_point(json::Value("100,200"), "at");
    CHECK(p.ok());
    if (p) CHECK_NEAR(p.value().x, 100, 1e-9);

    auto i = cc::actions::parse_point(json::Value("50,60@image"), "at");
    CHECK(i.ok());
    if (i) CHECK(i.value().space == Space::Image);

    CHECK(!cc::actions::parse_point(json::Value("nope"), "at").ok());
}

// --- PowerShell -EncodedCommand ---------------------------------------------
// Quoting a PowerShell command on a Windows command line cannot be done
// reliably: CommandLineToArgvW, cmd and PowerShell each reinterpret the
// quotes. -EncodedCommand takes base64 of UTF-16LE instead. The encoding is
// Windows-only in use but plain arithmetic, so it is tested everywhere.
// Expected values come from Python's codecs, not from this implementation.

TEST(utf16le_base64_matches_reference_encoding) {
    CHECK_EQ(text::utf16le_base64("echo hello"), std::string("ZQBjAGgAbwAgAGgAZQBsAGwAbwA="));
    // The case that broke the old quoting: embedded double quotes.
    CHECK_EQ(text::utf16le_base64("Write-Host \"hi\""),
             std::string("VwByAGkAdABlAC0ASABvAHMAdAAgACIAaABpACIA"));
}

TEST(utf16le_base64_handles_non_bmp) {
    // café + U+1F600, which needs a surrogate pair in UTF-16.
    CHECK_EQ(text::utf16le_base64("Write-Host \"caf\xC3\xA9 \xF0\x9F\x98\x80\""),
             std::string("VwByAGkAdABlAC0ASABvAHMAdAAgACIAYwBhAGYA6QAgAD3YAN4iAA=="));
}

TEST(utf16le_base64_is_empty_for_empty_input) {
    CHECK_EQ(text::utf16le_base64(""), std::string(""));
}

TEST(utf16le_base64_emits_no_bom) {
    // A BOM would make PowerShell treat the first character as content.
    const std::string encoded = text::utf16le_base64("a");
    CHECK_EQ(encoded, std::string("YQA="));
}

TEST(utf16le_base64_repairs_malformed_input) {
    // A truncated sequence must not propagate into a command PowerShell will
    // then fail to parse; it becomes U+FFFD.
    const std::string encoded = text::utf16le_base64("ok\xC3");
    CHECK(!encoded.empty());
    CHECK_EQ(encoded, std::string("bwBrAP3/"));
}
