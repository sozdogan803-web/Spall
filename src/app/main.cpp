#include "app/app.h"
#include "app/selftest.h"
#include "core/gl.h"
#include "core/gl_util.h"
#include "core/log.h"
#include "core/paths.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

// On a machine with an integrated and a discrete GPU the driver picks the integrated one for a
// program that does not ask. These exported flags are the documented request for the discrete
// card (NVIDIA Optimus / AMD PowerXpress); the million-particle tiers need its memory and speed.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// Command-line modes print to the parent console (the exe uses the Windows subsystem).
static void attachParentConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
    }
}

int main(int argc, char** argv) {
    bool glInfo = false, selftest = false;
    AppOptions appOptions;
    int windowWidth = 1280, windowHeight = 800;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (std::strcmp(argv[i], "--gl-info") == 0) glInfo = true;
        else if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;
        else if (std::strcmp(argv[i], "--capture") == 0 && more) appOptions.capture = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && more) appOptions.frames = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--armour-mpm") == 0) appOptions.armourMpm = true;
        else if (std::strcmp(argv[i], "--prerender") == 0) appOptions.armourMpm = appOptions.prerender = true;
        else if (std::strcmp(argv[i], "--mpm-colour") == 0 && more) appOptions.mpmColour = argv[++i];
        else if (std::strcmp(argv[i], "--mpm-quality") == 0 && more) appOptions.mpmQuality = argv[++i];
        else if (std::strcmp(argv[i], "--mpm-soft-mounts") == 0) appOptions.mpmSoftMounts = true;
        else if (std::strcmp(argv[i], "--projectile") == 0 && more) appOptions.projectile = argv[++i];
        else if (std::strcmp(argv[i], "--preset") == 0 && more) appOptions.preset = argv[++i];
        else if (std::strcmp(argv[i], "--at") == 0 && more) appOptions.armourAt = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--obliquity") == 0 && more) appOptions.obliquity = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--theme") == 0 && more) appOptions.theme = argv[++i];
        else if (std::strcmp(argv[i], "--size") == 0 && more) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
                windowWidth = w;
                windowHeight = h;
                appOptions.sizeGiven = true;
            }
        }
    }
    const bool headless = glInfo || selftest || !appOptions.capture.empty();
    if (headless) attachParentConsole();

    if (!glfwInit()) {
        MessageBoxA(nullptr, "GLFW could not be initialised.", "Spall", MB_ICONERROR);
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // Hidden until runApp has applied the size and position saved in spall.ini.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "Spall", nullptr, nullptr);
    if (!window) {
        MessageBoxA(nullptr, "This computer's graphics driver does not provide OpenGL 4.6.", "Spall", MB_ICONERROR);
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    const char* missing = nullptr;
    if (!gl::load(&missing)) {
        std::printf("missing GL entry point: %s\n", missing);
        return 1;
    }
    if (glInfo) {
        std::printf("GL_VENDOR   %s\n", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
        std::printf("GL_RENDERER %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        std::printf("GL_VERSION  %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        std::printf("GLSL        %s\n", reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
        std::printf("MAX_SSBO_BLOCK_SIZE %.2f GB\n", static_cast<double>(gl::maxStorageBlockBytes()) / 1073741824.0);
        if (gl::hasExtension("GL_NVX_gpu_memory_info")) {
            GLint kb = 0;
            glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &kb);
            std::printf("VRAM        %d MB\n", kb / 1024);
        }
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
    if (selftest) {
        int code = 1;
        try {
            code = runSelftest(projectRoot());
        } catch (const std::exception& e) {
            std::printf("selftest: %s\n", e.what());
        }
        std::fflush(stdout);
        glfwDestroyWindow(window);
        glfwTerminate();
        return code;
    }
    int code = 1;
    try {
        code = runApp(window, projectRoot(), appOptions);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Spall", MB_ICONERROR);
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return code;
}
