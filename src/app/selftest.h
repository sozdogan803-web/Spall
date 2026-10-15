// `Spall --selftest`: GPU checks of the deformation simulation (see mpm_selftest.h). Prints
// the failures and a PASS/FAIL line.
#pragma once

#include <filesystem>

// Needs a current OpenGL 4.6 context. Returns 0 when every check passes.
int runSelftest(const std::filesystem::path& root);
