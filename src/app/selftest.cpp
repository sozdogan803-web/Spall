#include "app/selftest.h"

#include "app/mpm_selftest.h"
#include "core/log.h"

#include <cstdio>
#include <string>
#include <vector>

int runSelftest(const std::filesystem::path& root) {
    logx::setEcho(false);
    const std::vector<std::string> failures = runMpmSelftest(root);
    if (failures.empty()) {
        std::printf("selftest: PASS\n");
        return 0;
    }
    for (const std::string& f : failures) std::printf("FAIL mpm: %s\n", f.c_str());
    std::printf("selftest: %zu failure(s)\n", failures.size());
    return 1;
}
