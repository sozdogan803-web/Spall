// Locates the folder holding data/ and shaders/ by walking up from the executable.
#pragma once

#include <filesystem>

// Throws std::runtime_error when no such folder exists within eight levels.
std::filesystem::path projectRoot();
