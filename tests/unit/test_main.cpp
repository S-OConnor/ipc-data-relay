#include <cstring>

#include "test_framework.hpp"

namespace testfw {
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
int& failures() {
    static int f = 0;
    return f;
}
}  // namespace testfw

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed_tests = 0;
    for (const auto& t : testfw::registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        int before = testfw::failures();
        std::fprintf(stderr, "[ RUN  ] %s\n", t.name);
        t.fn();
        ++run;
        if (testfw::failures() != before) {
            ++failed_tests;
            std::fprintf(stderr, "[ FAIL ] %s\n", t.name);
        } else {
            std::fprintf(stderr, "[  OK  ] %s\n", t.name);
        }
    }
    std::fprintf(stderr, "%d test(s) run, %d failed, %d check(s) failed\n", run, failed_tests, testfw::failures());
    return failed_tests == 0 ? 0 : 1;
}
