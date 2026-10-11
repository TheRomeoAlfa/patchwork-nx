#pragma once
// Minimal test harness: no external dependency so it builds anywhere.

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

// Fresh temporary directory, removed when the object goes out of scope.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("mmtest-" + std::to_string(rd()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace t

#define MM_CAT2(a, b) a##b
#define MM_CAT(a, b) MM_CAT2(a, b)
#define TEST(name)                                                    \
    static void MM_CAT(test_, __LINE__)();                            \
    static t::Register MM_CAT(reg_, __LINE__)(name, MM_CAT(test_, __LINE__)); \
    static void MM_CAT(test_, __LINE__)()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n";     \
            ++t::failures();                                                              \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        auto _va = (a);                                                                                   \
        auto _vb = (b);                                                                                   \
        if (!(_va == _vb)) {                                                                              \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n    got: " << _va \
                      << "\n    want: " << _vb << "\n";                                                   \
            ++t::failures();                                                                              \
        }                                                                                                 \
    } while (0)
