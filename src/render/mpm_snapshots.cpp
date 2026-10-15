#include "render/mpm_snapshots.h"

#include "core/log.h"
#include "core/sysmem.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr size_t kFrameParticleCap = size_t(16) << 20;  // beyond this a frame is too slow to draw, stride grows
constexpr int kMaxAttempts = 10;

}  // namespace

MpmSnapshotLimits MpmSnapshots::limitsForThisMachine(bool useSystemMemory) {
    MpmSnapshotLimits l;
    l.gpuBytes = mpmSnapshotBudget(gl::freeVideoBytes());
    l.hostBytes = useSystemMemory ? mpmSnapshotHostBudget(freeHostBytes()) : 0;
    l.maxFrames = kMaxFrames;
    return l;
}

void MpmSnapshots::init(const std::filesystem::path& root) {
    pack_.compute(root / "shaders" / "mpm" / "pack.comp");
}

void MpmSnapshots::begin(const MpmSolver& solver, double t0, double t1, const MpmSnapshotLimits& limits) {
    clear();
    if (!pack_.valid()) return;
    count_ = solver.particleCount();
    if (count_ <= 0) return;
    GLint align = 256;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
    const size_t a = static_cast<size_t>(std::max(align, 1));
    size_t maxFrameParticles = limits.maxFrameParticles;
    if (maxFrameParticles == 0) {
        const size_t block = gl::maxStorageBlockBytes();
        maxFrameParticles = block ? std::min(block / kBytesPerParticle, kFrameParticleCap) : kFrameParticleCap;
    }
    size_t gpuBytes = limits.gpuBytes, hostBytes = limits.hostBytes;
    // A card or computer short of memory gets fewer, or coarser, frames.
    for (int attempt = 0;; ++attempt) {
        if (attempt == kMaxAttempts) {
            logx::warn("pre-render: out of memory");
            clear();
            return;
        }
        plan_ = mpmPlanSnapshots(static_cast<size_t>(count_), kBytesPerParticle, gpuBytes, hostBytes, limits.minFrames,
                                 limits.maxFrames, maxFrameParticles);
        slotBytes_ = (plan_.frameBytes + a - 1) / a * a;
        const int cacheSlots = plan_.hostFrames() > 0 ? kMpmSnapshotCacheSlots : 0;
        while (glGetError() != GL_NO_ERROR) {
        }
        store_.allocate(GL_SHADER_STORAGE_BUFFER, slotBytes_ * static_cast<size_t>(plan_.gpuFrames + cacheSlots), nullptr,
                        GL_DYNAMIC_COPY);
        if (glGetError() == GL_OUT_OF_MEMORY) {
            gpuBytes /= 2;
            continue;
        }
        host_.clear();
        try {
            for (int i = 0; i < plan_.hostFrames(); ++i) host_.push_back(std::make_unique_for_overwrite<uint8_t[]>(plan_.frameBytes));
        } catch (const std::bad_alloc&) {
            host_.clear();
            hostBytes /= 2;
            continue;
        }
        break;
    }
    times_ = mpmSnapshotTimes(t0, t1, plan_.frames);
    measures_.assign(times_.size(), MpmMeasure{});
    taken_ = 0;
    cached_[0] = cached_[1] = -1;
    cacheNext_ = 0;
    logx::info("pre-render: %d frames of %zu particles (one in %d), %d in video memory, %d in system memory", plan_.frames,
               plan_.frameParticles, plan_.stride, plan_.gpuFrames, plan_.hostFrames());
}

void MpmSnapshots::pack(MpmSolver& solver, size_t slot) {
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 7, store_.id(), static_cast<GLintptr>(slotOffset(slot)),
                      static_cast<GLsizeiptr>(plan_.frameBytes));
    const size_t stride = static_cast<size_t>(plan_.stride);
    for (int c = 0; c < solver.chunkCount(); ++c) {
        const size_t first = solver.chunkFirst(c), count = solver.chunkSize(c);
        if (count == 0) continue;
        // Chunks wait in system memory, a spilled one is copied up when it is asked for.
        solver.chunkForReading(c).bindBase(GL_SHADER_STORAGE_BUFFER, 0);
        pack_.use();
        glUniform1ui(pack_.loc("uCount"), static_cast<GLuint>(count));
        glUniform1ui(pack_.loc("uBase"), static_cast<GLuint>(first));
        glUniform1ui(pack_.loc("uStride"), static_cast<GLuint>(stride));
        glUniform1ui(pack_.loc("uTotal"), static_cast<GLuint>(count_));
        // The slots whose blocks of `stride` particles overlap the chunk.
        const size_t blocks = (first + count - 1) / stride - first / stride + 1;
        glDispatchCompute(static_cast<GLuint>((blocks + 255) / 256), 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
}

bool MpmSnapshots::capture(MpmSolver& solver) {
    if (!active()) return false;
    const double half = 0.5 * solver.scene().dt;
    while (!complete() && solver.time() + half >= times_[static_cast<size_t>(taken_)]) {
        const size_t i = static_cast<size_t>(taken_);
        const bool waits = static_cast<int>(i) >= plan_.gpuFrames;
        // A frame for system memory is packed into the first cache slot and read back from it.
        pack(solver, waits ? static_cast<size_t>(plan_.gpuFrames) : i);
        if (waits) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, store_.id());
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(slotOffset(static_cast<size_t>(plan_.gpuFrames))),
                               static_cast<GLsizeiptr>(plan_.frameBytes), host_[i - static_cast<size_t>(plan_.gpuFrames)].get());
            cached_[0] = -1;  // the slot was overwritten
        }
        gl::check("MPM pack frame");
        measures_[i] = solver.measure();
        // A frame shows the solver's own time, which may differ from the plan by half a step.
        times_[i] = std::max(solver.time(), i > 0 ? times_[i - 1] : solver.time());
        ++taken_;
    }
    return complete();
}

void MpmSnapshots::clear() {
    times_.clear();
    measures_.clear();
    host_.clear();
    host_.shrink_to_fit();
    taken_ = 0;
    count_ = 0;
    slotBytes_ = 0;
    plan_ = MpmSnapshotPlan{};
    cached_[0] = cached_[1] = -1;
    cacheNext_ = 0;
    if (store_.id()) store_.allocate(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_COPY);  // release the memory
}

int MpmSnapshots::frameAt(double t) const {
    std::vector<double> got(times_.begin(), times_.begin() + taken_);
    return mpmPickFrame(got, t).a;
}

MpmMeasure MpmSnapshots::measureAt(double t) const {
    const int a = frameAt(t);
    if (a < 0) return {};
    MpmMeasure m = measures_[static_cast<size_t>(a)];
    for (int i = 0; i < a; ++i)
        if (measures_[static_cast<size_t>(i)].valid) m.reach = std::max(m.reach, measures_[static_cast<size_t>(i)].reach);
    return m;
}

std::vector<MpmMeasure> MpmSnapshots::measures() const {
    return std::vector<MpmMeasure>(measures_.begin(), measures_.begin() + taken_);
}

std::vector<MpmParticleState> MpmSnapshots::readFrame(double t, const MpmScene& scene) const {
    std::vector<MpmParticleState> out;
    const int a = frameAt(t);
    if (a < 0 || plan_.frameParticles == 0) return out;
    std::vector<MpmFrameWords> raw(plan_.frameParticles);
    if (a >= plan_.gpuFrames) {
        std::memcpy(raw.data(), host_[static_cast<size_t>(a - plan_.gpuFrames)].get(), raw.size() * sizeof(MpmFrameWords));
    } else {
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, store_.id());
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(slotOffset(static_cast<size_t>(a))),
                           static_cast<GLsizeiptr>(raw.size() * sizeof(MpmFrameWords)), raw.data());
        gl::check("MPM read frame");
    }
    out.reserve(raw.size());
    for (const MpmFrameWords& f : raw) out.push_back(mpmUnpackFrame(f, scene));
    return out;
}

size_t MpmSnapshots::slotOf(int f, int keep) {
    if (f < plan_.gpuFrames) return static_cast<size_t>(f);
    for (int c = 0; c < kMpmSnapshotCacheSlots; ++c)
        if (cached_[c] == f) return static_cast<size_t>(plan_.gpuFrames + c);
    // The slot to fill: an empty one, else the one that does not hold the frame to keep.
    int c = cached_[0] < 0 ? 0 : cached_[1] < 0 ? 1 : cacheNext_;
    if (cached_[c] == keep && keep >= 0) c = 1 - c;
    const size_t slot = static_cast<size_t>(plan_.gpuFrames + c);
    store_.update(host_[static_cast<size_t>(f - plan_.gpuFrames)].get(), plan_.frameBytes, slotOffset(slot));
    cached_[c] = f;
    cacheNext_ = 1 - c;  // next time the other one goes first
    return slot;
}

float MpmSnapshots::bind(double t) {
    std::vector<double> got(times_.begin(), times_.begin() + taken_);
    const MpmFramePick p = mpmPickFrame(got, t);
    if (p.a < 0) return 0;
    const size_t sa = slotOf(p.a, -1);
    const size_t sb = p.b == p.a ? sa : slotOf(p.b, p.a);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 7, store_.id(), static_cast<GLintptr>(slotOffset(sa)),
                      static_cast<GLsizeiptr>(plan_.frameBytes));
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 8, store_.id(), static_cast<GLintptr>(slotOffset(sb)),
                      static_cast<GLsizeiptr>(plan_.frameBytes));
    return static_cast<float>(p.lerp);
}
