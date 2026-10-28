#include "test.hpp"

#include <chrono>
#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (auto& c : altest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        altest::current() = c.name;
        int before = altest::failures();
        auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
        } catch (const std::exception& e) {
            altest::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        } catch (...) {
            altest::fail(__FILE__, __LINE__, "unknown exception");
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s %-40s %8.1f ms\n", altest::failures() == before ? "[ ok ]" : "[FAIL]", c.name, ms);
        ++run;
    }
    std::printf("\n%d tests, %d checks, %d failures\n", run, altest::checks(), altest::failures());
    return altest::failures() == 0 ? 0 : 1;
}
