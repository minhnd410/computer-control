// SPDX-License-Identifier: MIT
#pragma once

// A ~60 line test harness. A real framework would be a build dependency on
// three platforms for a project whose whole selling point is not having any.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline std::string& current() {
    static std::string c;
    return c;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        cases().push_back({name, std::move(fn)});
    }
};

inline void report(bool ok, const char* expr, const char* file, int line,
                   const std::string& extra) {
    if (ok) return;
    ++failures();
    std::printf("  FAIL %s:%d\n    %s\n", file, line, expr);
    if (!extra.empty()) std::printf("    %s\n", extra.c_str());
}

inline int run_all() {
    int passed = 0;
    for (auto& c : cases()) {
        current() = c.name;
        const int before = failures();
        std::printf("%s\n", c.name.c_str());
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++failures();
            std::printf("  FAIL threw: %s\n", e.what());
        } catch (...) {
            ++failures();
            std::printf("  FAIL threw an unknown exception\n");
        }
        if (failures() == before) ++passed;
    }
    std::printf("\n%d/%zu cases passed, %d assertion failure(s)\n", passed, cases().size(),
                failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace test

#define TEST(name)                                    \
    static void name();                               \
    static ::test::Registrar reg_##name(#name, name); \
    static void name()

#define CHECK(expr) ::test::report((expr), #expr, __FILE__, __LINE__, "")

#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        auto _a = (a);                                                  \
        auto _b = (b);                                                  \
        ::test::report(_a == _b, #a " == " #b, __FILE__, __LINE__, ""); \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                \
    do {                                                                                     \
        const double _a = static_cast<double>(a);                                            \
        const double _b = static_cast<double>(b);                                            \
        char _buf[128];                                                                      \
        std::snprintf(_buf, sizeof(_buf), "got %g, want %g (+/- %g)", _a, _b,                \
                      static_cast<double>(eps));                                             \
        ::test::report(std::fabs(_a - _b) <= (eps), #a " ~= " #b, __FILE__, __LINE__, _buf); \
    } while (0)
