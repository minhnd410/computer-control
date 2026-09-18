// SPDX-License-Identifier: MIT
#pragma once

// A small, dependency-free JSON value. Enough for MCP framing, the action
// dispatcher's JSON-shaped returns, and config parsing. Deliberately not a
// general-purpose library: no comments, no trailing commas, no arbitrary-
// precision numbers.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cc::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

enum class Type : std::uint8_t { Null = 0, Bool, Number, String, Array_, Object_ };

class Value {
public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}                           // NOLINT
    Value(bool b) : type_(Type::Bool), bool_(b) {}                         // NOLINT
    Value(int v) : type_(Type::Number), num_(v) {}                         // NOLINT
    Value(long v) : type_(Type::Number), num_(double(v)) {}                // NOLINT
    Value(long long v) : type_(Type::Number), num_(double(v)) {}           // NOLINT
    Value(unsigned v) : type_(Type::Number), num_(v) {}                    // NOLINT
    Value(unsigned long long v) : type_(Type::Number), num_(double(v)) {}  // NOLINT
    Value(double v) : type_(Type::Number), num_(v) {}                      // NOLINT
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}        // NOLINT
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}      // NOLINT
    Value(std::string_view s) : type_(Type::String), str_(s) {}            // NOLINT
    Value(Array a) : type_(Type::Array_), arr_(std::move(a)) {}            // NOLINT
    Value(Object o) : type_(Type::Object_), obj_(std::move(o)) {}          // NOLINT

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array_; }
    bool is_object() const noexcept { return type_ == Type::Object_; }

    bool as_bool(bool d = false) const;
    double as_double(double d = 0) const;
    std::int64_t as_int(std::int64_t d = 0) const;
    std::string as_string(std::string d = {}) const;

    const Array& as_array() const;
    Array& as_array();
    const Object& as_object() const;
    Object& as_object();

    // Object access. Missing keys read as Null rather than throwing, because
    // most callers want a default.
    // Read-only by design. Mutation goes through set()/push_back()/as_object(),
    // which keeps `v["a"][0]` unambiguous: a non-const string overload makes an
    // integer index ambiguous against it for a non-const Value.
    const Value& operator[](std::string_view key) const;
    const Value& at(std::string_view key) const { return (*this)[key]; }
    bool contains(std::string_view key) const;

    const Value& operator[](std::size_t i) const;
    const Value& operator[](int i) const { return (*this)[static_cast<std::size_t>(i)]; }
    std::size_t size() const;

    void push_back(Value v);
    void set(std::string key, Value v);

    std::string dump(int indent = -1) const;

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    Array arr_;
    Object obj_;
};

struct ParseError {
    bool ok = true;
    std::string message;
    std::size_t offset = 0;
};

Value parse(std::string_view text, ParseError* err = nullptr);
std::string escape(std::string_view s);

}  // namespace cc::json
