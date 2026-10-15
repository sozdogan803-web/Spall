// GPU checks of the deformation simulation, run by `Spall --selftest` (or `--selftest mpm`
// alone): free flight keeps momentum and moves v t, the strengthless (hydrodynamic) crater depth
// is near L sqrt(rho_p / rho_t), a thin plate is perforated, a thick block is not, and nothing
// goes non-finite. Needs a current OpenGL 4.6 context. Returns the failures.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::vector<std::string> runMpmSelftest(const std::filesystem::path& root);
