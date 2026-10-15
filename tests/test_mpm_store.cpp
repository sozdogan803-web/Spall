#include "test.h"

#include "sim/mpm_frames.h"
#include "sim/mpm_store.h"

#include <cmath>
#include <string>

namespace {

constexpr size_t kGiB = size_t(1) << 30;
constexpr size_t kMiB = size_t(1) << 20;

MpmStoreLimits limits(size_t freeVideo, size_t freeHost, size_t block = 2 * kGiB) {
    MpmStoreLimits l;
    l.freeVideoBytes = freeVideo;
    l.freeHostBytes = freeHost;
    l.maxBlockBytes = block;
    return l;
}

}  // namespace

TEST(mpm_store_small_scene_is_one_resident_chunk) {
    const MpmStorePlan p = mpmPlanStore(1000000, 1500, 800, limits(8 * kGiB, 16 * kGiB));
    CHECK(p.ok);
    CHECK(p.problem.empty());
    CHECK(p.chunks == 1);
    CHECK(p.residentChunks == 1 && p.spilledChunks == 0);
    CHECK(p.chunkCount(0) == 1000000);
    CHECK(p.hostBytes == 0);
    CHECK(p.videoBytes == mpmVideoBytes(1000000, 1500, 800));
    CHECK(p.windowBytes == 0);
}

TEST(mpm_store_chunks_are_aligned_and_cover_every_particle) {
    const size_t n = 20000000;
    const MpmStorePlan p = mpmPlanStore(n, 3000, 3000, limits(64 * kGiB, 16 * kGiB));
    CHECK(p.ok);
    // 512 MiB of 144-byte particles, rounded down to whole 256-particle groups.
    CHECK(p.chunkParticles == (512 * kMiB / kMpmParticleBytes) / 256 * 256);
    CHECK(p.chunkParticles % 256 == 0);
    CHECK(p.chunkParticles * kMpmParticleBytes <= 512 * kMiB);
    CHECK(p.chunks == static_cast<int>((n + p.chunkParticles - 1) / p.chunkParticles));
    size_t total = 0;
    for (int i = 0; i < p.chunks; ++i) {
        CHECK(p.chunkFirst(i) == total);
        CHECK(p.chunkFirst(i) % 256 == 0);
        CHECK(p.chunkCount(i) > 0);
        if (i + 1 < p.chunks) CHECK(p.chunkCount(i) == p.chunkParticles);
        total += p.chunkCount(i);
    }
    CHECK(total == n);
    // Every chunk starts on the storage-buffer offset alignment (at most 256 bytes) of the card.
    CHECK((p.chunkParticles * kMpmParticleBytes) % 256 == 0);
}

TEST(mpm_store_chunk_is_never_over_the_block_limit) {
    const MpmStorePlan p = mpmPlanStore(20000000, 3000, 3000, limits(64 * kGiB, 16 * kGiB, 256 * kMiB));
    CHECK(p.ok);
    CHECK(p.chunkParticles * kMpmParticleBytes <= 256 * kMiB);
    CHECK(p.chunks > 8);
    // Unknown limit: the target chunk size.
    const MpmStorePlan u = mpmPlanStore(20000000, 3000, 3000, limits(64 * kGiB, 16 * kGiB, 0));
    CHECK(u.ok);
    CHECK(u.chunkParticles * kMpmParticleBytes <= 512 * kMiB);
}

TEST(mpm_store_chunk_size_can_be_capped_below_the_card_limit) {
    // A small scene on a big card is cut up on request (the self-test runs every chunk path that way).
    MpmStoreLimits l = limits(64 * kGiB, 16 * kGiB);
    l.maxChunkBytes = 1000 * 256 * kMpmParticleBytes;  // 256000 particles
    const MpmStorePlan p = mpmPlanStore(1000000, 1500, 800, l);
    CHECK(p.ok);
    CHECK(p.chunkParticles == 256000);
    CHECK(p.chunks == 4);
    CHECK(p.chunkCount(3) == 1000000 - 3 * 256000);
    // A cap over the card's block limit does not raise it, and zero leaves the 512 MiB default.
    l.maxChunkBytes = 4 * kGiB;
    CHECK(mpmPlanStore(20000000, 3000, 3000, l).chunkParticles * kMpmParticleBytes <= 512 * kMiB);
    l.maxChunkBytes = 0;
    CHECK(mpmPlanStore(20000000, 3000, 3000, l).chunkParticles == (512 * kMiB / kMpmParticleBytes) / 256 * 256);
}

TEST(mpm_store_fits_when_it_all_fits_video_memory) {
    // 100 M particles are 14.4 GB: all in video memory on a card with 20 GiB free, in chunks.
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, limits(20 * kGiB, 16 * kGiB));
    CHECK(p.ok);
    CHECK(p.spilledChunks == 0);
    CHECK(p.residentChunks == p.chunks);
    CHECK(p.hostBytes == 0 && p.windowBytes == 0);
    CHECK(p.videoBytes == mpmVideoBytes(100000000, 6144, 6144));
}

TEST(mpm_store_spills_the_overflow_to_host_memory) {
    // The same scene on a 16 GiB card (13.44 GiB free): the tail of the chunks stays in system memory.
    const size_t freeVideo = static_cast<size_t>(13.44 * static_cast<double>(kGiB));
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, limits(freeVideo, 20 * kGiB));
    CHECK(p.ok);
    CHECK(p.spilledChunks > 0);
    CHECK(p.residentChunks > 0);
    CHECK(p.residentChunks + p.spilledChunks == p.chunks);
    CHECK(p.videoBytes <= static_cast<size_t>(0.85 * static_cast<double>(freeVideo)));
    // The streaming window is one chunk of video memory, counted in the total.
    CHECK(p.windowBytes == p.chunkParticles * kMpmParticleBytes);
    // The last chunks are the spilled ones; the host holds exactly their bytes.
    size_t spilled = 0;
    for (int i = 0; i < p.chunks; ++i) {
        CHECK(p.spilled(i) == (i >= p.residentChunks));
        if (p.spilled(i)) spilled += p.chunkCount(i) * kMpmParticleBytes;
    }
    CHECK(p.hostBytes == spilled);
    // Everything is accounted for: resident particles, grid, partials and window in video memory.
    size_t resident = 0;
    for (int i = 0; i < p.residentChunks; ++i) resident += p.chunkCount(i) * kMpmParticleBytes;
    CHECK(p.videoBytes ==
          resident + p.windowBytes + (mpmVideoBytes(100000000, 6144, 6144) - 100000000 * kMpmParticleBytes));
}

TEST(mpm_store_spills_as_little_as_it_can) {
    const size_t freeVideo = static_cast<size_t>(13.44 * static_cast<double>(kGiB));
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, limits(freeVideo, 20 * kGiB));
    CHECK(p.ok);
    // One more resident chunk would not fit the 85 % of the card.
    const size_t more = p.videoBytes + p.chunkCount(p.residentChunks) * kMpmParticleBytes;
    CHECK(more > static_cast<size_t>(0.85 * static_cast<double>(freeVideo)));
}

TEST(mpm_store_without_spill_names_the_video_memory) {
    MpmStoreLimits l = limits(7 * kGiB, 20 * kGiB);
    l.allowSpill = false;
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, l);
    CHECK(!p.ok);
    CHECK(p.problem.find("video memory") != std::string::npos);
    // Allowed to spill, the same scene is planned (the card has 7 GiB free: 5.95 usable).
    const MpmStorePlan s = mpmPlanStore(100000000, 6144, 6144, limits(7 * kGiB, 20 * kGiB));
    CHECK(s.ok);
    CHECK(s.spilledChunks > 0);
}

TEST(mpm_store_names_short_system_memory) {
    // 14.4 GB of particles with 6 GiB of video memory leaves about 9 GB for the host: 6 GiB free is short.
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, limits(6 * kGiB, 6 * kGiB));
    CHECK(!p.ok);
    CHECK(p.problem.find("system memory") != std::string::npos);
    // Unknown free system memory cannot be spilled to.
    const MpmStorePlan u = mpmPlanStore(100000000, 6144, 6144, limits(6 * kGiB, 0));
    CHECK(!u.ok);
    CHECK(u.problem.find("system memory") != std::string::npos);
}

TEST(mpm_store_takes_at_most_sixty_percent_of_free_system_memory) {
    const size_t freeVideo = 6 * kGiB;
    const MpmStorePlan open = mpmPlanStore(100000000, 6144, 6144, limits(freeVideo, 64 * kGiB));
    CHECK(open.ok);
    const size_t host = open.hostBytes;
    CHECK(host > 0);
    // Free memory just under and just over host / 0.6.
    CHECK(!mpmPlanStore(100000000, 6144, 6144, limits(freeVideo, static_cast<size_t>(static_cast<double>(host) / 0.61))).ok);
    CHECK(mpmPlanStore(100000000, 6144, 6144, limits(freeVideo, static_cast<size_t>(static_cast<double>(host) / 0.59))).ok);
}

TEST(mpm_store_unknown_video_memory_keeps_it_all_resident) {
    // A card that cannot report its memory (not NVIDIA): nothing is spilled, nothing refused for size.
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, limits(0, 16 * kGiB));
    CHECK(p.ok);
    CHECK(p.spilledChunks == 0);
    CHECK(p.residentChunks == p.chunks);
}

TEST(mpm_store_grid_over_the_block_limit_is_refused) {
    // 16 bytes a node: a 6144 x 6144 grid is 604 MB, over a 512 MiB block.
    const MpmStorePlan p = mpmPlanStore(1000, 6144, 6144, limits(64 * kGiB, 16 * kGiB, 512 * kMiB));
    CHECK(!p.ok);
    CHECK(p.problem.find("buffer") != std::string::npos);
    CHECK(mpmPlanStore(1000, 6144, 6144, limits(64 * kGiB, 16 * kGiB, 2 * kGiB)).ok);
}

TEST(mpm_store_grid_alone_over_video_memory_is_refused) {
    // Even fully streamed the grid and window must fit.
    const MpmStorePlan p = mpmPlanStore(100000000, 6144, 6144, limits(kGiB, 40 * kGiB));
    CHECK(!p.ok);
    CHECK(p.problem.find("video memory") != std::string::npos);
}

TEST(mpm_store_small_card_keeps_what_it_can_resident) {
    // 2 GiB free: 1.7 usable; the grid takes 0.22 GB and the streaming window 0.54, so one chunk stays.
    const MpmStorePlan p = mpmPlanStore(20000000, 3000, 3000, limits(2 * kGiB, 40 * kGiB));
    CHECK(p.ok);
    CHECK(p.residentChunks <= p.chunks);
    CHECK(p.residentChunks + p.spilledChunks == p.chunks);
    CHECK(p.videoBytes <= static_cast<size_t>(0.85 * 2.0 * static_cast<double>(kGiB)));
}

TEST(mpm_store_zero_particles_plans_nothing) {
    const MpmStorePlan p = mpmPlanStore(0, 100, 100, limits(8 * kGiB, 16 * kGiB));
    CHECK(p.chunks == 0);
    CHECK(p.hostBytes == 0);
}

TEST(mpm_snapshot_plan_small_scene_is_all_in_video_memory) {
    // 150 k particles: 6 MB a frame; 240 frames are 1.4 GB, under the 4 GiB budget.
    const MpmSnapshotPlan p = mpmPlanSnapshots(150000, 40, 4 * kGiB, 8 * kGiB, 60, 240, 6000000);
    CHECK(p.stride == 1);
    CHECK(p.frames == 240);
    CHECK(p.gpuFrames == 240);
    CHECK(p.hostFrames() == 0);
    CHECK(p.frameParticles == 150000);
}

TEST(mpm_snapshot_plan_spills_frames_to_host_memory) {
    // 1.6 M particles: 64 MB a frame; 4 GiB of video memory holds 64 (62 with two playback slots),
    // 8 GiB of host memory 128 more.
    const MpmSnapshotPlan p = mpmPlanSnapshots(1600000, 40, 4 * kGiB, 8 * kGiB, 60, 240, 6000000);
    CHECK(p.stride == 1);
    CHECK(p.frames > 100 && p.frames <= 240);
    CHECK(p.gpuFrames > 0 && p.gpuFrames < p.frames);
    CHECK(p.hostFrames() == p.frames - p.gpuFrames);
    CHECK(p.hostFrames() * p.frameBytes <= 8 * kGiB);
    CHECK((p.gpuFrames + 2) * p.frameBytes <= 4 * kGiB);
}

TEST(mpm_snapshot_plan_decimates_a_frame_over_the_particle_cap) {
    // Insane: 11 M particles. A frame keeps at most 6 M, every third here to reach 60 frames.
    const MpmSnapshotPlan p = mpmPlanSnapshots(11000000, 40, 4 * kGiB, 8 * kGiB, 60, 240, 6000000);
    CHECK(p.stride >= 2);
    CHECK(p.frameParticles == (11000000 + static_cast<size_t>(p.stride) - 1) / static_cast<size_t>(p.stride));
    CHECK(p.frameParticles <= 6000000);
    CHECK(p.frames >= 60);
    CHECK(p.frameBytes == p.frameParticles * 40);
}

TEST(mpm_snapshot_plan_ludicrous_gets_a_usable_number_of_frames) {
    const MpmSnapshotPlan p = mpmPlanSnapshots(83400000, 40, 4 * kGiB, 8 * kGiB, 60, 240, 6000000);
    CHECK(p.stride >= 14);
    CHECK(p.stride <= 32);
    CHECK(p.frameParticles <= 6000000);
    CHECK(p.frames >= 60);
}

TEST(mpm_snapshot_plan_keeps_every_particle_when_frames_are_enough) {
    // 3 M particles: 120 MB a frame; the 12 GiB of budget holds 100: no decimation needed.
    const MpmSnapshotPlan p = mpmPlanSnapshots(3000000, 40, 4 * kGiB, 8 * kGiB, 60, 240, 6000000);
    CHECK(p.stride == 1);
    CHECK(p.frames >= 60);
}

TEST(mpm_snapshot_plan_never_has_fewer_than_both_ends) {
    // No budget at all and a huge scene: two frames at the maximum decimation.
    const MpmSnapshotPlan p = mpmPlanSnapshots(100000000, 40, 0, 0, 60, 240, 6000000);
    CHECK(p.frames == 2);
    CHECK(p.stride == 32);
    CHECK(mpmPlanSnapshots(0, 40, kGiB, kGiB, 60, 240, 6000000).frames == 0);
}

TEST(mpm_snapshot_plan_without_host_memory_is_all_video) {
    const MpmSnapshotPlan p = mpmPlanSnapshots(1600000, 40, 4 * kGiB, 0, 60, 240, 6000000);
    CHECK(p.hostFrames() == 0);
    CHECK(p.gpuFrames == p.frames);
    CHECK(p.frames == 67);  // 4 GiB over 64 MB a frame
}

TEST(mpm_snapshot_host_budget_is_a_share_of_free_memory) {
    CHECK(mpmSnapshotHostBudget(0) == 0);                    // unknown: none
    CHECK(mpmSnapshotHostBudget(10 * kGiB) == 5 * kGiB);     // half of what is free
    CHECK(mpmSnapshotHostBudget(100 * kGiB) == 16 * kGiB);   // at most sixteen
    CHECK(mpmSnapshotHostBudget(kGiB) == 0);                 // under two gigabytes free: leave it alone
}

TEST(mpm_decimated_frame_index_maps_particles) {
    // Frame j holds particle j * stride; particle ids not on the stride are not in the frame.
    CHECK(mpmFrameParticles(10, 1) == 10);
    CHECK(mpmFrameParticles(10, 3) == 4);  // 0, 3, 6, 9
    CHECK(mpmFrameParticles(9, 3) == 3);   // 0, 3, 6
    CHECK(mpmFrameParticles(0, 3) == 0);
    // The projectile (the first ids) is the first frame entries.
    CHECK(mpmFrameParticles(7, 3) == 3);
}
