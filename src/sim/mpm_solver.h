// GPU solver of the deformation simulation (MPM): 2D plane-strain MLS-MPM with APIC transfers,
// a fixed-point grid (deterministic integer atomics), Johnson-Cook strength with thermal
// softening, failure to a pressure-only fluid, bulk viscosity in compression, and reactive
// layers that initiate, run a detonation front and burn to gamma-law products. Takes the
// initial state from buildMpmScene (mpm_scene.h). Needs a current OpenGL 4.6 context.
#pragma once

#include "core/gl_util.h"
#include "sim/mpm_scene.h"
#include "sim/mpm_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

// Projectile figures are of its body: a dart's fittings (cap, fins, tracer) are left out.
struct MpmMeasure {
    bool valid = false;
    double time = 0;           // s, armour timeline
    double nose = 0;           // m, world x of the intact projectile's leading edge
    double tail = 0;           // m
    double reach = 0;          // m, furthest projectile material, eroded included
    double residualLength = 0; // m, intact projectile area / diameter
    double velocity = 0;       // m/s, mean vx of the intact projectile
    double momentumX = 0, momentumY = 0;  // kg m/s per m of depth, active particles
    double maxSpeed = 0;       // m/s
    double maxTemperature = 0; // K
    double maxStress = 0;      // Pa, von Mises
    double maxPlasticStrain = 0;
    int intact = 0;            // intact projectile particles
    int failedPlate = 0;       // failed plate particles
    int retired = 0;           // particles retired for going non-finite
    int failedStrain = 0, failedSpall = 0, failedMelt = 0;  // projectile particles by failure cause
    int reacted = 0;           // reactive particles lit
    double plateSpeed = 0;     // m/s, fastest intact inert plate material
    int comminuted = 0;        // ceramic particles fully fractured
    int delaminated = 0;       // laminate particles whose matrix has split
    int fibreBroken = 0;       // laminate particles whose fibres have broken
    double leadVelocity = 0;   // m/s, fastest vx of any jet material, failed included
    int jetBroken = 0;         // jet particles past breakup
    bool fuzeTriggered = false; // the round's fuze has triggered
    double fuzeTime = 0;       // s, when it triggered
    int fillerLit = 0;         // filler particles lit
};

// One particle read back from the GPU, for checks.
struct MpmParticleState {
    double x = 0, y = 0;       // m, world
    double vx = 0, vy = 0;     // m/s
    double burn = 0;           // reacted fraction
    double temperature = 0;    // K
    double pressure = 0;       // Pa
    double plasticStrain = 0;
    int material = 0;
    uint32_t flags = 0;
    double fracture = 0;       // ceramic fracture variable D
    double fibreStretch = 1;   // laminate fibre stretch |f|
};

// A copy of the evolving state of a run (particles, detonation front, fuze sums and the step
// count), kept on the GPU, for MpmSolver::restoreState. Good for the scene it was saved from.
class MpmState {
public:
    bool valid() const { return valid_; }
    int steps() const { return steps_; }
    size_t bytes() const;
    void release();  // gives the memory back

private:
    friend class MpmSolver;
    std::vector<gl::Buffer> particles_;  // one per chunk of the store
    gl::Buffer detonation_, fuze_;
    int steps_ = 0;
    bool valid_ = false;
};

class MpmSolver {
public:
    MpmSolver();
    ~MpmSolver();
    MpmSolver(const MpmSolver&) = delete;
    MpmSolver& operator=(const MpmSolver&) = delete;

    // Compiles the shaders under root/shaders/mpm. Throws std::runtime_error on failure.
    void init(const std::filesystem::path& root);
    // Uploads a supported scene and resets to its start. Unsupported scenes unload. The particles
    // are kept in chunks of at most 512 MiB (mpm_store.h); with `limits` the chunks that do not fit
    // free video memory wait in system memory and are streamed through a window of video memory
    // every step, giving the figures a run with all of them resident gives, bit for bit. Without
    // limits everything stays in video memory. Throws std::runtime_error when the plan does not
    // fit or the memory is not there.
    void load(MpmScene scene, const MpmStoreLimits* limits = nullptr);  // by value: a big scene can be moved in
    void unload();
    const MpmStorePlan& storePlan() const { return plan_; }
    bool loaded() const { return loaded_; }
    const MpmScene& scene() const { return scene_; }
    // Back to the scene's initial state.
    void reset();
    void step();
    // Steps until time() >= t, or until about budgetMs of GPU time (the step cost measured with
    // timer queries over earlier calls) or of wall time is used; true when caught up.
    bool advanceTo(double t, double budgetMs);
    // GPU milliseconds per step over the recent calls of advanceTo (0 until one is timed).
    double gpuMsPerStep() const { return msPerStep_; }
    double time() const { return scene_.tStart + steps_ * scene_.dt; }
    // Lets the simulation run on to time t, past the scene's own end (the domain stays).
    void extendTo(double t) { scene_.tEnd = std::max(scene_.tEnd, t); }
    int steps() const { return steps_; }
    // Reduces the figures on the GPU and reads them back (stalls until the GPU is done).
    MpmMeasure measure();
    // The same without stalling: requestMeasure reduces the figures now (unless a request is
    // pending) and pollMeasure returns them, as of the request, once the GPU has finished.
    void requestMeasure();
    bool pollMeasure(MpmMeasure& out);
    // Scales every yield stress (1 = material data, 0 = hydrodynamic). Takes effect next step.
    void setStrengthScale(double s) { strength_ = s; }
    // Starts the detonation front of plate layer `layer` now, from world (x, y) in the initial
    // configuration (for checks; normally the first particle to initiate starts it).
    void ignite(double x, double y, int layer);
    // Reads every particle back (stalls until the GPU is done).
    std::vector<MpmParticleState> readParticles() const;

    // The bytes saveState copies (0 until a scene is loaded, and while chunks wait in system
    // memory: copies of that would cost as much system memory).
    size_t stateBytes() const;
    // Copies the evolving state into `out` on the GPU; false, leaving `out` empty, when there is
    // no memory for it.
    bool saveState(MpmState& out) const;
    // Goes back to a state saved from this scene: the steps that follow are the ones the run took
    // the first time, bit for bit (the step is deterministic), so a run can be scrubbed back
    // and played on without starting again. False when it is not one of this scene's.
    bool restoreState(const MpmState& state);

    // For the renderer and the snapshots: the particles in chunks (layout of a particle in
    // shaders/mpm/mpm_common.glsl); chunk i holds particles chunkFirst(i) .. + chunkSize(i) - 1 of
    // the scene.
    int chunkCount() const { return static_cast<int>(chunks_.size()); }
    size_t chunkFirst(int i) const { return chunks_[static_cast<size_t>(i)].first; }
    size_t chunkSize(int i) const { return chunks_[static_cast<size_t>(i)].count; }
    // The buffer chunk i can be read from now: its own, or for one that waits in system memory the
    // window of video memory after the chunk has been copied into it (valid until the next call).
    const gl::Buffer& chunkForReading(int i);
    int particleCount() const { return static_cast<int>(scene_.particles.size()); }
    double massRef() const { return massRef_; }

private:
    struct Chunk {
        gl::Buffer buf;  // video memory, or system memory when spilled
        size_t first = 0, count = 0;
        bool spilled = false;
    };
    gl::Program p2g_, gridUpdate_, g2p_, measure_;
    gl::Buffer grid_, gridVelocity_, materials_, partials_, detonation_, fuze_, window_;
    std::vector<Chunk> chunks_;
    MpmStorePlan plan_;
    int windowHolds_ = -1;  // the chunk the window holds a current copy of
    MpmScene scene_;
    bool loaded_ = false;
    int steps_ = 0;
    double massRef_ = 1, velRef_ = 4000, strength_ = 1;
    // GPU timestamps around the steps of recent advanceTo calls, collected without stalling.
    struct StepTimer {
        GLuint begin = 0, end = 0;
        int steps = 0;
        bool pending = false;
    };
    std::array<StepTimer, 4> timers_;
    double msPerStep_ = 0;
    GLsync measureFence_ = nullptr;
    double measureTime_ = 0;

    void setCommon(const gl::Program& p) const;
    void setChunk(const gl::Program& p, const Chunk& c) const;
    void allocateStore();
    void loadChunk(size_t i);   // chunk i is bound for reading at 0
    void storeChunk(size_t i);  // chunk i, written in the window, goes back to system memory
    void collectTimers();
    void dispatchMeasure();
    MpmMeasure readMeasure(double time);
    void dropMeasure();
};
