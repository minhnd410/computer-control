// SPDX-License-Identifier: MIT
#include "cc/types.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace cc {

const char* to_string(Space s) noexcept {
    switch (s) {
        case Space::Logical: return "logical";
        case Space::Physical: return "physical";
        case Space::Image: return "image";
    }
    return "logical";
}

std::optional<Space> space_from_string(std::string_view s) noexcept {
    if (s == "logical" || s == "points" || s == "pt") return Space::Logical;
    if (s == "physical" || s == "pixels" || s == "px" || s == "device") return Space::Physical;
    if (s == "image" || s == "screenshot") return Space::Image;
    return std::nullopt;
}

const char* to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::InvalidArgument: return "invalid_argument";
        case ErrorCode::PermissionDenied: return "permission_denied";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::Busy: return "busy";
        case ErrorCode::BackendFailure: return "backend_failure";
        case ErrorCode::DeviceError: return "device_error";
        case ErrorCode::IoError: return "io_error";
        case ErrorCode::Internal: return "internal";
    }
    return "internal";
}

}  // namespace cc
