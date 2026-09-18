// SPDX-License-Identifier: MIT
#include "core/json.hpp"
#include "test_framework.hpp"

using namespace cc::json;

TEST(json_roundtrip_scalars) {
    ParseError pe;
    Value v = parse(R"({"a":1,"b":"two","c":true,"d":null,"e":3.5})", &pe);
    CHECK(pe.ok);
    CHECK_EQ(v["a"].as_int(), 1);
    CHECK_EQ(v["b"].as_string(), std::string("two"));
    CHECK(v["c"].as_bool());
    CHECK(v["d"].is_null());
    CHECK_NEAR(v["e"].as_double(), 3.5, 1e-9);
}

TEST(json_nested_and_arrays) {
    ParseError pe;
    Value v = parse(R"({"pts":[[1,2],[3,4]],"o":{"k":[true,false]}})", &pe);
    CHECK(pe.ok);
    CHECK_EQ(v["pts"].size(), std::size_t(2));
    CHECK_EQ(v["pts"][1][0].as_int(), 3);
    CHECK(v["o"]["k"][0].as_bool());
}

TEST(json_missing_keys_read_as_null) {
    Value v = parse(R"({"a":1})");
    CHECK(v["nope"].is_null());
    CHECK_EQ(v["nope"].as_int(42), 42);
    CHECK_EQ(v["nope"]["deeper"].as_string("d"), std::string("d"));
}

TEST(json_escapes_and_unicode) {
    ParseError pe;
    Value v = parse(R"({"s":"line\nbreak \"quoted\" é 😀"})", &pe);
    CHECK(pe.ok);
    const std::string s = v["s"].as_string();
    CHECK(s.find('\n') != std::string::npos);
    CHECK(s.find('"') != std::string::npos);
    // e-acute is two UTF-8 bytes, the emoji is four.
    CHECK(s.find("\xc3\xa9") != std::string::npos);
    CHECK(s.find("\xf0\x9f\x98\x80") != std::string::npos);
}

TEST(json_rejects_malformed) {
    ParseError pe;
    parse("{\"a\":}", &pe);
    CHECK(!pe.ok);
    parse("[1,2", &pe);
    CHECK(!pe.ok);
    parse("", &pe);
    CHECK(!pe.ok);
}

TEST(json_dump_is_reparseable) {
    Value v = Value::object();
    v.set("text", "quote \" and \\ backslash\nnewline");
    v.set("n", -12.25);
    Value arr = Value::array();
    arr.push_back(1);
    arr.push_back("two");
    v.set("arr", arr);

    ParseError pe;
    Value again = parse(v.dump(), &pe);
    CHECK(pe.ok);
    CHECK_EQ(again["text"].as_string(), v["text"].as_string());
    CHECK_NEAR(again["n"].as_double(), -12.25, 1e-9);
    CHECK_EQ(again["arr"][1].as_string(), std::string("two"));
}

TEST(json_deep_nesting_is_bounded) {
    // A hostile payload must not blow the stack.
    std::string deep(500, '[');
    ParseError pe;
    parse(deep, &pe);
    CHECK(!pe.ok);
}
