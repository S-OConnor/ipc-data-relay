// Minimal self-contained test framework (keeps external dependencies to
// libzmq only, BRG-005).
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    const char* name = nullptr;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
int& failures();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <typename A, typename B>
std::string describe_eq(const char* ea, const char* eb, const A& a, const B& b) {
    std::ostringstream o;
    o << ea << " == " << eb << " (" << a << " vs " << b << ")";
    return o.str();
}

}  // namespace testfw

#define TEST(name)                                                        \
    static void test_##name();                                            \
    static ::testfw::Registrar registrar_##name(#name, test_##name);      \
    static void test_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) ::testfw::report_failure(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                                            \
    do {                                                                                          \
        auto va_ = (a);                                                                           \
        auto vb_ = (b);                                                                           \
        if (!(va_ == vb_)) ::testfw::report_failure(__FILE__, __LINE__, ::testfw::describe_eq(#a, #b, va_, vb_)); \
    } while (0)

#define REQUIRE(cond)                                                     \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ::testfw::report_failure(__FILE__, __LINE__, #cond);          \
            return;                                                       \
        }                                                                 \
    } while (0)
