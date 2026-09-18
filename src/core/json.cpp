// SPDX-License-Identifier: MIT
#include "core/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cc::json {
namespace {
const Value kNull{};
const Array kEmptyArray{};
const Object kEmptyObject{};
}  // namespace

bool Value::as_bool(bool d) const {
    switch (type_) {
        case Type::Bool: return bool_;
        case Type::Number: return num_ != 0;
        case Type::String: return str_ == "true" || str_ == "1" || str_ == "yes" || str_ == "on";
        case Type::Null: return d;
        default: return d;
    }
}

double Value::as_double(double d) const {
    if (type_ == Type::Number) return num_;
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    if (type_ == Type::String) {
        try {
            return std::stod(str_);
        } catch (...) {
            return d;
        }
    }
    return d;
}

std::int64_t Value::as_int(std::int64_t d) const {
    if (type_ == Type::Number) return static_cast<std::int64_t>(std::llround(num_));
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    if (type_ == Type::String) {
        try {
            return std::stoll(str_);
        } catch (...) {
            return d;
        }
    }
    return d;
}

std::string Value::as_string(std::string d) const {
    if (type_ == Type::String) return str_;
    if (type_ == Type::Null) return d;
    return dump();
}

const Array& Value::as_array() const {
    return type_ == Type::Array_ ? arr_ : kEmptyArray;
}
Array& Value::as_array() {
    if (type_ != Type::Array_) {
        type_ = Type::Array_;
        arr_.clear();
    }
    return arr_;
}
const Object& Value::as_object() const {
    return type_ == Type::Object_ ? obj_ : kEmptyObject;
}
Object& Value::as_object() {
    if (type_ != Type::Object_) {
        type_ = Type::Object_;
        obj_.clear();
    }
    return obj_;
}

const Value& Value::operator[](std::string_view key) const {
    if (type_ != Type::Object_) return kNull;
    auto it = obj_.find(std::string(key));
    return it == obj_.end() ? kNull : it->second;
}

bool Value::contains(std::string_view key) const {
    return type_ == Type::Object_ && obj_.count(std::string(key)) > 0;
}

const Value& Value::operator[](std::size_t i) const {
    if (type_ != Type::Array_ || i >= arr_.size()) return kNull;
    return arr_[i];
}

std::size_t Value::size() const {
    if (type_ == Type::Array_) return arr_.size();
    if (type_ == Type::Object_) return obj_.size();
    if (type_ == Type::String) return str_.size();
    return 0;
}

void Value::push_back(Value v) {
    if (type_ != Type::Array_) {
        type_ = Type::Array_;
        arr_.clear();
    }
    arr_.push_back(std::move(v));
}

void Value::set(std::string key, Value v) {
    if (type_ != Type::Object_) {
        type_ = Type::Object_;
        obj_.clear();
    }
    obj_[std::move(key)] = std::move(v);
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

namespace {
void append_number(std::string& out, double v) {
    if (std::isnan(v) || std::isinf(v)) {
        out += "null";
        return;
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    out += buf;
}
void newline_indent(std::string& out, int indent, int depth) {
    if (indent < 0) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}
}  // namespace

void Value::dump_to(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null: out += "null"; return;
        case Type::Bool: out += bool_ ? "true" : "false"; return;
        case Type::Number: append_number(out, num_); return;
        case Type::String: out += escape(str_); return;
        case Type::Array_: {
            if (arr_.empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            bool first = true;
            for (const auto& v : arr_) {
                if (!first) out.push_back(',');
                first = false;
                newline_indent(out, indent, depth + 1);
                v.dump_to(out, indent, depth + 1);
            }
            newline_indent(out, indent, depth);
            out.push_back(']');
            return;
        }
        case Type::Object_: {
            if (obj_.empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : obj_) {
                if (!first) out.push_back(',');
                first = false;
                newline_indent(out, indent, depth + 1);
                out += escape(k);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                v.dump_to(out, indent, depth + 1);
            }
            newline_indent(out, indent, depth);
            out.push_back('}');
            return;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    out.reserve(256);
    dump_to(out, indent, 0);
    return out;
}

// --------------------------------------------------------------------------
// Parser
// --------------------------------------------------------------------------
namespace {

class Parser {
public:
    Parser(std::string_view t) : t_(t) {}

    Value parse_value(int depth) {
        if (depth > 200) {
            fail("nesting too deep");
            return {};
        }
        skip_ws();
        if (failed_) return {};
        if (pos_ >= t_.size()) {
            fail("unexpected end of input");
            return {};
        }
        char c = t_[pos_];
        switch (c) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': return Value(parse_string());
            case 't': return expect("true") ? Value(true) : Value();
            case 'f': return expect("false") ? Value(false) : Value();
            case 'n': return expect("null") ? Value() : Value();
            default: return parse_number();
        }
    }

    bool failed_ = false;
    std::string error_;
    std::size_t pos_ = 0;

private:
    void fail(std::string m) {
        if (!failed_) {
            failed_ = true;
            error_ = std::move(m);
        }
    }
    void skip_ws() {
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }
    bool expect(std::string_view lit) {
        if (t_.compare(pos_, lit.size(), lit) != 0) {
            fail("invalid literal");
            return false;
        }
        pos_ += lit.size();
        return true;
    }

    void encode_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80)
            out.push_back(char(cp));
        else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    int hex4(std::size_t at) {
        int v = 0;
        for (int i = 0; i < 4; ++i) {
            if (at + i >= t_.size()) return -1;
            char c = t_[at + i];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= c - '0';
            else if (c >= 'a' && c <= 'f')
                v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v |= c - 'A' + 10;
            else
                return -1;
        }
        return v;
    }

    std::string parse_string() {
        std::string out;
        if (pos_ >= t_.size() || t_[pos_] != '"') {
            fail("expected string");
            return out;
        }
        ++pos_;
        while (pos_ < t_.size()) {
            char c = t_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= t_.size()) break;
            char e = t_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    int hi = hex4(pos_);
                    if (hi < 0) {
                        fail("bad \\u escape");
                        return out;
                    }
                    pos_ += 4;
                    std::uint32_t cp = static_cast<std::uint32_t>(hi);
                    // Surrogate pair.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < t_.size() && t_[pos_] == '\\' &&
                        t_[pos_ + 1] == 'u') {
                        int lo = hex4(pos_ + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            pos_ += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (std::uint32_t(lo) - 0xDC00);
                        }
                    }
                    encode_utf8(out, cp);
                    break;
                }
                default: fail("bad escape"); return out;
            }
        }
        fail("unterminated string");
        return out;
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) {
            ++pos_;
            any = true;
        }
        if (pos_ < t_.size() && t_[pos_] == '.') {
            ++pos_;
            while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) {
                ++pos_;
                any = true;
            }
        }
        if (any && pos_ < t_.size() && (t_[pos_] == 'e' || t_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
            while (pos_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[pos_]))) ++pos_;
        }
        if (!any) {
            fail("invalid number");
            return {};
        }
        return Value(std::stod(std::string(t_.substr(start, pos_ - start))));
    }

    Value parse_array(int depth) {
        Array arr;
        ++pos_;  // '['
        skip_ws();
        if (pos_ < t_.size() && t_[pos_] == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        while (pos_ < t_.size()) {
            arr.push_back(parse_value(depth + 1));
            if (failed_) return {};
            skip_ws();
            if (pos_ >= t_.size()) break;
            if (t_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (t_[pos_] == ']') {
                ++pos_;
                return Value(std::move(arr));
            }
            fail("expected ',' or ']'");
            return {};
        }
        fail("unterminated array");
        return {};
    }

    Value parse_object(int depth) {
        Object obj;
        ++pos_;  // '{'
        skip_ws();
        if (pos_ < t_.size() && t_[pos_] == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        while (pos_ < t_.size()) {
            skip_ws();
            std::string key = parse_string();
            if (failed_) return {};
            skip_ws();
            if (pos_ >= t_.size() || t_[pos_] != ':') {
                fail("expected ':'");
                return {};
            }
            ++pos_;
            obj[std::move(key)] = parse_value(depth + 1);
            if (failed_) return {};
            skip_ws();
            if (pos_ >= t_.size()) break;
            if (t_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (t_[pos_] == '}') {
                ++pos_;
                return Value(std::move(obj));
            }
            fail("expected ',' or '}'");
            return {};
        }
        fail("unterminated object");
        return {};
    }

    std::string_view t_;
};

}  // namespace

Value parse(std::string_view text, ParseError* err) {
    Parser p(text);
    Value v = p.parse_value(0);
    if (err) {
        err->ok = !p.failed_;
        err->message = p.error_;
        err->offset = p.pos_;
    }
    return p.failed_ ? Value() : v;
}

}  // namespace cc::json
