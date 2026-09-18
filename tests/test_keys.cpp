// SPDX-License-Identifier: MIT
#include "cc/keys.hpp"
#include "test_framework.hpp"

using namespace cc;

TEST(chord_parses_modifiers) {
    auto c = parse_chord("cmd+shift+a");
    CHECK(c.ok());
    CHECK(has(c.value().modifiers, Modifier::Meta));
    CHECK(has(c.value().modifiers, Modifier::Shift));
    CHECK_EQ(c.value().keys.size(), std::size_t(1));
    CHECK(c.value().keys[0] == Key::A);
}

TEST(chord_accepts_platform_aliases) {
    for (const char* spec : {"win+r", "super+r", "meta+r", "command+r"}) {
        auto c = parse_chord(spec);
        CHECK(c.ok());
        CHECK(has(c.value().modifiers, Modifier::Meta));
        CHECK(c.value().keys[0] == Key::R);
    }
}

TEST(chord_accepts_dash_separator) {
    auto c = parse_chord("ctrl-c");
    CHECK(c.ok());
    CHECK(has(c.value().modifiers, Modifier::Control));
    CHECK(c.value().keys[0] == Key::C);
}

TEST(chord_handles_punctuation_keys) {
    // These are the cases a naive split on '+' or '-' gets wrong.
    auto minus = parse_chord("ctrl+-");
    CHECK(minus.ok());
    CHECK(has(minus.value().modifiers, Modifier::Control));
    CHECK(minus.value().keys[0] == Key::Minus);

    auto equal = parse_chord("shift+=");
    CHECK(equal.ok());
    CHECK(equal.value().keys[0] == Key::Equal);

    auto slash = parse_chord("cmd+/");
    CHECK(slash.ok());
    CHECK(slash.value().keys[0] == Key::Slash);
}

TEST(chord_bare_modifier_is_tappable) {
    auto c = parse_chord("cmd");
    CHECK(c.ok());
    CHECK_EQ(c.value().keys.size(), std::size_t(1));
    CHECK(is_modifier_key(c.value().keys[0]));
}

TEST(chord_function_and_named_keys) {
    CHECK(parse_chord("f12").value().keys[0] == Key::F12);
    CHECK(parse_chord("Escape").value().keys[0] == Key::Escape);
    CHECK(parse_chord("PageDown").value().keys[0] == Key::PageDown);
    CHECK(parse_chord("enter").value().keys[0] == Key::Return);
}

TEST(chord_rejects_unknown_key) {
    auto c = parse_chord("cmd+nosuchkey");
    CHECK(!c.ok());
    CHECK(c.error().code == ErrorCode::InvalidArgument);
    CHECK(!c.error().remedy.empty());
}

TEST(chord_sequence_splits_on_space) {
    auto seq = parse_chord_sequence("cmd+k cmd+s");
    CHECK(seq.ok());
    CHECK_EQ(seq.value().size(), std::size_t(2));
    CHECK(seq.value()[0].keys[0] == Key::K);
    CHECK(seq.value()[1].keys[0] == Key::S);
}
