#pragma once
// Minimal dependency-free test framework (no external deps needed).

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& FailCount() {
    static int failures = 0;
    return failures;
}

inline int& AssertCount() {
    static int asserts = 0;
    return asserts;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        Registry().push_back({std::move(name), std::move(fn)});
    }
};

inline void Fail(const char* file, int line, const std::string& message) {
    ++FailCount();
    std::printf("    FAIL %s:%d  %s\n", file, line, message.c_str());
}

inline int RunAll() {
    for (auto& test : Registry()) {
        std::printf("[ RUN  ] %s\n", test.name.c_str());
        try {
            test.fn();
            std::printf("[  OK  ] %s\n", test.name.c_str());
        } catch (const std::exception& ex) {
            ++FailCount();
            std::printf("[ FAIL ] %s  unexpected exception: %s\n", test.name.c_str(), ex.what());
        } catch (...) {
            ++FailCount();
            std::printf("[ FAIL ] %s  unexpected unknown exception\n", test.name.c_str());
        }
    }
    std::printf("------------------------------------------------------------\n");
    std::printf("%d checks, %d failures\n", AssertCount(), FailCount());
    return FailCount() == 0 ? 0 : 1;
}

} // namespace testfw

#define TEST(name)                                                                                  \
    static void test_##name();                                                                      \
    static ::testfw::Registrar registrar_##name(#name, test_##name);                                \
    static void test_##name()

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        ++::testfw::AssertCount();                                                                  \
        if (!(cond)) {                                                                              \
            ::testfw::Fail(__FILE__, __LINE__, #cond);                                              \
        }                                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++::testfw::AssertCount();                                                                  \
        auto lhs_ = (a);                                                                            \
        auto rhs_ = (b);                                                                            \
        if (!(lhs_ == rhs_)) {                                                                      \
            ::testfw::Fail(__FILE__, __LINE__,                                                      \
                           std::string(#a " == " #b) + " (lhs=" + std::to_string(lhs_) +            \
                               ", rhs=" + std::to_string(rhs_) + ")");                              \
        }                                                                                           \
    } while (0)

#define CHECK_THROWS(expr)                                                                          \
    do {                                                                                            \
        ++::testfw::AssertCount();                                                                  \
        bool caught_ = false;                                                                       \
        try {                                                                                       \
            (void)(expr);                                                                           \
        } catch (...) {                                                                             \
            caught_ = true;                                                                         \
        }                                                                                           \
        if (!caught_) {                                                                             \
            ::testfw::Fail(__FILE__, __LINE__, "expected exception: " #expr);                       \
        }                                                                                           \
    } while (0)

#define CHECK_THROWS_AS(expr, ExType)                                                               \
    do {                                                                                            \
        ++::testfw::AssertCount();                                                                  \
        bool caught_ = false;                                                                       \
        try {                                                                                       \
            (void)(expr);                                                                           \
        } catch (const ExType&) {                                                                   \
            caught_ = true;                                                                         \
        } catch (...) {                                                                             \
        }                                                                                           \
        if (!caught_) {                                                                             \
            ::testfw::Fail(__FILE__, __LINE__, "expected " #ExType ": " #expr);                     \
        }                                                                                           \
    } while (0)
