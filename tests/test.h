// Tiny self-registering test harness (no external framework).
#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& message);
void countCheck();
// Directory containing projectiles.json / explosives.json, found by walking up from the exe.
std::string dataDir();

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

} // namespace test

#define TEST(name)                                          \
    static void name();                                     \
    static test::Registrar name##_registrar(#name, name);   \
    static void name()

#define CHECK(expr)                                                        \
    do {                                                                   \
        test::countCheck();                                                \
        if (!(expr)) test::fail(__FILE__, __LINE__, "CHECK(" #expr ")");   \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                  \
    do {                                                                                       \
        test::countCheck();                                                                    \
        const double va_ = (a), vb_ = (b), vt_ = (tol);                                        \
        if (!(std::fabs(va_ - vb_) <= vt_))                                                    \
            test::fail(__FILE__, __LINE__,                                                     \
                       std::string("CHECK_NEAR(" #a ", " #b ") got ") + std::to_string(va_) + \
                           " expected " + std::to_string(vb_) + " +/- " + std::to_string(vt_)); \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                                     \
    do {                                                                                       \
        test::countCheck();                                                                    \
        const std::string va_ = (a), vb_ = (b);                                                \
        if (va_ != vb_)                                                                        \
            test::fail(__FILE__, __LINE__,                                                     \
                       std::string("CHECK_EQ_STR(" #a ", " #b ") got \"") + va_ +              \
                           "\" expected \"" + vb_ + "\"");                                     \
    } while (0)
