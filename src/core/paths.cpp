#include "core/paths.h"

#include "core/gl.h"

#include <stdexcept>

std::filesystem::path projectRoot() {
    wchar_t executable[32768];
    const DWORD size = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!size || size >= std::size(executable)) throw std::runtime_error("Cannot determine the executable path");
    std::filesystem::path dir = std::filesystem::path(executable).parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "data" / "projectiles.json") && std::filesystem::exists(dir / "shaders"))
            return dir;
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    throw std::runtime_error("Cannot find data/projectiles.json and shaders/ above the executable directory");
}
