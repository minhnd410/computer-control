// SPDX-License-Identifier: MIT
#include <sstream>

#include "cc/permissions.hpp"

namespace cc {

std::string permission_guidance() {
    const auto all = check_permissions();

    std::ostringstream os;
    bool any_problem = false;

    for (const auto& s : all) {
        if (s.state == PermissionState::Granted || s.state == PermissionState::NotRequired) {
            continue;
        }
        any_problem = true;
        os << to_string(s.permission) << ": " << to_string(s.state) << "\n";
        if (!s.detail.empty()) os << "  " << s.detail << "\n";
        if (!s.affects.empty()) {
            os << "  affects: ";
            for (std::size_t i = 0; i < s.affects.size(); ++i) {
                if (i) os << ", ";
                os << s.affects[i];
            }
            os << "\n";
        }
        if (!s.remedy.empty()) {
            // The remedy is usually multi-line and is the only part worth
            // reading, so indent it rather than collapsing it.
            std::istringstream lines(s.remedy);
            std::string line;
            while (std::getline(lines, line)) os << "  " << line << "\n";
        }
        os << "\n";
    }

    if (!any_problem) return {};
    return os.str();
}

}  // namespace cc
