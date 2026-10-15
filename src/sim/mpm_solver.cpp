#include "sim/mpm_solver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Mirrors struct Particle in shaders/mpm/mpm_common.glsl (std430).
struct MpmParticle {
    float x[2], v[2];
    float C[4], F[4], tau[4];
    float Fzz, epsP, temp, vol;
    float mass;
    uint32_t mat, flags;
    float stress;
    float xlo[2];
    float pressure, burn;
    float X0[2];
    float tLight, fracture;
    float fibre[2];
    float kick, pad;
};
static_assert(sizeof(MpmParticle) == kMpmParticleBytes, "MpmParticle must match the std430 layout");

// Mirrors the Detonation block in shaders/mpm/g2p.comp.
struct MpmDetonation {
    uint32_t step, layer;
    float origin[2];
    float time, pad[3];
};
constexpr uint32_t kNoFront = 0xFFFFFFFFu;

// Mirrors the Fuze block in shaders/mpm/g2p.comp.
struct MpmFuze {
    uint32_t acc[4];
    uint32_t step;
    float time;
    uint32_t pad[2];
};
constexpr int kMeasureVec4s = 6;

// Copies the first `bytes` of a buffer to the start of another, on the GPU.
void copyBytes(const gl::Buffer& from, const gl::Buffer& to, size_t bytes) {
    glBindBuffer(GL_COPY_READ_BUFFER, from.id());
    glBindBuffer(GL_COPY_WRITE_BUFFER, to.id());
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, static_cast<GLsizeiptr>(bytes));
}

void copyBuffer(const gl::Buffer& from, const gl::Buffer& to) { copyBytes(from, to, from.size()); }

constexpr int kParticleGroup = 256;
constexpr int kGridGroup = 16;

GLuint groups(int n, int size) { return static_cast<GLuint>((n + size - 1) / size); }

// The starting state of particles first .. first + count - 1 of the scene.
void fillParticles(const MpmScene& s, size_t first, size_t count, MpmParticle* out) {
    for (size_t i = 0; i < count; ++i) {
        const MpmSeed& seed = s.particles[first + i];
        MpmParticle& p = out[i];
        p = {};
        p.x[0] = static_cast<float>((seed.x - s.originX) / s.dx);
        p.x[1] = static_cast<float>((seed.y - s.originY) / s.dx);
        p.X0[0] = p.x[0];
        p.X0[1] = p.x[1];
        p.v[0] = seed.vx;
        p.v[1] = seed.vy;
        p.kick = seed.kick;
        p.F[0] = p.F[3] = 1;
        p.Fzz = 1;
        p.temp = seed.temp > 0 ? seed.temp : static_cast<float>(phys::kRoomTemperature);
        p.vol = seed.volume;
        p.mass = static_cast<float>(s.materials[seed.material].density * seed.volume);
        p.mat = seed.material;
        p.flags = seed.flags;
        p.fibre[0] = static_cast<float>(s.faceTangent[0]);
        p.fibre[1] = static_cast<float>(s.faceTangent[1]);
    }
}

}  // namespace

MpmSolver::MpmSolver() = default;
MpmSolver::~MpmSolver() {
    dropMeasure();
    for (StepTimer& t : timers_)
        if (t.begin) {
            glDeleteQueries(1, &t.begin);
            glDeleteQueries(1, &t.end);
        }
}

void MpmSolver::init(const std::filesystem::path& root) {
    const auto dir = root / "shaders" / "mpm";
    p2g_.compute(dir / "p2g.comp");
    gridUpdate_.compute(dir / "grid.comp");
    g2p_.compute(dir / "g2p.comp");
    measure_.compute(dir / "measure.comp");
}

void MpmSolver::unload() {
    dropMeasure();
    loaded_ = false;
    scene_ = MpmScene{};
    plan_ = MpmStorePlan{};
    steps_ = 0;
    windowHolds_ = -1;
    // The memory goes now: the next scene is planned against what is free.
    chunks_.clear();
    chunks_.shrink_to_fit();
    for (gl::Buffer* b : {&grid_, &gridVelocity_, &materials_, &partials_, &detonation_, &fuze_, &window_}) *b = gl::Buffer();
    // Another scene costs differently per step.
    msPerStep_ = 0;
    for (StepTimer& t : timers_) t.pending = false;
}

void MpmSolver::load(MpmScene scene, const MpmStoreLimits* limits) {
    unload();
    if (!scene.supported || scene.particles.empty()) return;
    scene_ = std::move(scene);
    plan_ = mpmPlanStore(scene_.particles.size(), scene_.nx, scene_.ny, limits ? *limits : MpmStoreLimits{});
    if (!plan_.ok) {
        const std::string why = plan_.problem;
        unload();
        throw std::runtime_error("the deformation simulation does not fit this machine: " + why);
    }
    try {
        allocateStore();
    } catch (...) {
        unload();
        throw;
    }

    massRef_ = 0;
    for (const MpmSeed& s : scene_.particles)
        massRef_ = std::max(massRef_, scene_.materials[s.material].density * s.volume);
    velRef_ = std::max(4000.0, 2 * scene_.velocity);
    loaded_ = true;
    reset();
}

// Gives every buffer its memory: the grid and readout partials, the chunks (the first
// plan_.residentChunks in video memory, the rest in system memory) and the streaming window.
void MpmSolver::allocateStore() {
    while (glGetError() != GL_NO_ERROR) {
    }
    std::vector<float> mats(scene_.materials.size() * kMpmMaterialFloats);
    for (size_t i = 0; i < scene_.materials.size(); ++i) mpmPackMaterial(scene_.materials[i], &mats[i * kMpmMaterialFloats]);
    materials_.allocate(GL_SHADER_STORAGE_BUFFER, mats.size() * sizeof(float), mats.data(), GL_STATIC_DRAW);

    const size_t nodes = static_cast<size_t>(scene_.nx) * scene_.ny;
    grid_.allocate(GL_SHADER_STORAGE_BUFFER, nodes * 4 * sizeof(int32_t), nullptr, GL_DYNAMIC_DRAW);
    gridVelocity_.allocate(GL_SHADER_STORAGE_BUFFER, nodes * 2 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    partials_.allocate(GL_SHADER_STORAGE_BUFFER,
                       static_cast<size_t>(groups(particleCount(), kParticleGroup)) * kMeasureVec4s * 4 * sizeof(float), nullptr,
                       GL_DYNAMIC_DRAW);
    if (glGetError() == GL_OUT_OF_MEMORY) throw std::runtime_error("out of video memory for the simulation grid");

    chunks_.clear();
    chunks_.resize(static_cast<size_t>(plan_.chunks));
    for (int i = 0; i < plan_.chunks; ++i) {
        Chunk& c = chunks_[static_cast<size_t>(i)];
        c.first = plan_.chunkFirst(i);
        c.count = plan_.chunkCount(i);
        c.spilled = plan_.spilled(i);
        const size_t bytes = c.count * sizeof(MpmParticle);
        if (c.spilled) {
            if (!c.buf.allocateHost(GL_SHADER_STORAGE_BUFFER, bytes))
                throw std::runtime_error("the driver would not keep " + std::to_string(bytes >> 20) + " MiB of particles in system memory");
        } else {
            c.buf.allocate(GL_SHADER_STORAGE_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW);
            if (glGetError() == GL_OUT_OF_MEMORY) throw std::runtime_error("out of video memory for the particles");
        }
    }
    if (plan_.spilledChunks > 0) {
        window_.allocate(GL_SHADER_STORAGE_BUFFER, plan_.windowBytes, nullptr, GL_DYNAMIC_COPY);
        if (glGetError() == GL_OUT_OF_MEMORY) throw std::runtime_error("out of video memory for the streaming window");
    }
}

void MpmSolver::reset() {
    dropMeasure();
    steps_ = 0;
    windowHolds_ = -1;
    if (!loaded_) return;
    // Uploaded in slices: the finest levels have millions of particles, and a second copy of
    // them in memory just to hand over would be gigabytes.
    constexpr size_t kUploadSlice = size_t(1) << 20;
    std::vector<MpmParticle> slice(std::min(scene_.particles.size(), kUploadSlice));
    for (Chunk& c : chunks_)
        for (size_t at = 0; at < c.count; at += slice.size()) {
            const size_t count = std::min(slice.size(), c.count - at);
            fillParticles(scene_, c.first + at, count, slice.data());
            c.buf.update(slice.data(), count * sizeof(MpmParticle), at * sizeof(MpmParticle));
        }
    const MpmDetonation none{kNoFront, 0, {0, 0}, 0, {0, 0, 0}};
    detonation_.allocate(GL_SHADER_STORAGE_BUFFER, sizeof(none), &none, GL_DYNAMIC_DRAW);
    const MpmFuze quiet{{0, 0, 0, 0}, kNoFront, 0, {0, 0}};
    fuze_.allocate(GL_SHADER_STORAGE_BUFFER, sizeof(quiet), &quiet, GL_DYNAMIC_DRAW);
    gl::check("MPM reset");
}

size_t MpmState::bytes() const {
    size_t b = detonation_.size() + fuze_.size();
    for (const gl::Buffer& p : particles_) b += p.size();
    return b;
}

void MpmState::release() {
    particles_.clear();
    particles_.shrink_to_fit();
    detonation_ = gl::Buffer();
    fuze_ = gl::Buffer();
    steps_ = 0;
    valid_ = false;
}

size_t MpmSolver::stateBytes() const {
    if (!loaded_ || plan_.spilledChunks > 0) return 0;
    size_t b = detonation_.size() + fuze_.size();
    for (const Chunk& c : chunks_) b += c.buf.size();
    return b;
}

bool MpmSolver::saveState(MpmState& out) const {
    out.release();
    if (!loaded_ || plan_.spilledChunks > 0) return false;
    while (glGetError() != GL_NO_ERROR) {
    }
    out.particles_.resize(chunks_.size());
    for (size_t i = 0; i < chunks_.size(); ++i)
        out.particles_[i].allocate(GL_SHADER_STORAGE_BUFFER, chunks_[i].buf.size(), nullptr, GL_DYNAMIC_COPY);
    out.detonation_.allocate(GL_SHADER_STORAGE_BUFFER, detonation_.size(), nullptr, GL_DYNAMIC_COPY);
    out.fuze_.allocate(GL_SHADER_STORAGE_BUFFER, fuze_.size(), nullptr, GL_DYNAMIC_COPY);
    if (glGetError() == GL_OUT_OF_MEMORY) {
        out.release();
        return false;
    }
    // The steps' writes are in before the copy reads them.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    for (size_t i = 0; i < chunks_.size(); ++i) copyBuffer(chunks_[i].buf, out.particles_[i]);
    copyBuffer(detonation_, out.detonation_);
    copyBuffer(fuze_, out.fuze_);
    gl::check("MPM save state");
    out.steps_ = steps_;
    out.valid_ = true;
    return true;
}

bool MpmSolver::restoreState(const MpmState& state) {
    if (!loaded_ || !state.valid_ || plan_.spilledChunks > 0 || state.particles_.size() != chunks_.size() ||
        state.detonation_.size() != detonation_.size() || state.fuze_.size() != fuze_.size())
        return false;
    for (size_t i = 0; i < chunks_.size(); ++i)
        if (state.particles_[i].size() != chunks_[i].buf.size()) return false;
    dropMeasure();
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    for (size_t i = 0; i < chunks_.size(); ++i) copyBuffer(state.particles_[i], chunks_[i].buf);
    copyBuffer(state.detonation_, detonation_);
    copyBuffer(state.fuze_, fuze_);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    gl::check("MPM restore state");
    steps_ = state.steps_;
    return true;
}

void MpmSolver::ignite(double x, double y, int layer) {
    if (!loaded_) return;
    // Readable from the next step on (the shader compares with the 1-based step number).
    const MpmDetonation d{static_cast<uint32_t>(steps_),
                          static_cast<uint32_t>(std::max(layer, 0)),
                          {static_cast<float>((x - scene_.originX) / scene_.dx),
                           static_cast<float>((y - scene_.originY) / scene_.dx)},
                          static_cast<float>(time()),
                          {0, 0, 0}};
    detonation_.update(&d, sizeof(d));
    gl::check("MPM ignite");
}

std::vector<MpmParticleState> MpmSolver::readParticles() const {
    std::vector<MpmParticleState> out;
    if (!loaded_) return out;
    out.reserve(static_cast<size_t>(particleCount()));
    // Read in slices: a particle's own memory is 144 bytes, and a whole copy of it beside the
    // converted list would double the peak.
    constexpr size_t kReadSlice = size_t(1) << 20;
    std::vector<MpmParticle> raw(std::min(static_cast<size_t>(particleCount()), kReadSlice));
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    for (const Chunk& c : chunks_) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, c.buf.id());
        for (size_t at = 0; at < c.count; at += raw.size()) {
            const size_t count = std::min(raw.size(), c.count - at);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(at * sizeof(MpmParticle)),
                               static_cast<GLsizeiptr>(count * sizeof(MpmParticle)), raw.data());
            for (size_t k = 0; k < count; ++k) {
                const MpmParticle& p = raw[k];
                MpmParticleState s;
                s.x = scene_.originX + (static_cast<double>(p.x[0]) + p.xlo[0]) * scene_.dx;
                s.y = scene_.originY + (static_cast<double>(p.x[1]) + p.xlo[1]) * scene_.dx;
                s.vx = p.v[0];
                s.vy = p.v[1];
                s.burn = p.burn;
                s.temperature = p.temp;
                s.pressure = p.pressure;
                s.plasticStrain = p.epsP;
                s.material = static_cast<int>(p.mat);
                s.flags = p.flags;
                s.fracture = p.fracture;
                s.fibreStretch = std::hypot(p.fibre[0], p.fibre[1]);
                out.push_back(s);
            }
        }
    }
    gl::check("MPM read particles");
    return out;
}

void MpmSolver::setCommon(const gl::Program& p) const {
    p.use();
    glUniform2i(p.loc("uN"), scene_.nx, scene_.ny);
    p.f("uDx", static_cast<float>(scene_.dx));
    p.f("uDt", static_cast<float>(scene_.dt));
    p.f("uMassRef", static_cast<float>(massRef_));
    p.f("uVelRef", static_cast<float>(velRef_));
}

void MpmSolver::setChunk(const gl::Program& p, const Chunk& c) const {
    glUniform1ui(p.loc("uCount"), static_cast<GLuint>(c.count));
    glUniform1ui(p.loc("uBase"), static_cast<GLuint>(c.first));
}

// A pass calls this before each chunk it reads (and storeChunk after each it writes). The window
// holds one chunk at a time, so each spilled chunk is copied up on its turn, unless the pass before
// left it there.
void MpmSolver::loadChunk(size_t i) {
    Chunk& c = chunks_[i];
    if (!c.spilled) {
        c.buf.bindBase(GL_SHADER_STORAGE_BUFFER, 0);
        return;
    }
    if (windowHolds_ != static_cast<int>(i)) {
        // The pass before is done with the window, and its writes are in, before it is overwritten.
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        copyBytes(c.buf, window_, c.count * sizeof(MpmParticle));
        windowHolds_ = static_cast<int>(i);
    }
    window_.bindBase(GL_SHADER_STORAGE_BUFFER, 0);
}

void MpmSolver::storeChunk(size_t i) {
    const Chunk& c = chunks_[i];
    if (!c.spilled) return;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    copyBytes(window_, c.buf, c.count * sizeof(MpmParticle));
}

const gl::Buffer& MpmSolver::chunkForReading(int i) {
    const size_t k = static_cast<size_t>(i);
    if (!chunks_[k].spilled) return chunks_[k].buf;
    if (windowHolds_ != i) {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        copyBytes(chunks_[k].buf, window_, chunks_[k].count * sizeof(MpmParticle));
        windowHolds_ = i;
    }
    return window_;
}

void MpmSolver::step() {
    if (!loaded_) return;
    grid_.bindBase(GL_SHADER_STORAGE_BUFFER, 1);
    gridVelocity_.bindBase(GL_SHADER_STORAGE_BUFFER, 2);
    materials_.bindBase(GL_SHADER_STORAGE_BUFFER, 3);
    detonation_.bindBase(GL_SHADER_STORAGE_BUFFER, 5);
    fuze_.bindBase(GL_SHADER_STORAGE_BUFFER, 6);

    // This step's fuze sum starts from zero.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, fuze_.id());
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, static_cast<GLintptr>(8 * ((steps_ + 1) & 1)), 8, GL_RED_INTEGER,
                         GL_UNSIGNED_INT, nullptr);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, grid_.id());
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    setCommon(p2g_);
    glUniform2f(p2g_.loc("uTether"), static_cast<float>(scene_.mountOmega), static_cast<float>(scene_.mountDamping));
    for (size_t i = 0; i < chunks_.size(); ++i) {
        loadChunk(i);
        setChunk(p2g_, chunks_[i]);
        glDispatchCompute(groups(static_cast<int>(chunks_[i].count), kParticleGroup), 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    setCommon(gridUpdate_);
    glDispatchCompute(groups(scene_.nx, kGridGroup), groups(scene_.ny, kGridGroup), 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    setCommon(g2p_);
    g2p_.f("uStrength", static_cast<float>(strength_));
    g2p_.f("uTime", static_cast<float>(scene_.tStart + (steps_ + 1) * scene_.dt));
    glUniform1ui(g2p_.loc("uStep"), static_cast<GLuint>(steps_ + 1));
    g2p_.f("uJetBreakup", static_cast<float>(std::min(scene_.jetBreakup, 1e30)));
    const phys::FuzeSpec& fz = scene_.fuze;
    const bool fuzed = fz.type != phys::FuzeType::None && scene_.fuzeTotalUnits > 0;
    g2p_.f("uImpactVelocity", static_cast<float>(scene_.velocity));
    g2p_.f("uFuzeMassUnit", static_cast<float>(fuzed ? scene_.fuzeMassUnit : 1));
    g2p_.f("uFuzeTotalUnits", static_cast<float>(fuzed ? scene_.fuzeTotalUnits : 0));
    g2p_.f("uTriggerDv", static_cast<float>(fuzed ? fz.triggerDv : 1e30));
    g2p_.f("uFuzeDelay", static_cast<float>(fz.type == phys::FuzeType::NoseContact ? fz.functionTime : fz.delay));
    glUniform2f(g2p_.loc("uFuzeOrigin"), static_cast<float>(scene_.fuzeOrigin[0]), static_cast<float>(scene_.fuzeOrigin[1]));
    // Taylor's tilt V sin(theta) = V^2 / (2 D) of the case beside the charge, with V the Gurney speed.
    const double tilt = fuzed && scene_.burstSpeed > 0 ? scene_.burstGurney * std::sin(scene_.burstTaylor) : 0;
    g2p_.f("uBurstTilt", static_cast<float>(tilt));
    g2p_.f("uBurstSweep", static_cast<float>(scene_.burstSweep));
    g2p_.f("uBurstSpeed", static_cast<float>(std::max(scene_.burstSpeed, 1.0)));
    glUniform2f(g2p_.loc("uBurstSpan"), static_cast<float>(scene_.burstSpan[0]), static_cast<float>(scene_.burstSpan[1]));
    // The chunks one after the other, the first one first: a detonation front that two chunks light
    // in the same step starts in the earlier one, whatever the memory they live in.
    for (size_t i = 0; i < chunks_.size(); ++i) {
        loadChunk(i);
        setChunk(g2p_, chunks_[i]);
        glDispatchCompute(groups(static_cast<int>(chunks_[i].count), kParticleGroup), 1, 1);
        if (chunks_[i].spilled)
            storeChunk(i);
        else if (i + 1 < chunks_.size())
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);  // the next chunk sees the front and fuze this one set
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    ++steps_;
}

void MpmSolver::collectTimers() {
    for (StepTimer& t : timers_) {
        if (!t.pending) continue;
        GLint available = 0;
        glGetQueryObjectiv(t.end, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) continue;
        GLuint64 a = 0, b = 0;
        glGetQueryObjectui64v(t.begin, GL_QUERY_RESULT, &a);
        glGetQueryObjectui64v(t.end, GL_QUERY_RESULT, &b);
        t.pending = false;
        const double ms = static_cast<double>(b - a) * 1e-6 / t.steps;
        msPerStep_ = msPerStep_ > 0 ? 0.7 * msPerStep_ + 0.3 * ms : ms;
    }
}

bool MpmSolver::advanceTo(double t, double budgetMs) {
    if (!loaded_) return true;
    // The CPU only queues the steps, so the GPU's own time per step sets how many fit the
    // budget (1 ms per step is assumed until a call has been timed).
    collectTimers();
    const double cap = std::max(1.0, budgetMs / (msPerStep_ > 0 ? msPerStep_ : 1.0));
    StepTimer* timer = nullptr;
    for (StepTimer& s : timers_)
        if (!s.pending) {
            timer = &s;
            break;
        }
    if (timer) {
        if (!timer->begin) {
            glGenQueries(1, &timer->begin);
            glGenQueries(1, &timer->end);
        }
        glQueryCounter(timer->begin, GL_TIMESTAMP);
    }
    const auto start = std::chrono::steady_clock::now();
    int n = 0;
    while (time() + 0.5 * scene_.dt < t && n < cap) {
        step();
        // Check the clock every few steps; glFlush keeps the queue moving.
        if (++n % 16 == 0) {
            glFlush();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms > budgetMs) break;
        }
    }
    if (timer && n > 0) {
        glQueryCounter(timer->end, GL_TIMESTAMP);
        timer->steps = n;
        timer->pending = true;
    }
    return time() + 0.5 * scene_.dt >= t;
}

void MpmSolver::dispatchMeasure() {
    materials_.bindBase(GL_SHADER_STORAGE_BUFFER, 3);
    partials_.bindBase(GL_SHADER_STORAGE_BUFFER, 4);
    setCommon(measure_);
    for (size_t i = 0; i < chunks_.size(); ++i) {
        loadChunk(i);
        setChunk(measure_, chunks_[i]);
        glDispatchCompute(groups(static_cast<int>(chunks_[i].count), kParticleGroup), 1, 1);
    }
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
}

void MpmSolver::dropMeasure() {
    if (!measureFence_) return;
    glDeleteSync(measureFence_);
    measureFence_ = nullptr;
}

MpmMeasure MpmSolver::measure() {
    if (!loaded_) return {};
    dropMeasure();
    dispatchMeasure();
    return readMeasure(time());
}

void MpmSolver::requestMeasure() {
    if (!loaded_ || measureFence_) return;
    dispatchMeasure();
    measureTime_ = time();
    measureFence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool MpmSolver::pollMeasure(MpmMeasure& out) {
    if (!measureFence_) return false;
    const GLenum state = glClientWaitSync(measureFence_, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED) return false;
    dropMeasure();
    out = readMeasure(measureTime_);
    return true;
}

MpmMeasure MpmSolver::readMeasure(double time) {
    MpmMeasure m;
    const GLuint pg = groups(particleCount(), kParticleGroup);
    std::vector<float> data(static_cast<size_t>(pg) * kMeasureVec4s * 4);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, partials_.id());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());

    double nose = -1e30, tail = 1e30, sumVx = 0, reach = -1e30, lead = -1e30;
    double intact = 0, retired = 0, failed = 0;
    for (GLuint g = 0; g < pg; ++g) {
        const float* s = &data[static_cast<size_t>(g) * kMeasureVec4s * 4];
        nose = std::max(nose, static_cast<double>(s[0]));
        tail = std::min(tail, static_cast<double>(s[1]));
        intact += s[2];
        sumVx += s[3];
        m.momentumX += s[4];
        m.momentumY += s[5];
        retired += s[6];
        m.maxSpeed = std::max(m.maxSpeed, static_cast<double>(s[7]));
        failed += s[8];
        m.maxTemperature = std::max(m.maxTemperature, static_cast<double>(s[9]));
        m.maxStress = std::max(m.maxStress, static_cast<double>(s[10]));
        m.maxPlasticStrain = std::max(m.maxPlasticStrain, static_cast<double>(s[11]));
        reach = std::max(reach, static_cast<double>(s[12]));
        m.failedStrain += static_cast<int>(s[13]);
        m.failedSpall += static_cast<int>(s[14]);
        m.failedMelt += static_cast<int>(s[15]);
        m.reacted += static_cast<int>(s[16]);
        m.plateSpeed = std::max(m.plateSpeed, static_cast<double>(s[17]));
        m.comminuted += static_cast<int>(s[18]);
        m.delaminated += static_cast<int>(s[19]);
        m.fibreBroken += static_cast<int>(s[20]);
        lead = std::max(lead, static_cast<double>(s[21]));
        m.jetBroken += static_cast<int>(s[22]);
        m.fillerLit += static_cast<int>(s[23]);
    }
    m.valid = true;
    m.time = time;
    m.intact = static_cast<int>(intact);
    m.failedPlate = static_cast<int>(failed);
    m.retired = static_cast<int>(retired);
    if (lead > -1e29) m.leadVelocity = lead;
    MpmFuze fz{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, fuze_.id());
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(fz), &fz);
    m.fuzeTriggered = fz.step != kNoFront;
    if (m.fuzeTriggered) m.fuzeTime = fz.time;
    if (reach > -1e29) m.reach = scene_.originX + reach * scene_.dx + 0.5 * scene_.spacing;
    if (m.intact > 0) {
        // Particle centres sit half a spacing inside the material's edges.
        const double half = 0.5 * scene_.spacing;
        m.nose = scene_.originX + nose * scene_.dx + half;
        m.tail = scene_.originX + tail * scene_.dx - half;
        m.velocity = sumVx / intact;
        const double area = scene_.bodyCount > 0 ? scene_.projectileArea / scene_.bodyCount : 0;
        m.residualLength = intact * area / std::max(scene_.diameter, 1e-9);
    }
    gl::check("MPM measure");
    return m;
}
