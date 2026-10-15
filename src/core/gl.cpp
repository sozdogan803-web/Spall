#include "core/gl.h"

#include <cstring>

#define BV_GL_DEFINE(ret, name, params) PFN_BV_##name name = nullptr;
BV_GL_FUNCTIONS(BV_GL_DEFINE)
#undef BV_GL_DEFINE

namespace gl {

static void* getProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    // Some drivers return small sentinel values instead of null for unsupported names.
    const auto v = reinterpret_cast<intptr_t>(p);
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) {
        static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
        p = opengl32 ? reinterpret_cast<void*>(GetProcAddress(opengl32, name)) : nullptr;
    }
    return p;
}

bool load(const char** missing) {
#define BV_GL_LOAD(ret, name, params)                                      \
    name = reinterpret_cast<PFN_BV_##name>(getProc(#name));                \
    if (!name) {                                                           \
        if (missing) *missing = #name;                                     \
        return false;                                                      \
    }
    BV_GL_FUNCTIONS(BV_GL_LOAD)
#undef BV_GL_LOAD
    return true;
}

bool hasExtension(const char* name) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (ext && std::strcmp(ext, name) == 0) return true;
    }
    return false;
}

} // namespace gl
