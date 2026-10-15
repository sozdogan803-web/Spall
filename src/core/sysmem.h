// How much of the computer's own memory (not the graphics card's) is free: the deformation
// simulation spills particles that do not fit video memory into it (src/sim/mpm_store.h).
#pragma once

#include <cstddef>

struct SystemMemory {
    size_t freeBytes = 0;   // physical memory a program can take now
    size_t totalBytes = 0;  // physical memory installed
};

// Both are 0 when the system does not say.
SystemMemory systemMemory();
size_t freeHostBytes();
