#include "core/sysmem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

SystemMemory systemMemory() {
    SystemMemory m;
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof(s);
    if (GlobalMemoryStatusEx(&s)) {
        m.freeBytes = static_cast<size_t>(s.ullAvailPhys);
        m.totalBytes = static_cast<size_t>(s.ullTotalPhys);
    }
    return m;
}

size_t freeHostBytes() { return systemMemory().freeBytes; }
