#include "test.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>

namespace test {
namespace {
int g_failures = 0;
int g_checks = 0;
bool g_currentFailed = false;
} // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void countCheck() { ++g_checks; }

void fail(const char* file, int line, const std::string& message) {
    ++g_failures;
    g_currentFailed = true;
    std::printf("    FAIL %s:%d  %s\n", file, line, message.c_str());
}

std::string dataDir() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::filesystem::path dir = std::filesystem::path(buf).parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "data" / "projectiles.json")) return (dir / "data").string();
        dir = dir.parent_path();
    }
    return "data";
}

} // namespace test

int main() {
    // Unbuffered, so a crash in a test still shows the output before it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failedCases = 0;
    for (const test::Case& c : test::registry()) {
        test::g_currentFailed = false;
        c.fn();
        std::printf("%s %s\n", test::g_currentFailed ? "[FAIL]" : "[ ok ]", c.name);
        if (test::g_currentFailed) ++failedCases;
    }
    std::printf("\n%zu tests, %d checks, %d failed checks, %d failed tests\n", test::registry().size(),
                test::g_checks, test::g_failures, failedCases);
    return failedCases == 0 ? 0 : 1;
}
