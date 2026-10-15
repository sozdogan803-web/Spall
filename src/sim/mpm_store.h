// Where the particles of a deformation simulation live (src/sim/mpm_solver.cpp): the whole set is
// cut into equal chunks of at most 512 MiB, each its own storage buffer, because one buffer is
// limited by the card (often 2 GiB) and a 100-million-particle set is 14 GB. The chunks that fit
// the free video memory stay there; the rest wait in system memory (the CPU's side of the machine)
// and are streamed through one chunk-sized window of video memory every step. This planner only
// does the sums, so it can be tested without a graphics card.
#pragma once

#include "sim/mpm_scene.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

constexpr size_t kMpmChunkBytes = size_t(512) << 20;  // the most one chunk holds
constexpr size_t kMpmChunkAlign = 256;                // particles: a chunk is whole measure groups
constexpr double kMpmVideoShare = 0.85;               // of the free video memory the scene may take
constexpr double kMpmHostShare = 0.6;                 // of the free system memory the spill may take
constexpr size_t kMpmGridBufferBytesPerNode = 4 * sizeof(int32_t);

struct MpmStoreLimits {
    size_t freeVideoBytes = 0;  // free (plus what the scene being replaced gives back); 0: unknown
    size_t freeHostBytes = 0;   // free system memory; 0: unknown, so nothing can be spilled
    size_t maxBlockBytes = 0;   // the most the card allows in one storage buffer; 0: unknown
    bool allowSpill = true;     // may chunks wait in system memory
    size_t maxChunkBytes = 0;   // cut chunks smaller than the 512 MiB default (0: the default)
};

struct MpmStorePlan {
    bool ok = false;
    std::string problem;                // why not, when !ok
    size_t particles = 0;
    size_t chunkParticles = 0;          // particles in every chunk but possibly the last
    int chunks = 0;
    int residentChunks = 0;             // the first ones stay in video memory
    int spilledChunks = 0;              // the rest wait in system memory
    size_t videoBytes = 0;              // resident particles, window, grid and readout partials
    size_t hostBytes = 0;               // the spilled particles
    size_t windowBytes = 0;             // the streaming window (0 when nothing is spilled)

    size_t chunkFirst(int i) const { return static_cast<size_t>(i) * chunkParticles; }
    size_t chunkCount(int i) const { return std::min(chunkParticles, particles - chunkFirst(i)); }
    bool spilled(int i) const { return i >= residentChunks; }
};

inline MpmStorePlan mpmPlanStore(size_t particles, int nx, int ny, const MpmStoreLimits& lim) {
    constexpr double kGB = 1073741824.0;
    char buf[200];
    MpmStorePlan p;
    p.particles = particles;
    const size_t nodes = static_cast<size_t>(std::max(nx, 0)) * static_cast<size_t>(std::max(ny, 0));

    if (lim.maxBlockBytes > 0 && nodes * kMpmGridBufferBytesPerNode > lim.maxBlockBytes) {
        std::snprintf(buf, sizeof(buf), "needs a %.1f GB grid buffer and the card allows %.1f GB in one",
                      static_cast<double>(nodes * kMpmGridBufferBytesPerNode) / kGB,
                      static_cast<double>(lim.maxBlockBytes) / kGB);
        p.problem = buf;
        return p;
    }
    size_t perChunk = kMpmChunkBytes;
    if (lim.maxChunkBytes > 0) perChunk = std::min(perChunk, lim.maxChunkBytes);
    if (lim.maxBlockBytes > 0) perChunk = std::min(perChunk, lim.maxBlockBytes);
    perChunk = perChunk / kMpmParticleBytes / kMpmChunkAlign * kMpmChunkAlign;
    if (perChunk == 0) {
        p.problem = "the card allows too small a storage buffer for the particles";
        return p;
    }
    if (particles == 0) {
        p.ok = true;
        return p;
    }
    const size_t padded = (particles + kMpmChunkAlign - 1) / kMpmChunkAlign * kMpmChunkAlign;
    p.chunkParticles = std::min(perChunk, padded);
    p.chunks = static_cast<int>((particles + p.chunkParticles - 1) / p.chunkParticles);

    const size_t total = mpmVideoBytes(particles, nx, ny);
    const size_t fixed = total - particles * kMpmParticleBytes;  // grid and readout partials
    const double usable = lim.freeVideoBytes > 0 ? kMpmVideoShare * static_cast<double>(lim.freeVideoBytes) : 0.0;

    if (lim.freeVideoBytes == 0 || static_cast<double>(total) <= usable) {
        p.ok = true;
        p.residentChunks = p.chunks;
        p.videoBytes = total;
        return p;
    }
    if (!lim.allowSpill) {
        std::snprintf(buf, sizeof(buf), "needs about %.1f GB of video memory and %.1f GB is free",
                      static_cast<double>(total) / kGB, static_cast<double>(lim.freeVideoBytes) / kGB);
        p.problem = buf;
        return p;
    }

    // Keep as many chunks resident as the card holds beside the grid and one streaming window.
    std::vector<size_t> bytes(static_cast<size_t>(p.chunks));
    for (int i = 0; i < p.chunks; ++i) bytes[static_cast<size_t>(i)] = p.chunkCount(i) * kMpmParticleBytes;
    std::vector<size_t> suffixMax(static_cast<size_t>(p.chunks) + 1, 0);  // the largest chunk from i on
    for (int i = p.chunks - 1; i >= 0; --i)
        suffixMax[static_cast<size_t>(i)] = std::max(suffixMax[static_cast<size_t>(i) + 1], bytes[static_cast<size_t>(i)]);
    std::vector<size_t> prefix(static_cast<size_t>(p.chunks) + 1, 0);  // bytes of the chunks before i
    for (int i = 0; i < p.chunks; ++i) prefix[static_cast<size_t>(i) + 1] = prefix[static_cast<size_t>(i)] + bytes[static_cast<size_t>(i)];

    int resident = -1;
    for (int r = p.chunks - 1; r >= 0; --r) {
        const size_t need = fixed + prefix[static_cast<size_t>(r)] + suffixMax[static_cast<size_t>(r)];
        if (static_cast<double>(need) <= usable) {
            resident = r;
            break;
        }
    }
    if (resident < 0) {
        std::snprintf(buf, sizeof(buf), "needs at least %.1f GB of video memory even streaming the particles and %.1f GB is free",
                      static_cast<double>(fixed + suffixMax[0]) / kGB, static_cast<double>(lim.freeVideoBytes) / kGB);
        p.problem = buf;
        return p;
    }
    p.residentChunks = resident;
    p.spilledChunks = p.chunks - resident;
    p.windowBytes = suffixMax[static_cast<size_t>(resident)];
    p.hostBytes = prefix[static_cast<size_t>(p.chunks)] - prefix[static_cast<size_t>(resident)];
    p.videoBytes = fixed + prefix[static_cast<size_t>(resident)] + p.windowBytes;

    if (lim.freeHostBytes == 0 || static_cast<double>(p.hostBytes) > kMpmHostShare * static_cast<double>(lim.freeHostBytes)) {
        std::snprintf(buf, sizeof(buf), "needs about %.1f GB of system memory beside the video memory and %.1f GB is free",
                      static_cast<double>(p.hostBytes) / kGB, static_cast<double>(lim.freeHostBytes) / kGB);
        p.problem = buf;
        p.ok = false;
        return p;
    }
    p.ok = true;
    return p;
}
