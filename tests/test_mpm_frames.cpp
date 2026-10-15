#include "test.h"

#include "sim/mpm_frames.h"

#include <cmath>
#include <vector>

TEST(mpm_snapshot_times_even) {
    const std::vector<double> t = mpmSnapshotTimes(-10e-6, 90e-6, 5);
    CHECK(t.size() == 5);
    CHECK_NEAR(t.front(), -10e-6, 1e-18);
    CHECK_NEAR(t.back(), 90e-6, 1e-18);
    for (size_t i = 1; i < t.size(); ++i) CHECK_NEAR(t[i] - t[i - 1], 25e-6, 1e-15);
    CHECK(mpmSnapshotTimes(0, 1, 1).size() == 2);  // at least both ends
}

TEST(mpm_snapshot_frames_fit_budget) {
    // 144 bytes a particle: 100k particles take 14.4 MB a frame.
    CHECK(mpmSnapshotFrames(100000, 144, 768u << 20, 240) == static_cast<int>((768u << 20) / (144u * 100000u)));
    CHECK(mpmSnapshotFrames(1000, 144, 768u << 20, 240) == 240);
    CHECK(mpmSnapshotFrames(100000000, 144, 768u << 20, 240) == 2);  // never fewer than both ends
    CHECK(mpmSnapshotFrames(0, 144, 768u << 20, 240) == 0);
}

TEST(mpm_snapshot_budget_follows_free_video_memory) {
    const size_t GiB = size_t(1) << 30;
    CHECK(mpmSnapshotBudget(0) == GiB);                // unknown: a gigabyte
    CHECK(mpmSnapshotBudget(GiB) == GiB);              // never less than a gigabyte
    CHECK(mpmSnapshotBudget(6 * GiB) == 3 * GiB);      // half of what is free
    CHECK(mpmSnapshotBudget(40 * GiB) == 4 * GiB);     // at most four
    // A frame of twelve million particles is 480 MB: the gigabyte holds two, four gigabytes eight.
    CHECK(mpmSnapshotFrames(12000000, 40, mpmSnapshotBudget(0), 240) == 2);
    CHECK(mpmSnapshotFrames(12000000, 40, mpmSnapshotBudget(40 * GiB), 240) == 8);
}

TEST(mpm_pick_frame_interpolates) {
    const std::vector<double> t = {0, 10, 20, 30};
    MpmFramePick p = mpmPickFrame(t, 15);
    CHECK(p.a == 1 && p.b == 2);
    CHECK_NEAR(p.lerp, 0.5, 1e-12);
    p = mpmPickFrame(t, -5);
    CHECK(p.a == 0 && p.b == 0 && p.lerp == 0);
    p = mpmPickFrame(t, 99);
    CHECK(p.a == 3 && p.b == 3 && p.lerp == 0);
    p = mpmPickFrame(t, 20);
    CHECK(p.a == 2 && p.b == 3 && p.lerp == 0);
    p = mpmPickFrame({}, 1);
    CHECK(p.a == -1);
}

TEST(mpm_half_floats_decode) {
    CHECK_NEAR(mpmHalfToFloat(0x3C00), 1.0, 0);
    CHECK_NEAR(mpmHalfToFloat(0xC000), -2.0, 0);
    CHECK_NEAR(mpmHalfToFloat(0x0000), 0.0, 0);
    CHECK_NEAR(mpmHalfToFloat(0x5CB0), 300.0, 0);
    CHECK_NEAR(mpmHalfToFloat(0x7BFF), 65504.0, 0);            // the largest finite half
    CHECK_NEAR(mpmHalfToFloat(0x0001), 5.9604645e-8, 1e-12);   // the smallest subnormal
    CHECK(std::isinf(mpmHalfToFloat(0x7C00)));
}

TEST(mpm_frame_unpacks_to_world_state) {
    MpmScene scene;
    scene.originX = 0.1;
    scene.originY = -0.05;
    scene.dx = 0.001;
    MpmFrameWords f{};
    f.x[0] = 120.5f;
    f.x[1] = -30.25f;
    f.v = 0x3C00u | (0xC000u << 16);                              // vx = 1, vy = -2 m/s
    f.flags = 0x00010008u;
    f.matBurnFrac = 3u | (128u << 8) | (255u << 16);              // material 3, burn 128/255, fracture 1
    f.FzzTemp = 0x3C00u | (0x5CB0u << 16);                        // Fzz = 1, 300 K
    f.epsStress = 0x3800u;                                        // plastic strain 0.5
    const MpmParticleState s = mpmUnpackFrame(f, scene);
    CHECK_NEAR(s.x, 0.1 + 120.5 * 0.001, 1e-9);
    CHECK_NEAR(s.y, -0.05 - 30.25 * 0.001, 1e-9);
    CHECK_NEAR(s.vx, 1.0, 0);
    CHECK_NEAR(s.vy, -2.0, 0);
    CHECK(s.flags == 0x00010008u);
    CHECK(s.material == 3);
    CHECK_NEAR(s.burn, 128.0 / 255.0, 1e-9);
    CHECK_NEAR(s.fracture, 1.0, 1e-9);
    CHECK_NEAR(s.temperature, 300.0, 0);
    CHECK_NEAR(s.plasticStrain, 0.5, 0);
}

TEST(mpm_measure_log_keeps_a_thinned_time_ordered_run) {
    auto at = [](double t) {
        MpmMeasure m;
        m.valid = true;
        m.time = t;
        return m;
    };
    std::vector<MpmMeasure> log;
    mpmLogMeasure(log, at(0.0), 1.0);
    mpmLogMeasure(log, at(0.5), 1.0);  // too soon after the last
    CHECK(log.size() == 1);
    mpmLogMeasure(log, at(1.2), 1.0);
    mpmLogMeasure(log, at(2.4), 1.0);
    CHECK(log.size() == 3);
    MpmMeasure bad;  // not a measurement yet
    bad.time = 9;
    mpmLogMeasure(log, bad, 1.0);
    CHECK(log.size() == 3);
    // The run went back (a seek): entries from the new time on are dropped.
    mpmLogMeasure(log, at(1.0), 1.0);
    CHECK(log.size() == 2);
    CHECK_NEAR(log.back().time, 1.0, 0);
    CHECK_NEAR(log.front().time, 0.0, 0);
    mpmLogMeasure(log, at(1.0), 1.0);  // the same one again
    CHECK(log.size() == 2);
}

TEST(mpm_checkpoint_slots_fit_a_quarter_of_free_memory) {
    constexpr size_t kMiB = size_t(1) << 20, kGiB = size_t(1) << 30;
    CHECK(mpmCheckpointSlots(60 * kMiB, 16 * kGiB, 32) == 32);   // a quarter is over the 2 GiB cap: 2 GiB / 60 MiB = 34, so 32
    CHECK(mpmCheckpointSlots(60 * kMiB, 2 * kGiB, 32) == 8);     // 512 MiB / 60 MiB
    CHECK(mpmCheckpointSlots(60 * kMiB, 0, 32) == 4);            // free memory unknown: the 256 MiB floor
    CHECK(mpmCheckpointSlots(1500 * kMiB, 16 * kGiB, 32) == 1);  // 2 GiB / 1500 MiB
    CHECK(mpmCheckpointSlots(3 * kGiB, 16 * kGiB, 32) == 0);     // not even one fits
    CHECK(mpmCheckpointSlots(0, 16 * kGiB, 32) == 0);            // nothing to copy
    CHECK(mpmCheckpointSlots(60 * kMiB, 16 * kGiB, 0) == 0);
}

TEST(mpm_checkpoint_times_are_even_and_inside_the_run) {
    const std::vector<double> t = mpmCheckpointTimes(0.0, 100e-6, 4);
    CHECK(t.size() == 4);
    CHECK_NEAR(t[0], 20e-6, 1e-18);
    CHECK_NEAR(t[3], 80e-6, 1e-18);
    CHECK(mpmCheckpointTimes(0.0, 100e-6, 0).empty());
    CHECK(mpmCheckpointTimes(5.0, 5.0, 3).empty());  // an empty run
    CHECK(mpmCheckpointTimes(5.0, 4.0, 3).empty());
    const std::vector<double> early = mpmCheckpointTimes(-10e-6, 90e-6, 1);
    CHECK(early.size() == 1);
    CHECK_NEAR(early[0], 40e-6, 1e-18);
}

TEST(mpm_pick_checkpoint_takes_the_latest_one_not_after_the_time) {
    const std::vector<double> times{20e-6, 40e-6, 60e-6};
    CHECK(mpmPickCheckpoint(times, 50e-6, 0) == 1);
    CHECK(mpmPickCheckpoint(times, 40e-6, 0) == 1);   // on one
    CHECK(mpmPickCheckpoint(times, 100e-6, 0) == 2);  // past the last
    CHECK(mpmPickCheckpoint(times, 10e-6, 0) == -1);  // before the first: start again
    CHECK(mpmPickCheckpoint(times, 39.9e-6, 0) == 0);
    CHECK(mpmPickCheckpoint(times, 39.9e-6, 0.2e-6) == 1);  // within the tolerance of a step
    CHECK(mpmPickCheckpoint({}, 50e-6, 0) == -1);
}
