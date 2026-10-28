#pragma once
// Minimal self-registering test framework (no external dependency).
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace altest {

struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
inline const char*& current() { static const char* c = ""; return c; }

struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

inline void fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::printf("  FAIL [%s] %s:%d: %s\n", current(), file, line, msg.c_str());
}

} // namespace altest

#define AL_CAT2(a, b) a##b
#define AL_CAT(a, b) AL_CAT2(a, b)
#define TEST(name) \
    static void AL_CAT(test_fn_, name)(); \
    static altest::Reg AL_CAT(test_reg_, name)(#name, AL_CAT(test_fn_, name)); \
    static void AL_CAT(test_fn_, name)()

#define CHECK(cond) do { ++altest::checks(); if (!(cond)) altest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)
#define CHECK_MSG(cond, ...) do { ++altest::checks(); if (!(cond)) { char _b[512]; std::snprintf(_b, sizeof(_b), __VA_ARGS__); altest::fail(__FILE__, __LINE__, _b); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++altest::checks(); double _a = double(a), _b = double(b); if (!(std::fabs(_a - _b) <= double(tol))) { char _m[256]; std::snprintf(_m, sizeof(_m), "%s = %.6f, expected %.6f +- %.6f", #a, _a, _b, double(tol)); altest::fail(__FILE__, __LINE__, _m); } } while (0)
#define REQUIRE(cond) do { ++altest::checks(); if (!(cond)) { altest::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")"); return; } } while (0)
