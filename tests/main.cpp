#include "test.hpp"

int main() {
    int failedCases = 0;
    for (const auto& c : t::registry()) {
        int before = t::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
            ++t::failures();
        }
        bool ok = t::failures() == before;
        if (!ok) ++failedCases;
        std::cout << (ok ? "ok   " : "FAIL ") << c.name << "\n";
    }
    std::cout << "\n" << t::registry().size() - failedCases << "/" << t::registry().size() << " passed\n";
    return failedCases ? 1 : 0;
}
