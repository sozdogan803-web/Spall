#include "app/mpm_selftest.h"

#include "core/sysmem.h"
#include "data/projectile_db.h"
#include "render/mpm_snapshots.h"
#include "sim/armour_run.h"
#include "sim/mpm_checkpoints.h"
#include "sim/mpm_solver.h"
#include "sim/mpm_store.h"
#include "sim/witness_readout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Tungsten rod, 10 mm x 60 mm at 2000 m/s, against a stack.
struct Setup {
    ArmourCase c;
    ArmourOutcome out;
    MpmScene scene;
};

Setup rodCase(const ProjectileDb& db, const std::vector<PresetLayer>& layers, double obliquity, MpmQuality q) {
    Setup s;
    s.c.penetrator.kind = phys::PenetratorKind::LongRod;
    s.c.penetrator.diameter = 0.01;
    s.c.penetrator.length = 0.06;
    s.c.penetrator.density = 17600;
    s.c.velocity = 2000;
    s.c.obliquityDeg = obliquity;
    s.c.layers = layers;
    s.out = evaluateArmour(db, s.c);
    s.scene = buildMpmScene(db, s.c, RodMaterial::Tungsten, phys::shapeForCase(nullptr, s.c.penetrator), s.out, q);
    return s;
}

// One RHA plate at 0 deg.
Setup rodCase(const ProjectileDb& db, double plate, MpmQuality q) {
    return rodCase(db, {{db.findMaterial("rha"), plate, 0}}, 0, q);
}

// The generic reactive sandwich: 4 mm RHA / 4 mm sheet explosive / 4 mm RHA.
Setup sandwichCase(const ProjectileDb& db, double obliquity, MpmQuality q) {
    const int rha = db.findMaterial("rha"), ex = db.findMaterial("sheet_explosive");
    return rodCase(db, {{rha, 0.004, 0}, {ex, 0.004, 0}, {rha, 0.004, 0}}, obliquity, q);
}

// Generic shaped charge: 80 mm copper cone, q = 0.5, at a standoff in cone diameters, against
// one RHA plate.
Setup jetCase(const ProjectileDb& db, double standoff, double plate, MpmQuality q) {
    Setup s;
    s.c.penetrator.kind = phys::PenetratorKind::ShapedCharge;
    s.c.penetrator.coneDiameter = 0.08;
    s.c.penetrator.standoff = standoff;
    s.c.penetrator.jetQuality = 0.5;
    s.c.penetrator.linerDensity = 8930;
    s.c.layers = {{db.findMaterial("rha"), plate, 0}};
    s.out = evaluateArmour(db, s.c);
    s.scene = buildMpmScene(db, s.c, RodMaterial::Tungsten, phys::ProjectileShape{}, s.out, q);
    return s;
}

void dropProjectile(MpmScene& scene) {
    scene.particles.erase(scene.particles.begin(), scene.particles.begin() + scene.projectileCount);
    scene.projectileCount = scene.bodyCount = 0;
}

// Initial distance of a plate particle along its face from where the shot line crosses the
// plate's mid-plane, m.
double alongFace(const Setup& s, const MpmSeed& seed) {
    const double th = s.c.obliquityDeg * kPi / 180;
    const phys::LayerResult& l = s.out.result.layers[seed.layer];
    const double thick = (l.back - l.front) * std::cos(th);
    return -(seed.x - l.front) * std::sin(th) + seed.y * std::cos(th) + 0.5 * thick * std::tan(th);
}

// Mean speed of the cover plates' particles (layers 0 and 2) whose initial distance along the
// face lies in [lo, hi], m/s.
double plateSpeed(const Setup& s, const std::vector<MpmParticleState>& ps, double lo, double hi) {
    double sum = 0;
    int n = 0;
    for (size_t k = 0; k < ps.size() && k < s.scene.particles.size(); ++k) {
        const MpmSeed& seed = s.scene.particles[k];
        if ((seed.flags & kMpmProjectile) || seed.layer == 1 || (ps[k].flags & kMpmInactive)) continue;
        const double d = std::abs(alongFace(s, seed));
        if (d < lo || d > hi) continue;
        sum += std::hypot(ps[k].vx, ps[k].vy);
        ++n;
    }
    return n ? sum / n : 0;
}

// Fraction of a layer's particles whose initial distance along the face lies in [lo, hi] and
// that carry `flag`.
double flaggedFraction(const Setup& s, const std::vector<MpmParticleState>& ps, int layer, double lo, double hi,
                       unsigned flag) {
    int n = 0, flagged = 0;
    for (size_t k = 0; k < ps.size() && k < s.scene.particles.size(); ++k) {
        const MpmSeed& seed = s.scene.particles[k];
        if ((seed.flags & (kMpmProjectile | kMpmAnchored)) || seed.layer != layer) continue;
        const double d = std::abs(alongFace(s, seed));
        if (d < lo || d > hi) continue;
        ++n;
        if (ps[k].flags & flag) ++flagged;
    }
    return n ? static_cast<double>(flagged) / n : 0;
}

// How far plate layer b has moved away from plate layer a across the faces: the mean normal
// displacement of b's particles minus a's, both whose initial distance along the face lies in
// [lo, hi], m (positive: the plates have moved apart).
double separation(const Setup& s, const std::vector<MpmParticleState>& ps, int a, int b, double lo, double hi) {
    const double th = s.c.obliquityDeg * kPi / 180, nx = std::cos(th), ny = std::sin(th);
    double sum[2] = {0, 0};
    int n[2] = {0, 0};
    for (size_t k = 0; k < ps.size() && k < s.scene.particles.size(); ++k) {
        const MpmSeed& seed = s.scene.particles[k];
        if ((seed.flags & (kMpmProjectile | kMpmAnchored)) || (ps[k].flags & kMpmInactive)) continue;
        if (seed.layer != a && seed.layer != b) continue;
        const double d = std::abs(alongFace(s, seed));
        if (d < lo || d > hi) continue;
        const int i = seed.layer == a ? 0 : 1;
        sum[i] += (ps[k].x - seed.x) * nx + (ps[k].y - seed.y) * ny;
        ++n[i];
    }
    return n[0] && n[1] ? sum[1] / n[1] - sum[0] / n[0] : 0;
}

void row(const char* name, const MpmSolver& solver, const MpmMeasure& m, double seconds) {
    std::printf("   mpm %-12s %7d particles, %5d steps, t %6.1f us, nose %6.1f mm, reach %6.1f mm, residual %5.1f mm, "
                "v %6.0f m/s, max T %5.0f K, failed %d/%d/%d, %.1f s wall\n",
                name, solver.particleCount(), solver.steps(), m.time * 1e6, m.nose * 1e3, m.reach * 1e3,
                m.residualLength * 1e3, m.velocity, m.maxTemperature, m.failedStrain,
                m.failedSpall, m.failedMelt, seconds);
}

void jetRow(const char* name, const MpmSolver& solver, const MpmMeasure& m, double seconds) {
    std::printf("   mpm %-12s %7d particles, %5d steps, t %6.1f us, reach %6.1f mm, fastest jet %5.0f m/s, broken %d, "
                "max T %5.0f K, failed %d/%d/%d, %.1f s wall\n",
                name, solver.particleCount(), solver.steps(), m.time * 1e6, m.reach * 1e3, m.leadVelocity, m.jetBroken,
                m.maxTemperature, m.failedStrain, m.failedSpall, m.failedMelt, seconds);
}

double runTo(MpmSolver& solver, double t) {
    const auto start = std::chrono::steady_clock::now();
    // SPALL_MPM_TRACE=n prints the figures every n steps.
    const char* traceEnv = std::getenv("SPALL_MPM_TRACE");
    const int every = traceEnv ? std::atoi(traceEnv) : 0;
    if (every > 0) {
        while (solver.time() + 0.5 * solver.scene().dt < t) {
            solver.step();
            if (solver.steps() % every == 0) row("trace", solver, solver.measure(), 0);
        }
    }
    solver.advanceTo(t, 1e12);
    glFinish();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// A catalogue round against a preset, at the preset's obliquity.
Setup catalogueCase(const ProjectileDb& db, const char* projectile, const char* preset, MpmQuality q) {
    Setup s;
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(db.findProjectile(projectile))];
    s.c.penetrator = e.penetrator;
    s.c.velocity = e.velocity;
    for (const ArmourPreset& p : db.presets())
        if (p.id == preset) {
            s.c.obliquityDeg = p.obliquityDeg;
            s.c.layers = p.layers;
        }
    s.out = evaluateArmour(db, s.c);
    s.scene = buildMpmScene(db, s.c, e.rodMaterial, phys::shapeForCase(e.shape.present ? &e.shape : nullptr, e.penetrator),
                            s.out, q);
    return s;
}

// Runs a fuzed round to t in 5 us legs; returns the first measured time any filler was lit
// (1e30: never), the furthest reach of the projectile on the way, and leaves the last figures in m.
double runFuzed(MpmSolver& solver, double t, MpmMeasure& m, double& wall, double* maxReach = nullptr) {
    double firstLit = 1e30;
    m = solver.measure();
    if (maxReach) *maxReach = m.reach;
    while (solver.time() + 0.5 * solver.scene().dt < t) {
        wall += runTo(solver, std::min(t, solver.time() + 5e-6));
        m = solver.measure();
        if (m.fillerLit > 0 && firstLit > 1e29) firstLit = m.time;
        if (maxReach) *maxReach = std::max(*maxReach, m.reach);
    }
    return firstLit;
}

void fuzeRow(const char* name, const MpmSolver& solver, const MpmMeasure& m, double firstLit, double seconds) {
    std::printf("   mpm %-12s %7d particles, %5d steps, t %6.1f us, nose %6.1f mm, reach %6.1f mm, fuze %s at %6.1f us, "
                "filler lit %d first at %6.1f us, retired %d, %.1f s wall\n",
                name, solver.particleCount(), solver.steps(), m.time * 1e6, m.nose * 1e3, m.reach * 1e3,
                m.fuzeTriggered ? "triggered" : "quiet", m.fuzeTime * 1e6, m.fillerLit,
                firstLit > 1e29 ? -1.0 : firstLit * 1e6, m.retired, seconds);
}

bool sameNumber(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }

// How many of b's particles are not bit for bit a's (-1: the sets are not the same size).
long differingParticles(const std::vector<MpmParticleState>& a, const std::vector<MpmParticleState>& b) {
    if (a.size() != b.size()) return -1;
    long differ = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const MpmParticleState& p = a[i];
        const MpmParticleState& q = b[i];
        if (!(sameNumber(p.x, q.x) && sameNumber(p.y, q.y) && sameNumber(p.vx, q.vx) && sameNumber(p.vy, q.vy) &&
              sameNumber(p.burn, q.burn) && sameNumber(p.temperature, q.temperature) &&
              sameNumber(p.pressure, q.pressure) && sameNumber(p.plasticStrain, q.plasticStrain) &&
              p.material == q.material && p.flags == q.flags && sameNumber(p.fracture, q.fracture) &&
              sameNumber(p.fibreStretch, q.fibreStretch)))
            ++differ;
    }
    return differ;
}

bool sameMeasure(const MpmMeasure& m1, const MpmMeasure& m2) {
    return m1.reacted == m2.reacted && m1.fillerLit == m2.fillerLit && m1.fuzeTriggered == m2.fuzeTriggered &&
           sameNumber(m1.fuzeTime, m2.fuzeTime) && m1.failedPlate == m2.failedPlate && m1.intact == m2.intact &&
           sameNumber(m1.nose, m2.nose) && sameNumber(m1.reach, m2.reach) && sameNumber(m1.velocity, m2.velocity) &&
           sameNumber(m1.maxStress, m2.maxStress) && sameNumber(m1.maxSpeed, m2.maxSpeed) &&
           sameNumber(m1.plateSpeed, m2.plateSpeed);
}

// 19. Checkpoints: sent back to one and played on, the run is the one it took the first time, bit for
// bit, with the reactive layer's detonation front and the fuze's sums and all. The run is planned to
// kEnd, played to kLook, sent back to kBack and played to kLook again.
void checkpointRun(const char* name, const Setup& s, MpmSolver& solver, double kEnd, double kLook, double kBack,
                   std::vector<std::string>& failures) {
    solver.load(s.scene);
    MpmCheckpoints ck;
    ck.begin(solver, kEnd);
    // Steps to t, stopping at each checkpoint due on the way (as the armour view does).
    auto advance = [&](double t) {
        for (;;) {
            const double stop = std::min(t, ck.nextTime());
            solver.advanceTo(stop, 1e12);
            ck.capture(solver, 0);
            if (stop >= t) break;
        }
    };
    advance(kLook);
    const std::vector<MpmParticleState> first = solver.readParticles();
    const MpmMeasure m1 = solver.measure();
    const int stepsFirst = solver.steps();
    const int taken = ck.taken();
    double reach = 0;
    const bool restored = ck.restore(solver, kBack, reach);
    const double backTime = solver.time();
    const int backSteps = solver.steps();
    double tooEarly = 0;
    const bool earlyRestored = ck.restore(solver, solver.scene().tStart + 0.5 * solver.scene().dt, tooEarly);
    if (earlyRestored) ck.restore(solver, kBack, reach);  // back to where the check goes on from
    advance(kLook);
    const std::vector<MpmParticleState> again = solver.readParticles();
    const MpmMeasure m2 = solver.measure();

    const long differ = differingParticles(first, again);
    const bool measuresMatch = sameMeasure(m1, m2);
    std::printf("   mpm checkpoints %-5s %d taken of %d planned (%.1f MB each); back to %.1f us (step %d of %d), played "
                "on to %.1f us: %ld of %zu particles differ, lit %d+%d/%d+%d, fuze %s\n",
                name, taken, ck.planned(), static_cast<double>(solver.stateBytes()) / (1 << 20), backTime * 1e6,
                backSteps, stepsFirst, solver.time() * 1e6, differ, first.size(), m2.reacted, m2.fillerLit, m1.reacted,
                m1.fillerLit, m1.fuzeTriggered ? "triggered" : "quiet");
    const std::string who = std::string("checkpoints (") + name + "): ";
    if (taken < 2) failures.push_back(who + "only " + std::to_string(taken) + " checkpoints were taken on the way");
    if (!restored || !(backTime <= kBack + 0.5 * solver.scene().dt) || !(backSteps < stepsFirst))
        failures.push_back(who + "a checkpoint did not send the run back to before the time asked");
    if (earlyRestored) failures.push_back(who + "a checkpoint was found before the first one was taken");
    if (m1.reacted + m1.fillerLit == 0) failures.push_back(who + "the run never lit anything, so it compared nothing");
    if (solver.steps() != stepsFirst) failures.push_back(who + "the replay ended at another step than the first run");
    if (differ != 0)
        failures.push_back(who + "the replay differed from the first run in " + std::to_string(differ) + " particles");
    if (!measuresMatch) failures.push_back(who + "the replay read out differently from the first run");
    if (m2.retired) failures.push_back(who + "the replay went non-finite");
}

void checkpointCheck(const ProjectileDb& db, MpmSolver& solver, std::vector<std::string>& failures) {
    // The reactive sandwich: the detonation front is part of the state.
    checkpointRun("era", sandwichCase(db, 60, MpmQuality::Medium), solver, 30e-6, 24e-6, 12e-6, failures);
    // An APHE round through 50 mm RHA: the fuze triggers at about 97 us and the filler lights at
    // about 1100 us, so the run goes back to between the two.
    checkpointRun("aphe", catalogueCase(db, "88mm_pzgr39_kwk36", "rha_50", MpmQuality::Medium), solver, 1500e-6,
                  1200e-6, 400e-6, failures);
}

// Limits that cut the scene into about `chunks` chunks and keep `spill` of them in system memory
// (the real free system memory stands behind them), by searching the video memory the planner is told.
bool forcedLimits(const MpmScene& sc, int chunks, int spill, MpmStoreLimits& lim) {
    const size_t n = sc.particles.size();
    const size_t per = (n + static_cast<size_t>(chunks) - 1) / static_cast<size_t>(chunks);
    const size_t perAligned = (per + kMpmChunkAlign - 1) / kMpmChunkAlign * kMpmChunkAlign;
    lim = MpmStoreLimits{};
    lim.maxBlockBytes = gl::maxStorageBlockBytes();
    lim.maxChunkBytes = perAligned * kMpmParticleBytes;
    lim.freeHostBytes = freeHostBytes();
    if (spill == 0) return true;  // video memory unknown: everything stays resident
    const size_t step = std::max<size_t>(size_t(1) << 12, perAligned * kMpmParticleBytes / 8);  // under one chunk
    for (size_t freeV = step; freeV < (size_t(1) << 38); freeV += step) {
        lim.freeVideoBytes = freeV;
        const MpmStorePlan plan = mpmPlanStore(n, sc.nx, sc.ny, lim);
        if (plan.ok && plan.spilledChunks <= spill) return plan.spilledChunks == spill;
    }
    return false;
}

// 20. Segmented store: a scene cut into chunks, some of them waiting in system memory and streamed
// through video memory every step, runs as the whole scene in one buffer does, bit for bit (the grid
// is fixed-point integers, so the order the particles are visited in cannot show). The reactive
// layer's detonation front and the fuze's sums are part of that.
void segmentRun(const char* name, const Setup& s, MpmSolver& solver, double tEnd, std::vector<std::string>& failures) {
    struct Variant {
        const char* label;
        int chunks, spill;
    };
    const Variant variants[] = {{"one chunk", 1, 0}, {"5 chunks in video memory", 5, 0}, {"5 chunks, 2 in system memory", 5, 2}};
    std::vector<MpmParticleState> reference;
    MpmMeasure refMeasure;
    int refSteps = 0;
    for (const Variant& v : variants) {
        const std::string who = std::string("segments (") + name + ", " + v.label + "): ";
        MpmStoreLimits lim;
        if (v.chunks > 1 && !forcedLimits(s.scene, v.chunks, v.spill, lim)) {
            failures.push_back(who + "the planner could not be made to cut the scene that way");
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (v.chunks > 1)
            solver.load(s.scene, &lim);
        else
            solver.load(s.scene);
        const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const MpmStorePlan& plan = solver.storePlan();
        const double wall = runTo(solver, tEnd);
        const std::vector<MpmParticleState> ps = solver.readParticles();
        const MpmMeasure m = solver.measure();
        MpmCheckpoints ck;
        ck.begin(solver, tEnd);
        long differ = 0;
        bool measuresMatch = true;
        if (reference.empty()) {
            reference = ps;
            refMeasure = m;
            refSteps = solver.steps();
        } else {
            differ = differingParticles(reference, ps);
            measuresMatch = sameMeasure(refMeasure, m);
        }
        std::printf("   mpm segments %-5s %-30s %zu particles in %d chunks (%d spilled, %.1f MB in system memory), loaded in "
                    "%.1f s, %d steps in %.1f s, %ld differ, lit %d+%d, fuze %s, %d checkpoints\n",
                    name, v.label, plan.particles, plan.chunks, plan.spilledChunks, static_cast<double>(plan.hostBytes) / (1 << 20),
                    loadSeconds, solver.steps(), wall, differ, m.reacted, m.fillerLit, m.fuzeTriggered ? "triggered" : "quiet",
                    ck.planned());
        if (v.chunks == 1 ? plan.chunks != 1 : plan.chunks < 3)
            failures.push_back(who + "the scene was cut into " + std::to_string(plan.chunks) + " chunks");
        if (plan.spilledChunks != v.spill)
            failures.push_back(who + std::to_string(plan.spilledChunks) + " chunks were spilled, wanted " + std::to_string(v.spill));
        if (m.reacted + m.fillerLit == 0) failures.push_back(who + "the run never lit anything, so it compared nothing");
        if (solver.steps() != refSteps) failures.push_back(who + "ended at another step than the one-chunk run");
        if (differ != 0)
            failures.push_back(who + "differed from the one-chunk run in " + std::to_string(differ) + " particles");
        if (!measuresMatch) failures.push_back(who + "read out differently from the one-chunk run");
        if (m.retired) failures.push_back(who + "went non-finite");
        if (v.spill > 0 && ck.planned() != 0)
            failures.push_back(who + "planned checkpoints, which a spilled run cannot copy");
    }
}

void segmentCheck(const ProjectileDb& db, MpmSolver& solver, std::vector<std::string>& failures) {
    segmentRun("era", sandwichCase(db, 60, MpmQuality::Medium), solver, 24e-6, failures);
    segmentRun("aphe", catalogueCase(db, "88mm_pzgr39_kwk36", "rha_50", MpmQuality::Medium), solver, 1200e-6, failures);
}

// 21. Pre-rendered frames of a scene cut into chunks, some of them (and some of the frames) waiting in
// system memory, are the frames of the one-chunk run bit for bit: the particle each block of three
// keeps, every frame, whether it is read back from video or system memory, and as the view's bindings
// see it after the frames are loaded into the cache slots.
struct FrameDump {
    MpmSnapshotPlan plan;
    std::vector<double> times;
    std::vector<MpmMeasure> measures;
    std::vector<std::vector<MpmParticleState>> read;  // readFrame at each frame's time
    long boundWrong = 0;                              // frames the view's bindings 7 and 8 did not hold
    long weightWrong = 0;                             // between-frame weights that were not the time's fraction
};

// What the frame bound to a storage-buffer binding point holds (empty when nothing is bound).
std::vector<MpmParticleState> boundFrame(GLuint binding, const MpmScene& sc) {
    constexpr GLenum kBinding = 0x90D3, kStart = 0x90D4, kSize = 0x90D5;  // GL_SHADER_STORAGE_BUFFER_*
    GLint id = 0, start = 0, size = 0;
    glGetIntegeri_v(kBinding, binding, &id);
    glGetIntegeri_v(kStart, binding, &start);
    glGetIntegeri_v(kSize, binding, &size);
    std::vector<MpmParticleState> out;
    if (id == 0 || size <= 0) return out;
    std::vector<MpmFrameWords> raw(static_cast<size_t>(size) / sizeof(MpmFrameWords));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(id));
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, start, size, raw.data());
    out.reserve(raw.size());
    for (const MpmFrameWords& f : raw) out.push_back(mpmUnpackFrame(f, sc));
    return out;
}

FrameDump captureFrames(MpmSolver& solver, MpmSnapshots& snaps, double tEnd, const std::string& who,
                        std::vector<std::string>& failures) {
    FrameDump d;
    const MpmScene& sc = solver.scene();
    const size_t n = static_cast<size_t>(solver.particleCount());
    // Frames of one particle in three: eight of them, the video budget holding five slots (three frames
    // and the two cache slots) so the other five wait in system memory.
    MpmSnapshotLimits lim;
    lim.minFrames = lim.maxFrames = 8;
    lim.maxFrameParticles = mpmFrameParticles(n, 3);
    lim.gpuBytes = 5 * lim.maxFrameParticles * MpmSnapshots::kBytesPerParticle;
    lim.hostBytes = size_t(1) << 30;
    snaps.begin(solver, sc.tStart, tEnd, lim);
    if (!snaps.active()) {
        failures.push_back(who + "the frames could not be allocated");
        return d;
    }
    d.plan = snaps.plan();
    for (int guard = 0; !snaps.capture(solver); ++guard) {
        if (guard > 100000) {
            failures.push_back(who + "the frames were never all taken");
            return d;
        }
        solver.advanceTo(snaps.nextTime(), 1e12);
    }
    d.measures = snaps.measures();
    for (const MpmMeasure& m : d.measures) {
        d.times.push_back(m.time);
        d.read.push_back(snaps.readFrame(m.time, sc));
    }
    // The view's bindings, frame by frame forwards, then backwards (the cache slots change hands).
    const int last = snaps.frames() - 1;
    auto look = [&](int i) {
        const float w = snaps.bind(d.times[static_cast<size_t>(i)]);
        if (w != 0.0f) ++d.weightWrong;
        if (differingParticles(boundFrame(7, sc), d.read[static_cast<size_t>(i)]) != 0) ++d.boundWrong;
        // At the first frame's time the picker returns that frame twice, as it does at the last's.
        const size_t b = static_cast<size_t>(i == 0 ? 0 : std::min(i + 1, last));
        if (differingParticles(boundFrame(8, sc), d.read[b]) != 0) ++d.boundWrong;
    };
    for (int i = 0; i <= last; ++i) look(i);
    for (int i = last; i >= 0; --i) look(i);
    // Halfway between two frames both are bound and the weight is one half.
    for (int i = 0; i < last; ++i) {
        const double mid = 0.5 * (d.times[static_cast<size_t>(i)] + d.times[static_cast<size_t>(i) + 1]);
        if (std::abs(snaps.bind(mid) - 0.5f) > 1e-3f) ++d.weightWrong;
        if (differingParticles(boundFrame(7, sc), d.read[static_cast<size_t>(i)]) != 0) ++d.boundWrong;
        if (differingParticles(boundFrame(8, sc), d.read[static_cast<size_t>(i) + 1]) != 0) ++d.boundWrong;
    }
    return d;
}

void snapshotRun(const char* name, const Setup& s, MpmSolver& solver, MpmSnapshots& snaps, double tEnd,
                 std::vector<std::string>& failures) {
    struct Variant {
        const char* label;
        int chunks, spill;
    };
    const Variant variants[] = {{"one chunk", 1, 0}, {"5 chunks, 2 in system memory", 5, 2}};
    FrameDump ref;
    for (const Variant& v : variants) {
        const std::string who = std::string("snapshots (") + name + ", " + v.label + "): ";
        MpmStoreLimits lim;
        if (v.chunks > 1 && !forcedLimits(s.scene, v.chunks, v.spill, lim)) {
            failures.push_back(who + "the planner could not be made to cut the scene that way");
            continue;
        }
        if (v.chunks > 1)
            solver.load(s.scene, &lim);
        else
            solver.load(s.scene);
        const auto t0 = std::chrono::steady_clock::now();
        const FrameDump d = captureFrames(solver, snaps, tEnd, who, failures);
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const size_t n = static_cast<size_t>(solver.particleCount());
        long differ = 0;
        bool measuresMatch = true;
        if (ref.read.empty()) {
            ref = d;
        } else if (d.read.size() == ref.read.size()) {
            for (size_t i = 0; i < d.read.size(); ++i) {
                differ += differingParticles(ref.read[i], d.read[i]);
                measuresMatch = measuresMatch && sameMeasure(ref.measures[i], d.measures[i]) && sameNumber(ref.times[i], d.times[i]);
            }
        } else {
            differ = -1;
        }
        std::printf("   mpm snapshots %-5s %-30s %d frames of %zu particles (one in %d: %d in video memory, %d in system memory) "
                    "in %.1f s, %ld frame particles differ, bindings wrong %ld, weights wrong %ld\n",
                    name, v.label, d.plan.frames, d.plan.frameParticles, d.plan.stride, d.plan.gpuFrames, d.plan.hostFrames(),
                    wall, differ, d.boundWrong, d.weightWrong);
        if (d.read.empty()) continue;
        if (d.plan.stride != 3 || d.plan.frames != 8 || d.plan.hostFrames() != 5)
            failures.push_back(who + "the frames were planned as stride " + std::to_string(d.plan.stride) + ", " +
                               std::to_string(d.plan.frames) + " frames, " + std::to_string(d.plan.hostFrames()) +
                               " in system memory, not 3, 8 and 5");
        if (d.plan.frameParticles != mpmFrameParticles(n, 3) || d.read.front().size() != d.plan.frameParticles)
            failures.push_back(who + "a frame did not hold one particle in three");
        if (differingParticles(d.read.front(), d.read.back()) == 0)
            failures.push_back(who + "the first and last frames are the same, so the run did not move");
        if (differ != 0)
            failures.push_back(who + "the frames differed from the one-chunk run in " + std::to_string(differ) + " particles");
        if (!measuresMatch) failures.push_back(who + "the frames' readouts or times differed from the one-chunk run");
        if (d.boundWrong != 0) failures.push_back(who + "the view's bindings held the wrong frame " + std::to_string(d.boundWrong) + " times");
        if (d.weightWrong != 0) failures.push_back(who + "the interpolation weight was wrong " + std::to_string(d.weightWrong) + " times");
    }
    snaps.clear();
}

void snapshotCheck(const std::filesystem::path& root, const ProjectileDb& db, MpmSolver& solver,
                   std::vector<std::string>& failures) {
    MpmSnapshots snaps;
    try {
        snaps.init(root);
    } catch (const std::exception& e) {
        failures.push_back(std::string("snapshots: the pack shader did not compile: ") + e.what());
        return;
    }
    snapshotRun("era", sandwichCase(db, 60, MpmQuality::Medium), solver, snaps, 24e-6, failures);
    snapshotRun("aphe", catalogueCase(db, "88mm_pzgr39_kwk36", "rha_50", MpmQuality::Medium), solver, snaps, 400e-6, failures);
}

// What each high quality tier makes of a case on this machine: the scene it builds, whether it fits, and
// (when it does) the speed of a few steps and whether the figures stay finite.
void qualityProbe(const ProjectileDb& db, MpmSolver& solver, MpmQuality q, const char* what, const char* projectile,
                  const char* preset, int steps) {
    const auto t0 = std::chrono::steady_clock::now();
    Setup s = catalogueCase(db, projectile, preset, q);
    const double buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const MpmScene& sc = s.scene;
    const size_t need = mpmVideoBytes(sc.particles.size(), sc.nx, sc.ny);
    MpmStoreLimits limits;
    limits.freeVideoBytes = gl::freeVideoBytes();
    limits.freeHostBytes = freeHostBytes();
    limits.maxBlockBytes = gl::maxStorageBlockBytes();
    // SPALL_MPM_FREEVIDEO_GB=8 plans as if only 8 GB of video memory were free (a smaller card).
    if (const char* gb = std::getenv("SPALL_MPM_FREEVIDEO_GB")) {
        const size_t pretend = static_cast<size_t>(std::atof(gb) * 1073741824.0);
        limits.freeVideoBytes = limits.freeVideoBytes ? std::min(limits.freeVideoBytes, pretend) : pretend;
    }
    const MpmStorePlan plan = mpmPlanStore(sc.particles.size(), sc.nx, sc.ny, limits);
    std::printf("   quality %-9s %-5s %11zu particles, grid %dx%d, dx %.3f mm, %d/side, dt %.3g ns, need %.2f GB, free %.2f GB "
                "video + %.2f GB system, block %.2f GB, build %.1f s, %s\n",
                mpmQualityLabel(q), what, sc.particles.size(), sc.nx, sc.ny, sc.dx * 1e3, sc.particlesPerSide, sc.dt * 1e9,
                static_cast<double>(need) / 1073741824.0, static_cast<double>(limits.freeVideoBytes) / 1073741824.0,
                static_cast<double>(limits.freeHostBytes) / 1073741824.0, static_cast<double>(limits.maxBlockBytes) / 1073741824.0,
                buildSeconds, plan.ok ? "fits" : plan.problem.c_str());
    if (plan.ok)
        std::printf("   quality %-9s %-5s plan: %d chunks (%d resident, %d in system memory), %.2f GB video, %.2f GB system\n",
                    mpmQualityLabel(q), what, plan.chunks, plan.residentChunks, plan.spilledChunks,
                    static_cast<double>(plan.videoBytes) / 1073741824.0, static_cast<double>(plan.hostBytes) / 1073741824.0);
    std::fflush(stdout);
    if (!plan.ok || steps <= 0) return;
    try {
        const auto t1 = std::chrono::steady_clock::now();
        solver.load(std::move(s.scene), &limits);
        glFinish();
        const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < steps; ++i) solver.step();
        const MpmMeasure m = solver.measure();
        const double perStep = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count() / steps;
        std::printf("   quality %-9s %-5s loaded in %.1f s, %d steps at %.1f ms each, nose %.1f mm, v %.0f m/s, retired %d, "
                    "max T %.0f K\n",
                    mpmQualityLabel(q), what, loadSeconds, steps, perStep * 1e3, m.nose * 1e3, m.velocity, m.retired,
                    m.maxTemperature);
    } catch (const std::exception& e) {
        std::printf("   quality %-9s %-5s load failed: %s\n", mpmQualityLabel(q), what, e.what());
    }
    solver.unload();
    std::fflush(stdout);
}

}  // namespace

std::vector<std::string> runMpmSelftest(const std::filesystem::path& root) {
    std::vector<std::string> failures;
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    if (!db.load((root / "data" / "armour_materials.json").string(), (root / "data" / "projectiles.json").string(),
                 report, fatal))
        return {"armour catalogue failed to load: " + fatal};
    MpmSolver solver;
    try {
        solver.init(root);
    } catch (const std::exception& e) {
        return {std::string("MPM shader error\n") + e.what()};
    }

    // SPALL_MPM_ONLY=quality reports the high tiers (SPALL_MPM_TIERS=ultra,insane,... narrows it).
    if (const char* only = std::getenv("SPALL_MPM_ONLY"); only && std::string(only) == "quality") {
        const char* tiersEnv = std::getenv("SPALL_MPM_TIERS");
        const std::string tiers = tiersEnv ? tiersEnv : "high,ultra,insane,ludicrous";
        for (const MpmQuality q : {MpmQuality::High, MpmQuality::Ultra, MpmQuality::Insane, MpmQuality::Ludicrous}) {
            std::string label = mpmQualityLabel(q);
            std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (tiers.find(label) == std::string::npos) continue;
            qualityProbe(db, solver, q, "aphe", "88mm_pzgr39_kwk36", "rha_50", 40);
        }
        return failures;
    }

    // SPALL_MPM_ONLY=segments runs just check 20.
    if (const char* only = std::getenv("SPALL_MPM_ONLY"); only && std::string(only) == "segments") {
        segmentCheck(db, solver, failures);
        return failures;
    }

    // SPALL_MPM_ONLY=snapshots runs just check 21.
    if (const char* only = std::getenv("SPALL_MPM_ONLY"); only && std::string(only) == "snapshots") {
        snapshotCheck(root, db, solver, failures);
        return failures;
    }

    // SPALL_MPM_ONLY=checkpoints runs just check 19.
    if (const char* only = std::getenv("SPALL_MPM_ONLY"); only && std::string(only) == "checkpoints") {
        checkpointCheck(db, solver, failures);
        return failures;
    }

    // 1. Free flight: the projectile alone keeps its momentum and moves v t.
    {
        Setup s = rodCase(db, 0.02, MpmQuality::Medium);
        s.scene.particles.resize(static_cast<size_t>(s.scene.projectileCount));
        solver.load(s.scene);
        const MpmMeasure m0 = solver.measure();
        const int steps = 300;
        for (int i = 0; i < steps; ++i) solver.step();
        const MpmMeasure m1 = solver.measure();
        row("free flight", solver, m1, 0);
        const double moved = m1.nose - m0.nose, expected = s.c.velocity * steps * s.scene.dt;
        if (std::abs(moved - expected) > 0.25 * s.scene.dx)
            failures.push_back("free flight moved " + std::to_string(moved * 1e3) + " mm, expected " +
                               std::to_string(expected * 1e3) + " mm");
        if (std::abs(m1.momentumX - m0.momentumX) > 1e-3 * std::abs(m0.momentumX) || std::abs(m1.momentumY) > 1e-4 * std::abs(m0.momentumX))
            failures.push_back("free flight momentum drifted");
        if (m1.intact != s.scene.bodyCount) failures.push_back("free flight lost intact projectile material");
        if (m1.retired) failures.push_back("free flight went non-finite");
    }

    // 2. Hydrodynamic limit: with no strength the crater depth is near L sqrt(rho_p / rho_t) = 1.5 L.
    {
        Setup s = rodCase(db, 0.24, MpmQuality::Medium);
        solver.load(s.scene);
        solver.setStrengthScale(0);
        const double t = 2.5 * s.c.penetrator.length / s.c.velocity;
        const double wall = runTo(solver, t);
        const MpmMeasure m = solver.measure();
        row("hydrodynamic", solver, m, wall);
        const double ratio = m.reach / s.c.penetrator.length;
        std::printf("   mpm hydrodynamic P/L %.2f (theory %.2f)\n", ratio, std::sqrt(17600.0 / 7850.0));
        if (!(ratio > 1.1 && ratio < 1.9)) failures.push_back("hydrodynamic P/L " + std::to_string(ratio) + " outside 1.1-1.9");
        if (m.retired) failures.push_back("hydrodynamic run went non-finite");
        solver.setStrengthScale(1);
    }

    // 3. A thin plate is perforated and the rod carries on.
    {
        Setup s = rodCase(db, 0.01, MpmQuality::Medium);
        solver.load(s.scene);
        // Measured while the nose is still inside the domain (about two rod lengths on).
        const double wall = runTo(solver, (0.01 + 1.5 * s.c.penetrator.length) / s.c.velocity);
        const MpmMeasure m = solver.measure();
        row("thin plate", solver, m, wall);
        if (!(m.intact > 0 && m.nose > 0.01 + s.c.penetrator.diameter && m.velocity > 0.5 * s.c.velocity))
            failures.push_back("thin plate was not perforated");
        if (m.retired) failures.push_back("thin plate run went non-finite");
    }

    // 4. A thick block stops the rod.
    {
        Setup s = rodCase(db, 0.24, MpmQuality::Medium);
        solver.load(s.scene);
        const double wall = runTo(solver, 2.5 * s.c.penetrator.length / s.c.velocity);
        const MpmMeasure m = solver.measure();
        row("thick block", solver, m, wall);
        std::printf("   mpm thick block depth %.1f mm (engineering model %.1f mm)\n", m.reach * 1e3,
                    s.out.result.depth * 1e3);
        if (!(m.reach < 0.24)) failures.push_back("thick block was perforated");
        if (!(m.reach > 0.2 * s.c.penetrator.length)) failures.push_back("rod barely entered the thick block");
        if (m.retired) failures.push_back("thick block run went non-finite");
    }

    // 5. An untouched reactive sandwich stays unreacted and at rest.
    {
        Setup s = sandwichCase(db, 0, MpmQuality::Medium);
        dropProjectile(s.scene);
        solver.load(s.scene);
        const double wall = runTo(solver, solver.time() + 20e-6);
        const MpmMeasure m = solver.measure();
        row("ERA at rest", solver, m, wall);
        if (m.reacted) failures.push_back("an untouched sandwich reacted");
        if (m.maxSpeed > 1) failures.push_back("an untouched sandwich moved (" + std::to_string(m.maxSpeed) + " m/s)");
        if (m.retired) failures.push_back("untouched sandwich run went non-finite");
    }

    // 5b. Soft mounts: the tethered edge strips feel no force at rest; struck, they give a little and
    // ring (a clamped strip would not move), and the plate stays whole and in place.
    {
        auto softCase = [&](double plate) {
            Setup s = rodCase(db, plate, MpmQuality::Medium);
            s.scene = buildMpmScene(db, s.c, RodMaterial::Tungsten, phys::shapeForCase(nullptr, s.c.penetrator), s.out,
                                    MpmQuality::Medium, true);
            return s;
        };
        Setup rest = softCase(0.02);
        dropProjectile(rest.scene);
        solver.load(rest.scene);
        const double restWall = runTo(solver, solver.time() + 20e-6);
        const MpmMeasure mr = solver.measure();
        row("soft at rest", solver, mr, restWall);
        if (mr.maxSpeed > 1) failures.push_back("an untouched soft-mounted plate moved (" + std::to_string(mr.maxSpeed) + " m/s)");
        if (mr.retired) failures.push_back("untouched soft-mounted plate went non-finite");

        Setup s = softCase(0.02);
        solver.load(s.scene);
        const double wall = runTo(solver, (0.02 + 1.5 * s.c.penetrator.length) / s.c.velocity);
        const MpmMeasure m = solver.measure();
        row("soft mounts", solver, m, wall);
        const std::vector<MpmParticleState> ps = solver.readParticles();
        double give = 0;
        int tethered = 0;
        for (size_t k = 0; k < ps.size() && k < s.scene.particles.size(); ++k) {
            const MpmSeed& seed = s.scene.particles[k];
            if (!(seed.flags & kMpmTethered)) continue;
            ++tethered;
            give = std::max(give, std::hypot(ps[k].x - seed.x, ps[k].y - seed.y));
        }
        std::printf("   mpm soft mounts: %d tethered, the edge gave up to %.3f mm (cell %.2f mm)\n", tethered, give * 1e3,
                    s.scene.dx * 1e3);
        if (!(tethered > 0 && give > 1e-3 * s.scene.dx)) failures.push_back("the soft mounts did not give");
        if (!(give < 0.25 * s.scene.plateHalfExtent)) failures.push_back("the soft mounts let the plate edge fly off");
        if (m.retired) failures.push_back("soft-mounted run went non-finite");
    }

    // 6. A sandwich lit at its centre drives its plates near the symmetric-sandwich Gurney speed
    //    sqrt(2 Q) (M / C + 1/3)^-1/2, M the mass of both plates and C of the explosive per area.
    {
        Setup s = sandwichCase(db, 0, MpmQuality::Medium);
        dropProjectile(s.scene);
        solver.load(s.scene);
        const std::vector<phys::LayerResult>& l = s.out.result.layers;
        solver.ignite(0.5 * (l[1].front + l[1].back), 0, 1);
        const double wall = runTo(solver, solver.time() + 20e-6);
        const MpmMeasure m = solver.measure();
        row("ERA lit", solver, m, wall);
        const phys::MpmMaterial& plate = s.scene.materials[1];
        const phys::MpmMaterial& ex = s.scene.materials[2];
        const double M = plate.density * (l[0].back - l[0].front + l[2].back - l[2].front);
        const double C = ex.density * (l[1].back - l[1].front);
        const double gurney = std::sqrt(2 * ex.energy) / std::sqrt(M / C + 1.0 / 3.0);
        const double v = plateSpeed(s, solver.readParticles(), 0, 0.03);
        std::printf("   mpm ERA lit plates %.0f m/s (symmetric-sandwich Gurney %.0f m/s), reacted %d/%d\n", v, gurney,
                    m.reacted, s.scene.reactiveCount);
        if (!(v > 0.6 * gurney && v < 1.3 * gurney))
            failures.push_back("lit sandwich plates at " + std::to_string(v) + " m/s, outside 0.6-1.3 x Gurney");
        if (m.reacted < 0.9 * s.scene.reactiveCount) failures.push_back("the lit sandwich did not react through");
        if (m.retired) failures.push_back("lit sandwich run went non-finite");
    }

    // 7. The rod hitting the sandwich at 60 deg sets it off and the plates are thrown.
    {
        Setup s = sandwichCase(db, 60, MpmQuality::Medium);
        solver.load(s.scene);
        const double wall = runTo(solver, 30e-6);
        const MpmMeasure m = solver.measure();
        row("ERA hit", solver, m, wall);
        const double v = plateSpeed(s, solver.readParticles(), 3 * s.c.penetrator.diameter, 0.8 * s.scene.plateHalfExtent);
        std::printf("   mpm ERA hit plates %.0f m/s away from the impact, reacted %d/%d, fastest intact plate %.0f m/s\n", v,
                    m.reacted, s.scene.reactiveCount, m.plateSpeed);
        if (m.reacted < 0.5 * s.scene.reactiveCount) failures.push_back("the rod did not set the sandwich off");
        if (!(v > 300)) failures.push_back("the hit sandwich's plates moved at only " + std::to_string(v) + " m/s");
        if (m.retired) failures.push_back("hit sandwich run went non-finite");
    }

    // 8. The rod into the ceramic sandwich (10 mm HHA, 60 mm alumina, 30 mm RHA). Comminution
    //    starts at the shot line: while the nose is still in the cover plate the ceramic far from
    //    the shot line is mostly intact. JH-2 damage is local, so crack bands then spread across
    //    the narrow tile; by the end the ceramic around the shot line is all comminuted.
    {
        const std::vector<PresetLayer> layers = {{db.findMaterial("hha"), 0.01, 0},
                                                 {db.findMaterial("alumina"), 0.06, 0},
                                                 {db.findMaterial("rha"), 0.03, 0}};
        Setup s = rodCase(db, layers, 0, MpmQuality::Medium);
        solver.load(s.scene);
        const double D = s.c.penetrator.diameter, h = s.scene.plateHalfExtent;
        const phys::LayerResult& cover = s.out.result.layers[0];
        const double end = 2 * s.c.penetrator.length / s.c.velocity;
        double wall = 0;
        MpmMeasure m = solver.measure();
        while (m.nose < cover.front + 0.8 * (cover.back - cover.front) && solver.time() < end) {
            wall += runTo(solver, solver.time() + 0.5e-6);
            m = solver.measure();
        }
        std::vector<MpmParticleState> ps = solver.readParticles();
        const double nearEarly = flaggedFraction(s, ps, 1, 0, 1.5 * D, kMpmComminuted);
        const double farEarly = flaggedFraction(s, ps, 1, 0.5 * h, 0.9 * h, kMpmComminuted);
        wall += runTo(solver, end);
        m = solver.measure();
        row("ceramic", solver, m, wall);
        ps = solver.readParticles();
        const double nearShot = flaggedFraction(s, ps, 1, 0, 1.5 * D, kMpmComminuted);
        const double farShot = flaggedFraction(s, ps, 1, 0.5 * h, 0.9 * h, kMpmComminuted);
        std::printf("   mpm ceramic comminuted within 1.5 D of the shot line / far from it: %.0f%% / %.0f%% with the "
                    "nose in the cover, %.0f%% / %.0f%% at the end, %d/%d in all\n",
                    100 * nearEarly, 100 * farEarly, 100 * nearShot, 100 * farShot, m.comminuted, s.scene.ceramicCount);
        if (!(nearEarly > 0.5))
            failures.push_back("the rod comminuted only " + std::to_string(100 * nearEarly) +
                               "% of the ceramic near the shot line while in the cover");
        if (!(farEarly < 0.5 * nearEarly))
            failures.push_back("comminution did not start at the shot line (" + std::to_string(100 * farEarly) +
                               "% far from it, " + std::to_string(100 * nearEarly) + "% near it)");
        if (!(nearShot > 0.9))
            failures.push_back("only " + std::to_string(100 * nearShot) + "% of the ceramic near the shot line comminuted");
        if (m.retired) failures.push_back("ceramic sandwich run went non-finite");
    }

    // 9. The rod into the bulging sandwich (6 mm RHA, 6 mm rubber, 6 mm RHA at 60 deg): the
    //    rubber stays finite and the plates bulge apart around the hole.
    {
        const int rha = db.findMaterial("rha");
        Setup s = rodCase(db, {{rha, 0.006, 0}, {db.findMaterial("rubber"), 0.006, 0}, {rha, 0.006, 0}}, 60,
                          MpmQuality::Medium);
        solver.load(s.scene);
        const double D = s.c.penetrator.diameter;
        const double wall = runTo(solver, 30e-6);
        const MpmMeasure m = solver.measure();
        row("bulging", solver, m, wall);
        const std::vector<MpmParticleState> ps = solver.readParticles();
        const double near1 = separation(s, ps, 0, 2, D, 2 * D), near2 = separation(s, ps, 0, 2, 2 * D, 4 * D);
        const double away = separation(s, ps, 0, 2, 0.5 * s.scene.plateHalfExtent, 0.9 * s.scene.plateHalfExtent);
        std::printf("   mpm bulging plates apart %.1f mm at 1-2 D, %.1f mm at 2-4 D, %.1f mm far from the hole\n",
                    1e3 * near1, 1e3 * near2, 1e3 * away);
        if (!(near1 > 0.005 && near1 > 2 * away && near2 > away))
            failures.push_back("the bulging sandwich plates did not bulge apart around the hole");
        if (m.retired) failures.push_back("bulging sandwich run went non-finite");
    }

    // 10. The rod into ceramic on a composite backing (20 mm alumina, 25 mm UHMWPE): the
    //     laminate starts to delaminate around the shot line (the dart's cap strikes first) and
    //     the front spreads outward; its weak through-thickness strength lets the plane-strain
    //     waves delaminate it widely by the end, while its fibres break only around the shot line.
    {
        Setup s = rodCase(db, {{db.findMaterial("alumina"), 0.020, 0}, {db.findMaterial("uhmwpe"), 0.025, 0}}, 0,
                          MpmQuality::Medium);
        solver.load(s.scene);
        const double D = s.c.penetrator.diameter, h = s.scene.plateHalfExtent;
        const phys::LayerResult& tile = s.out.result.layers[0];
        const double end = 2 * s.c.penetrator.length / s.c.velocity;
        double wall = 0;
        MpmMeasure m = solver.measure();
        while (m.nose < tile.front + 0.5 * (tile.back - tile.front) && solver.time() < end) {
            wall += runTo(solver, solver.time() + 0.5e-6);
            m = solver.measure();
        }
        std::vector<MpmParticleState> ps = solver.readParticles();
        const double delamEarly = flaggedFraction(s, ps, 1, 0, 1.5 * D, kMpmDelaminated);
        const double delamEarlyFar = flaggedFraction(s, ps, 1, 0.5 * h, 0.9 * h, kMpmDelaminated);
        wall += runTo(solver, end);
        m = solver.measure();
        row("composite", solver, m, wall);
        ps = solver.readParticles();
        const double delamNear = flaggedFraction(s, ps, 1, 0, 1.5 * D, kMpmDelaminated);
        const double delamFar = flaggedFraction(s, ps, 1, 0.5 * h, 0.9 * h, kMpmDelaminated);
        const double brokenNear = flaggedFraction(s, ps, 1, 0, 1.5 * D, kMpmFibreBroken);
        const double brokenFar = flaggedFraction(s, ps, 1, 0.5 * h, 0.9 * h, kMpmFibreBroken);
        std::printf("   mpm composite delaminated within 1.5 D of the shot line / far from it: %.0f%% / %.0f%% with the "
                    "nose halfway through the tile, %.0f%% / %.0f%% at the end; fibres broken %.0f%% / %.0f%%; %d/%d/%d in all\n",
                    100 * delamEarly, 100 * delamEarlyFar, 100 * delamNear, 100 * delamFar, 100 * brokenNear,
                    100 * brokenFar, m.delaminated, m.fibreBroken, s.scene.compositeCount);
        if (!(delamEarly > 0.5 && delamEarlyFar < 0.5 * delamEarly))
            failures.push_back("the laminate did not start to delaminate around the shot line");
        if (!(delamNear > 0.9))
            failures.push_back("only " + std::to_string(100 * delamNear) + "% of the laminate near the shot line delaminated");
        if (!(brokenNear > 0.2 && brokenFar < 0.25 * brokenNear))
            failures.push_back("the laminate's fibres did not break around the shot line alone (" +
                               std::to_string(100 * brokenNear) + "% near it, " + std::to_string(100 * brokenFar) +
                               "% far from it)");
        if (m.retired) failures.push_back("ceramic on composite run went non-finite");
    }

    // 11. A free jet (14 cone diameters of standoff, so it is still short of the plate at
    //     1.5 t_b): before breakup it stretches as (v_tip - v_min) t; at t_b every particle
    //     breaks up, every neck fails, the segments drift sideways with their kicks (the
    //     copper's strength takes up some of each kick and slows the tip a little), and the x
    //     momentum is kept.
    {
        Setup s = jetCase(db, 14, 0.02, MpmQuality::Medium);
        solver.load(s.scene);
        const phys::JetParameters jp = phys::jetParameters(s.c.penetrator);
        const double tb = jp.breakupTime;
        const int n = s.scene.projectileCount;
        const MpmMeasure m0 = solver.measure();
        double wall = runTo(solver, 0.9 * tb);
        MpmMeasure m = solver.measure();
        std::vector<MpmParticleState> ps = solver.readParticles();
        double lo = 1e9, hi = -1e9;
        for (int k = 0; k < n; ++k) {
            if (ps[k].flags & kMpmInactive) continue;
            lo = std::min(lo, ps[k].x);
            hi = std::max(hi, ps[k].x);
        }
        const double length = hi - lo, expected = (jp.tipVelocity - jp.minVelocity) * solver.time();
        const int brokenEarly = m.jetBroken;
        wall += runTo(solver, 1.5 * tb);
        m = solver.measure();
        jetRow("free jet", solver, m, wall);
        ps = solver.readParticles();
        int necksIntact = 0;
        double num = 0, den = 0;
        for (int k = 0; k < n; ++k) {
            const MpmSeed& seed = s.scene.particles[static_cast<size_t>(k)];
            if (seed.flags & kMpmJetNeck) {
                if (!(ps[k].flags & kMpmFailed)) ++necksIntact;
            } else {
                num += ps[k].vy * seed.kick;
                den += static_cast<double>(seed.kick) * seed.kick;
            }
        }
        const double lateral = den > 0 ? num / den : 0;
        const double px = m.momentumX / m0.momentumX;
        std::printf("   mpm free jet length %.1f mm at 0.9 t_b (stretching %.1f mm); at 1.5 t_b %d/%d broken, %d necks "
                    "intact, lateral velocity %.2f of the kick, x momentum %.4f of the start, fastest %.0f m/s (tip %.0f)\n",
                    length * 1e3, expected * 1e3, m.jetBroken, n, necksIntact, lateral, px, m.leadVelocity, jp.tipVelocity);
        if (std::abs(length / expected - 1) > 0.1)
            failures.push_back("the free jet's length " + std::to_string(length * 1e3) + " mm is not within 10% of " +
                               std::to_string(expected * 1e3) + " mm");
        if (brokenEarly) failures.push_back("the free jet broke up before t_b");
        if (m.jetBroken != n || necksIntact) failures.push_back("the free jet did not break up at t_b");
        if (!(lateral > 0.4 && lateral < 1.5)) failures.push_back("the jet segments did not drift with their kicks");
        if (std::abs(px - 1) > 0.01) failures.push_back("the free jet's x momentum drifted");
        if (std::abs(m.leadVelocity / jp.tipVelocity - 1) > 0.05) failures.push_back("the free jet's tip slowed");
        if (!(m.reach < 0)) failures.push_back("the free jet reached the plate early");
        if (m.retired) failures.push_back("free jet run went non-finite");
    }

    // 12. A jet into a thick RHA block at 2 cone diameters stops inside it, its crater a fair
    //     part of the model's (the plane-strain sheet is only a few cells wide).
    {
        Setup s = jetCase(db, 2, 0.6, MpmQuality::Medium);
        solver.load(s.scene);
        const double wall = runTo(solver, s.out.result.endTime);
        const MpmMeasure m = solver.measure();
        jetRow("jet block", solver, m, wall);
        std::printf("   mpm jet block reach %.1f mm (engineering model %.1f mm)\n", m.reach * 1e3,
                    s.out.result.depth * 1e3);
        const double ratio = m.reach / s.out.result.depth;
        if (!(m.reach < 0.6)) failures.push_back("the jet perforated the thick block");
        if (!(ratio > 0.4 && ratio < 1.1))
            failures.push_back("the jet's crater " + std::to_string(m.reach * 1e3) + " mm is out of step with the model's " +
                               std::to_string(s.out.result.depth * 1e3) + " mm");
        if (m.retired) failures.push_back("jet into a thick block went non-finite");
    }

    // 13. A jet through a 20 mm plate at 2 cone diameters: fast jet material carries on beyond it.
    {
        Setup s = jetCase(db, 2, 0.02, MpmQuality::Medium);
        solver.load(s.scene);
        const double back = s.out.result.layers[0].back;
        const double wall = runTo(solver, s.out.result.layers[0].exitTime + 0.06 / s.out.result.residualVelocity);
        const MpmMeasure m = solver.measure();
        jetRow("jet plate", solver, m, wall);
        std::printf("   mpm jet plate reach %.1f mm beyond the back, fastest jet %.0f m/s (model residual tip %.0f m/s)\n",
                    (m.reach - back) * 1e3, m.leadVelocity, s.out.result.residualVelocity);
        if (!(m.reach > back + 0.02 && m.leadVelocity > 2000))
            failures.push_back("the jet did not carry on beyond the 20 mm plate");
        if (m.retired) failures.push_back("jet through a plate went non-finite");
    }
    // 14. A 122 mm shell into 50 mm RHA hits hard enough to throw the whole plate in plane strain.
    //     The band next to each held edge (the mounting) stretches but never fails, so the plate
    //     stays on its mounts: the band moves only a few cells, long after the hit.
    {
        Setup s;
        const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(db.findProjectile("122mm_br471"))];
        s.c.penetrator = e.penetrator;
        s.c.velocity = e.velocity;
        s.c.layers = {{db.findMaterial("rha"), 0.05, 0}};
        s.out = evaluateArmour(db, s.c);
        s.scene = buildMpmScene(db, s.c, e.rodMaterial, phys::shapeForCase(e.shape.present ? &e.shape : nullptr, e.penetrator),
                                s.out, MpmQuality::Medium);
        solver.load(s.scene);
        const double wall = runTo(solver, 3 * s.scene.tEnd);
        const MpmMeasure m = solver.measure();
        row("mounts", solver, m, wall);
        const std::vector<MpmParticleState> ps = solver.readParticles();
        int band = 0, failed = 0;
        double moved = 0;
        for (size_t k = 0; k < ps.size() && k < s.scene.particles.size(); ++k) {
            const MpmSeed& seed = s.scene.particles[k];
            if (!(seed.flags & kMpmMount)) continue;
            ++band;
            if (ps[k].flags & kMpmFailed) ++failed;
            moved += std::hypot(ps[k].x - seed.x, ps[k].y - seed.y);
        }
        moved /= std::max(band, 1);
        std::printf("   mpm mounts band %d particles, %d failed, moved %.1f mm (%.1f cells) at %.0f us\n", band, failed,
                    1e3 * moved, moved / s.scene.dx, 3e6 * s.scene.tEnd);
        if (band == 0) failures.push_back("the plate has no mount band");
        if (failed) failures.push_back(std::to_string(failed) + " mount band particles failed");
        if (!(moved < 3 * s.scene.dx))
            failures.push_back("the plate tore off its mounts (band moved " + std::to_string(1e3 * moved) + " mm)");
        if (m.retired) failures.push_back("mounts run went non-finite");
    }

    // 15. An APHE round through 50 mm RHA: the fuze triggers on the plate, the base delay runs
    //     out after perforation and the filler burns from the fuze outwards.
    {
        Setup s = catalogueCase(db, "88mm_pzgr39_kwk36", "rha_50", MpmQuality::Medium);
        solver.load(s.scene);
        MpmMeasure m;
        double wall = 0;
        double reach = 0;
        const double firstLit = runFuzed(solver, s.scene.tEnd, m, wall, &reach);
        fuzeRow("aphe rha_50", solver, m, firstLit, wall);
        std::printf("   mpm aphe rha_50 furthest reach %.1f mm\n", reach * 1e3);
        const double back = s.out.result.layers.back().back;
        if (!m.fuzeTriggered) failures.push_back("the APHE fuze did not trigger on 50 mm RHA");
        if (!(reach > back)) failures.push_back("the APHE round did not perforate 50 mm RHA");
        if (!(m.fillerLit > 0.9 * s.scene.fillerCount))
            failures.push_back("only " + std::to_string(m.fillerLit) + " of " + std::to_string(s.scene.fillerCount) +
                               " filler particles lit");
        if (!(firstLit >= m.fuzeTime + s.scene.fuze.delay))
            failures.push_back("the APHE filler lit before the fuze delay ran out");
        if (m.retired) failures.push_back("APHE run went non-finite");
    }

    // 16. The same round into the reactive sandwich: the explosive layer's detonation does not
    //     light the filler; only the fuze does.
    {
        Setup s = catalogueCase(db, "88mm_pzgr39_kwk36", "reactive_sandwich", MpmQuality::Medium);
        solver.load(s.scene);
        MpmMeasure m;
        double wall = 0;
        const double firstLit = runFuzed(solver, s.scene.tEnd, m, wall);
        fuzeRow("aphe era", solver, m, firstLit, wall);
        if (firstLit < 1e29 && !(m.fuzeTriggered && firstLit >= m.fuzeTime + s.scene.fuze.delay))
            failures.push_back("the reactive layer lit the APHE filler");
        if (m.retired) failures.push_back("APHE into the reactive sandwich went non-finite");
    }

    // 17. A burst beyond the grid's right edge: the round runs to the end with nothing non-finite.
    {
        Setup s = catalogueCase(db, "88mm_pzgr39_kwk36", "rha_50", MpmQuality::Medium);
        const double edge = s.out.fuze.burstDepth - s.scene.diameter;
        s.scene.nx = std::min(s.scene.nx, static_cast<int>(std::ceil((edge - s.scene.originX) / s.scene.dx)) + 1);
        solver.load(s.scene);
        MpmMeasure m;
        double wall = 0;
        const double firstLit = runFuzed(solver, s.scene.tEnd, m, wall);
        fuzeRow("aphe edge", solver, m, firstLit, wall);
        if (m.retired) failures.push_back("a burst beyond the grid went non-finite");
    }

    // 18. The rod through 12 mm RHA, a 1.5 mm aluminium witness sheet 150 mm behind it and a free
    //     200 mm gelatin block 100 mm further back: the sheet is torn at the shot line and holds at
    //     its clamped edge, the gelatin (a kPa-scale elastomer) stays finite and its free block is
    //     never anchored. With the nose 40 mm into the gelatin the first 30 mm of it are cut along
    //     the shot line and still whole well away from it; the plane-strain blade's energy per
    //     metre of depth then fails the whole block (the known blade limit), so the end is not asked
    //     to leave it whole.
    {
        PresetLayer plate, sheet, gel;
        plate.material = db.findMaterial("rha");
        plate.thickness = 0.012;
        sheet.material = db.findMaterial("al2024");
        sheet.thickness = 0.0015;
        sheet.gap = 0.15;
        gel.material = db.findMaterial("ballistic_gelatin");
        gel.thickness = 0.25;
        gel.gap = 0.1;
        gel.blockWidth = 0.2;
        gel.blockCount = 1;
        Setup s = rodCase(db, {plate, sheet, gel}, 0, MpmQuality::Medium);
        solver.load(s.scene);
        const double D = s.c.penetrator.diameter, h = s.scene.plateHalfExtent;
        const phys::LayerResult& block = s.out.result.layers[2];
        // Failed fraction of the gelatin's first 30 mm whose initial |y| lies in [lo, hi].
        auto gelFailed = [&](const std::vector<MpmParticleState>& ps, double lo, double hi) {
            int n = 0, failed = 0;
            for (size_t k = 0; k < ps.size() && k < s.scene.particles.size(); ++k) {
                const MpmSeed& seed = s.scene.particles[k];
                if (seed.layer != 2 || (seed.flags & kMpmProjectile) || seed.x > block.front + 0.03) continue;
                const double y = std::abs(seed.y);
                if (y < lo || y > hi) continue;
                ++n;
                if (ps[k].flags & kMpmFailed) ++failed;
            }
            return n ? static_cast<double>(failed) / n : 0;
        };
        double wall = 0;
        MpmMeasure m = solver.measure();
        while (m.nose < block.front + 0.04 && solver.time() < s.scene.tEnd) {
            wall += runTo(solver, solver.time() + 1e-6);
            m = solver.measure();
        }
        std::vector<MpmParticleState> ps = solver.readParticles();
        const double gelNearEarly = gelFailed(ps, 0, 2 * D), gelFarEarly = gelFailed(ps, 0.06, 0.095);
        wall += runTo(solver, s.scene.tEnd);
        m = solver.measure();
        row("gelatin", solver, m, wall);
        ps = solver.readParticles();
        const double sheetNear = flaggedFraction(s, ps, 1, 0, 2 * D, kMpmFailed);
        const double sheetFar = flaggedFraction(s, ps, 1, 0.5 * h, 0.9 * h, kMpmFailed);
        const double gelFarEnd = gelFailed(ps, 0.06, 0.095);
        // The witness readout the armour view offers, on the same readback: a hole through the sheet
        // at the shot line that is narrower than the sheet, and the rod past it.
        const WitnessReadout witness = measureWitness(s.scene, ps, 1, m);
        int gelAnchored = 0, gelTotal = 0;
        for (const MpmSeed& seed : s.scene.particles)
            if (seed.layer == 2 && !(seed.flags & kMpmProjectile)) {
                ++gelTotal;
                if (seed.flags & kMpmAnchored) ++gelAnchored;
            }
        std::printf("   mpm gelatin sheet failed %.0f %% at the shot line, %.0f %% far from it; gelatin's first 30 mm "
                    "failed %.0f %% within 2 D, %.0f %% at 6-9.5 cm with the nose 40 mm in, %.0f %% at 6-9.5 cm at the "
                    "end; %d gelatin particles, %d anchored\n",
                    100 * sheetNear, 100 * sheetFar, 100 * gelNearEarly, 100 * gelFarEarly, 100 * gelFarEnd, gelTotal,
                    gelAnchored);
        if (!(sheetNear > 0.3))
            failures.push_back("the witness sheet was torn over only " + std::to_string(100 * sheetNear) +
                               "% near the shot line");
        std::printf("   mpm witness sheet: hole %.1f mm, bulge %.1f mm, fastest %.0f m/s, penetrator past it at %.0f m/s\n",
                    witness.holeWidth * 1e3, witness.bulge * 1e3, witness.peakSpeed, witness.residualVelocity);
        if (!witness.valid || !(witness.holeWidth > 0) || !(witness.holeWidth < 1.6 * h))
            failures.push_back("the witness readout found no sensible hole (" + std::to_string(witness.holeWidth * 1e3) +
                               " mm) in the sheet the rod went through");
        if (witness.valid && !(witness.bulge >= 0 && witness.peakSpeed >= 0))
            failures.push_back("the witness readout reported a negative bulge or speed");
        if (!(sheetFar < 0.5 * sheetNear))
            failures.push_back("the witness sheet failed as much far from the shot line (" +
                               std::to_string(100 * sheetFar) + "%) as near it");
        if (!(gelNearEarly > 0.3))
            failures.push_back("the gelatin was cut over only " + std::to_string(100 * gelNearEarly) +
                               "% near the shot line with the nose 40 mm in");
        if (!(gelFarEarly < 0.5 * gelNearEarly))
            failures.push_back("the gelatin failed as much far from the shot line (" +
                               std::to_string(100 * gelFarEarly) + "%) as near it with the nose 40 mm in");
        if (gelTotal == 0 || gelAnchored != 0) failures.push_back("the gelatin block is missing or anchored");
        if (m.retired) failures.push_back("the gelatin run went non-finite");
    }

    checkpointCheck(db, solver, failures);
    segmentCheck(db, solver, failures);
    snapshotCheck(root, db, solver, failures);
    return failures;
}
