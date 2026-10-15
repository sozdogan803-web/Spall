// Frames of a pre-rendered deformation simulation (src/render/mpm_snapshots.h): when to take
// them, how many fit a memory budget, and which pair a playback time falls between.
#pragma once

#include "sim/mpm_scene.h"
#include "sim/mpm_solver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// n even times from t0 to t1, both ends included (at least two).
inline std::vector<double> mpmSnapshotTimes(double t0, double t1, int n) {
    n = std::max(n, 2);
    std::vector<double> t(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) t[static_cast<size_t>(i)] = t0 + (t1 - t0) * i / (n - 1);
    return t;
}

// Video memory the frames may take: half of what is free (a gigabyte when that is unknown), but
// at least a gigabyte and at most four.
inline size_t mpmSnapshotBudget(size_t freeBytes) {
    constexpr size_t kGiB = size_t(1) << 30;
    return std::clamp<size_t>(freeBytes / 2, kGiB, 4 * kGiB);
}

// Frames of `particles` particles of `bytesPerParticle` that fit `budget` bytes, at most
// `maxFrames` and never fewer than both ends (0 without particles).
inline int mpmSnapshotFrames(int particles, size_t bytesPerParticle, size_t budget, int maxFrames) {
    if (particles <= 0) return 0;
    const size_t frame = static_cast<size_t>(particles) * bytesPerParticle;
    return static_cast<int>(std::clamp<size_t>(budget / frame, 2, static_cast<size_t>(std::max(maxFrames, 2))));
}

// A pre-render's frames at the finest qualities: a frame of 80 million particles is 3 GB, so each
// keeps one particle of every `stride` consecutive ones (picked by a hash, shaders/mpm/pack.comp,
// so the kept ones do not line up with the scene's rows) and the frames that do not fit video
// memory wait in system memory, two cache slots of video memory showing the pair being played.
inline size_t mpmFrameParticles(size_t particles, int stride) {
    const size_t s = static_cast<size_t>(std::max(stride, 1));
    return (particles + s - 1) / s;
}

// System memory the frames may take: half of what is free, at most 16 GiB, and nothing when under
// 2 GiB is free or the amount is not known.
inline size_t mpmSnapshotHostBudget(size_t freeHostBytes) {
    constexpr size_t kGiB = size_t(1) << 30;
    if (freeHostBytes < 2 * kGiB) return 0;
    return std::min<size_t>(freeHostBytes / 2, 16 * kGiB);
}

constexpr int kMpmSnapshotMaxStride = 32;
constexpr int kMpmSnapshotCacheSlots = 2;

struct MpmSnapshotPlan {
    int stride = 1;                // every stride-th particle is kept
    int frames = 0;
    int gpuFrames = 0;             // the first ones live in video memory; the rest in system memory
    size_t frameParticles = 0;
    size_t frameBytes = 0;
    int hostFrames() const { return frames - gpuFrames; }
};

// The finest frames (the smallest stride, so every particle when it can be) that still make at
// least `minFrames`, from the video budget `gpuBudget` and the system budget `hostBudget`, at most
// `maxFrames`, none over `maxFrameParticles` particles, never fewer than both ends.
inline MpmSnapshotPlan mpmPlanSnapshots(size_t particles, size_t bytesPerParticle, size_t gpuBudget, size_t hostBudget,
                                        int minFrames, int maxFrames, size_t maxFrameParticles) {
    MpmSnapshotPlan best;
    if (particles == 0) return best;
    maxFrames = std::max(maxFrames, 2);
    for (int stride = 1; stride <= kMpmSnapshotMaxStride; ++stride) {
        MpmSnapshotPlan p;
        p.stride = stride;
        p.frameParticles = mpmFrameParticles(particles, stride);
        p.frameBytes = p.frameParticles * bytesPerParticle;
        const size_t gpuCap = p.frameBytes ? gpuBudget / p.frameBytes : 0;
        const size_t hostCap = p.frameBytes ? hostBudget / p.frameBytes : 0;
        size_t frames = std::min<size_t>(gpuCap, static_cast<size_t>(maxFrames));
        size_t gpu = frames;
        if (gpuCap < static_cast<size_t>(maxFrames) && hostCap > 0 && gpuCap > kMpmSnapshotCacheSlots) {
            const size_t held = gpuCap - kMpmSnapshotCacheSlots;  // the cache slots take the rest
            const size_t both = std::min<size_t>(held + hostCap, static_cast<size_t>(maxFrames));
            if (both > frames) {
                frames = both;
                gpu = std::min(held, both);
            }
        }
        if (frames < 2) {
            frames = 2;
            gpu = 2;
        }
        p.frames = static_cast<int>(frames);
        p.gpuFrames = static_cast<int>(gpu);
        best = p;
        if (p.frameParticles <= maxFrameParticles && p.frames >= std::min(minFrames, maxFrames)) break;
    }
    return best;
}

// The frames a playback time falls between, a + lerp towards b (a = b at either end; a = -1
// without frames).
struct MpmFramePick {
    int a = -1, b = -1;
    double lerp = 0;
};
inline MpmFramePick mpmPickFrame(const std::vector<double>& times, double t) {
    MpmFramePick p;
    if (times.empty()) return p;
    const int last = static_cast<int>(times.size()) - 1;
    if (t <= times.front()) return {0, 0, 0};
    if (t >= times.back()) return {last, last, 0};
    const int b = static_cast<int>(std::upper_bound(times.begin(), times.end(), t) - times.begin());
    p.a = b - 1;
    p.b = b;
    const double span = times[static_cast<size_t>(b)] - times[static_cast<size_t>(p.a)];
    p.lerp = span > 0 ? (t - times[static_cast<size_t>(p.a)]) / span : 0;
    return p;
}

// Checkpoints let a live run scrub backwards without starting over (src/sim/mpm_checkpoints.h):
// how many copies of the solver's state fit, when to take them, and which one to resume from.
//
// Slots of `bytes` each that fit a quarter of the free video memory (a quarter of nothing when
// that is unknown, so the floor), between 256 MiB and 2 GiB, at most `maxSlots` (0 when one does
// not fit, or there is nothing to copy).
inline int mpmCheckpointSlots(size_t bytes, size_t freeBytes, int maxSlots) {
    constexpr size_t kMiB = size_t(1) << 20;
    if (bytes == 0 || maxSlots <= 0) return 0;
    const size_t budget = std::clamp<size_t>(freeBytes / 4, 256 * kMiB, 2048 * kMiB);
    return static_cast<int>(std::min<size_t>(budget / bytes, static_cast<size_t>(maxSlots)));
}

// n even times strictly between t0 and t1 (none for an empty run).
inline std::vector<double> mpmCheckpointTimes(double t0, double t1, int n) {
    std::vector<double> t;
    if (n <= 0 || !(t1 > t0)) return t;
    t.reserve(static_cast<size_t>(n));
    for (int i = 1; i <= n; ++i) t.push_back(t0 + (t1 - t0) * i / (n + 1));
    return t;
}

// The latest checkpoint taken at or before t, give or take `tol` (a fraction of a step, for the
// solver stops half a step either side of a planned time); -1 when none is, so start again.
inline int mpmPickCheckpoint(const std::vector<double>& taken, double t, double tol) {
    const int n = static_cast<int>(std::upper_bound(taken.begin(), taken.end(), t + tol) - taken.begin());
    return n - 1;
}

// A half-precision float (IEEE 754 binary16), as GLSL's unpackHalf2x16 reads it.
inline float mpmHalfToFloat(uint16_t h) {
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else
        v = std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
    return sign ? -v : v;
}

// One particle of a pre-rendered frame as the GPU stores it (shaders/mpm/frame.glsl, 40 bytes).
struct MpmFrameWords {
    float x[2];  // cells
    uint32_t v, F01, F23, flags, matBurnFrac, FzzTemp, epsStress, X0;
};
static_assert(sizeof(MpmFrameWords) == 40, "must match struct Frame in shaders/mpm/frame.glsl");

// The fields a frame keeps, in world units (the rest of the state is zero).
inline MpmParticleState mpmUnpackFrame(const MpmFrameWords& f, const MpmScene& scene) {
    MpmParticleState s;
    s.x = scene.originX + static_cast<double>(f.x[0]) * scene.dx;
    s.y = scene.originY + static_cast<double>(f.x[1]) * scene.dx;
    s.vx = mpmHalfToFloat(static_cast<uint16_t>(f.v & 0xFFFFu));
    s.vy = mpmHalfToFloat(static_cast<uint16_t>(f.v >> 16));
    s.flags = f.flags;
    s.material = static_cast<int>(f.matBurnFrac & 255u);
    s.burn = static_cast<double>((f.matBurnFrac >> 8) & 255u) / 255.0;
    s.fracture = static_cast<double>((f.matBurnFrac >> 16) & 255u) / 255.0;
    s.temperature = mpmHalfToFloat(static_cast<uint16_t>(f.FzzTemp >> 16));
    s.plasticStrain = mpmHalfToFloat(static_cast<uint16_t>(f.epsStress & 0xFFFFu));
    return s;
}

// Adds a measurement to a run's log, which stays in time order and at least minSpacing apart:
// a measurement from earlier than the last (the run went back) first drops what came after it, and
// one at the last's own time is the same measurement again. Measurements that are not valid are
// ignored.
inline void mpmLogMeasure(std::vector<MpmMeasure>& log, const MpmMeasure& m, double minSpacing) {
    if (!m.valid) return;
    while (!log.empty() && log.back().time > m.time) log.pop_back();
    if (!log.empty() && log.back().time == m.time) return;  // the same measurement again
    if (log.empty() || m.time - log.back().time >= minSpacing) log.push_back(m);
}
