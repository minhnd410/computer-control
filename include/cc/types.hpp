// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cc {

// ---------------------------------------------------------------------------
// Coordinate spaces
// ---------------------------------------------------------------------------
//
// Getting this wrong is the single most common source of "the click landed
// 40px off" bugs on Retina / fractional-scaling setups, so the space is part
// of every coordinate that crosses the API boundary.
//
//   Logical  - OS-reported points / device-independent pixels. This is the
//              space the input subsystems actually speak (CGEvent on macOS,
//              X11 root coordinates, Win32 physical-after-DPI-awareness).
//              Origin is the top-left of the virtual desktop.
//   Physical - real device pixels. A 2x Retina display reports Logical
//              1512x982 and Physical 3024x1964. Raw captures live here.
//   Image    - pixels of the most recent capture the caller was handed. A
//              capture may have been downscaled to fit a payload budget, or
//              cropped to one display, so this space carries an offset and a
//              scale relative to Physical.
//
// Conversions always go through DisplayGraph, which is the only component
// that knows per-display scale factors and bounds.
enum class Space : std::uint8_t { Logical = 0, Physical = 1, Image = 2 };

const char* to_string(Space s) noexcept;
std::optional<Space> space_from_string(std::string_view s) noexcept;

struct Point {
    double x = 0;
    double y = 0;
    Space space = Space::Logical;

    constexpr Point() = default;
    constexpr Point(double x_, double y_, Space sp = Space::Logical) : x(x_), y(y_), space(sp) {}
};

struct Size {
    double w = 0;
    double h = 0;
    Space space = Space::Logical;

    constexpr Size() = default;
    constexpr Size(double w_, double h_, Space sp = Space::Logical) : w(w_), h(h_), space(sp) {}
};

struct Rect {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    Space space = Space::Logical;

    constexpr Rect() = default;
    constexpr Rect(double x_, double y_, double w_, double h_, Space sp = Space::Logical)
        : x(x_), y(y_), w(w_), h(h_), space(sp) {}

    constexpr double right() const { return x + w; }
    constexpr double bottom() const { return y + h; }
    constexpr Point center() const { return Point{x + w / 2.0, y + h / 2.0, space}; }
    constexpr bool contains(const Point& p) const {
        return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
    }
    bool intersects(const Rect& o) const {
        return !(o.x >= right() || o.right() <= x || o.y >= bottom() || o.bottom() <= y);
    }
    constexpr bool empty() const { return w <= 0 || h <= 0; }
};

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

enum class ErrorCode : std::int32_t {
    Ok = 0,
    InvalidArgument = 1,
    PermissionDenied = 2,  // TCC / uinput / UIAccess not granted
    Unsupported = 3,       // valid request, this platform/session can't do it
    NotFound = 4,          // window, display, element, device
    Timeout = 5,
    Busy = 6,            // e.g. a mouse button is already held
    BackendFailure = 7,  // the OS API itself returned an error
    DeviceError = 8,     // simulator / emulator bridge failure
    IoError = 9,
    Internal = 10,
};

const char* to_string(ErrorCode c) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    // Actionable next step for the caller (and for an LLM driving the server).
    std::string remedy;

    Error() = default;
    Error(ErrorCode c, std::string msg, std::string rem = {})
        : code(c), message(std::move(msg)), remedy(std::move(rem)) {}

    explicit operator bool() const noexcept { return code != ErrorCode::Ok; }
};

// Minimal expected-like result. We avoid std::expected so the project builds
// on the AppleClang / MSVC / GCC baselines listed in the README.
template <typename T>
class Result {
public:
    Result(T value) : store_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Result(Error err) : store_(std::move(err)) {}  // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return store_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() { return std::get<0>(store_); }
    const T& value() const { return std::get<0>(store_); }
    const Error& error() const { return std::get<1>(store_); }

    T value_or(T fallback) const { return ok() ? std::get<0>(store_) : std::move(fallback); }

private:
    std::variant<T, Error> store_;
};

// Void specialisation: either Ok or an Error.
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error err) : err_(std::move(err)) {}  // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return err_.code == ErrorCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const { return err_; }

private:
    Error err_;
};

using Status = Result<void>;

inline Status ok() {
    return Status{};
}
inline Error err(ErrorCode c, std::string m, std::string remedy = {}) {
    return Error{c, std::move(m), std::move(remedy)};
}

}  // namespace cc
