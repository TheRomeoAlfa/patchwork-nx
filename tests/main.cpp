#include <cstdlib>
#include <cstring>

#include "test.hpp"

// MM_TEST=<text> runs only the tests whose names contain it.
int main() {
    int failedCases = 0;
    const char* only = std::getenv("MM_TEST");
    for (const auto& c : t::registry()) {
        if (only && !std::strstr(c.name, only)) continue;
        int before = t::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
            ++t::failures();
        }
        bool ok = t::failures() == before;
        if (!ok) ++failedCases;
        std::cout << (ok ? "ok   " : "FAIL ") << c.name << std::endl;  // flushed: a hang shows where
    }
    std::cout << "\n" << t::registry().size() - failedCases << "/" << t::registry().size() << " passed\n";
    return failedCases ? 1 : 0;
}
