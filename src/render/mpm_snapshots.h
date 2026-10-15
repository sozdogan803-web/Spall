// A pre-rendered deformation simulation: the solver's particles packed into frames at even times
// (src/sim/mpm_frames.h, shaders/mpm/frame.glsl), kept on the GPU so playback can run at any
// speed and scrub freely, with the readouts of each frame. The view (src/render/mpm_view.h)
// draws a time between two frames with the positions interpolated.
//
// At the finest qualities a frame of every particle would be gigabytes, so a frame keeps one
// particle of every `stride` (shaders/mpm/pack.comp) and the frames that do not fit video memory
// wait in system memory, two slots of video memory showing the pair being played.
#pragma once

#include "core/gl_util.h"
#include "sim/mpm_frames.h"
#include "sim/mpm_solver.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

// What a pre-render may take, and how many frames it should make.
struct MpmSnapshotLimits {
    size_t gpuBytes = 0;   // video memory
    size_t hostBytes = 0;  // system memory (0 keeps every frame in video memory)
    int minFrames = 90;    // the stride grows until this many frames fit (or the stride limit)
    int maxFrames = 240;
    size_t maxFrameParticles = 0;  // 0 = what one frame's shader binding can hold, at most 16 million
};

class MpmSnapshots {
public:
    static constexpr size_t kBytesPerParticle = 40;  // Frame (shaders/mpm/frame.glsl)
    static constexpr int kMaxFrames = 240;

    // The limits of this machine now: half the free video memory (a gigabyte at least, four at
    // most) and, with `useSystemMemory`, half the free system memory (at most 16 GiB).
    static MpmSnapshotLimits limitsForThisMachine(bool useSystemMemory);

    void init(const std::filesystem::path& root);  // throws if the shader does not compile
    // Allocates frames for t0..t1 for the solver's scene; the solver should be at or before t0.
    // Nothing is active afterwards when even two frames do not fit.
    void begin(const MpmSolver& solver, double t0, double t1, const MpmSnapshotLimits& limits);
    // Packs the solver's particles into every frame whose time it has reached (within half a
    // step, as MpmSolver::advanceTo stops); true once the last frame is in.
    bool capture(MpmSolver& solver);
    void clear();

    bool active() const { return !times_.empty(); }
    bool complete() const { return active() && taken_ == static_cast<int>(times_.size()); }
    double progress() const { return active() ? static_cast<double>(taken_) / times_.size() : 0; }
    // The next frame's time (the solver should advance to it).
    double nextTime() const { return complete() ? times_.back() : times_[static_cast<size_t>(taken_)]; }
    int frames() const { return static_cast<int>(times_.size()); }
    const MpmSnapshotPlan& plan() const { return plan_; }
    int stride() const { return plan_.stride; }
    // Particles in a frame (slot j keeps one of the scene's particles j * stride ...).
    size_t frameParticles() const { return plan_.frameParticles; }
    // The readouts at the last frame at or before t; reach is the furthest up to then.
    MpmMeasure measureAt(double t) const;
    // The readouts of every frame taken so far, in time order (for export).
    std::vector<MpmMeasure> measures() const;
    // The particles of the frame measureAt(t) reports, read back from the GPU or system memory
    // (the fields a frame keeps, one in `stride` of the scene's; empty without frames). Slow:
    // for occasional readouts, not every frame.
    std::vector<MpmParticleState> readFrame(double t, const MpmScene& scene) const;

    // Binds the frames around time t to SSBO bindings 7 and 8 (copying them up from system
    // memory when they are not in video memory); returns the interpolation weight towards the
    // second.
    float bind(double t);

private:
    int frameAt(double t) const;
    void pack(MpmSolver& solver, size_t slot);
    size_t slotOffset(size_t slot) const { return slot * slotBytes_; }
    // The video-memory slot frame `f` can be read from, loading it into a cache slot (not the
    // one holding `keep`) when it waits in system memory.
    size_t slotOf(int f, int keep);

    gl::Program pack_;
    MpmSnapshotPlan plan_;
    std::vector<double> times_;  // planned, replaced by the solver's time as each is taken
    std::vector<MpmMeasure> measures_;
    int taken_ = 0;
    int count_ = 0;              // particles of the scene
    size_t slotBytes_ = 0;       // a frame's slot in the store, aligned for binding
    gl::Buffer store_;           // the video-memory frames, then the cache slots
    std::vector<std::unique_ptr<uint8_t[]>> host_;  // the frames from plan_.gpuFrames on
    int cached_[kMpmSnapshotCacheSlots] = {-1, -1};  // the frame in each cache slot
    int cacheNext_ = 0;
};
