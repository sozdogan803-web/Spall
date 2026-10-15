#include "test.h"

#include "data/projectile_db.h"
#include "physics/fuze.h"
#include "sim/armour_run.h"

#include <cstdio>
#include <string>

namespace {

phys::FuzeSpec baseFuze(double delay) {
    phys::FuzeSpec f;
    f.type = phys::FuzeType::BaseDelay;
    f.triggerDv = 30;
    f.delay = delay;
    f.fillerLength = 0.05;
    f.detonationSpeed = 6900;
    return f;
}

// 800 -> 700 m/s linearly over 0..100 us, the nose in one plate 0..60 mm.
phys::PenetrationResult linearHistory(bool perforated) {
    phys::PenetrationResult r;
    for (int i = 0; i <= 10; ++i) {
        const double t = i * 10e-6;
        r.history.push_back({t, 0.6 * t * 1000, 800 - 1e6 * t, 0.1, 750, 0.01, 0});
    }
    phys::LayerResult l;
    l.front = 0;
    l.back = 0.08;
    l.depth = 0.06;
    l.perforated = perforated;
    r.layers.push_back(l);
    r.perforated = perforated;
    r.depth = 0.06;
    r.residualVelocity = perforated ? 700 : 0;
    return r;
}

} // namespace

TEST(fuze_triggers_on_velocity_loss) {
    const phys::FuzeEvent e = phys::fuzeEvent(baseFuze(20e-6), 800, linearHistory(true));
    CHECK(e.triggered);
    CHECK_NEAR(e.triggerTime, 30e-6, 1e-9);
    CHECK(e.triggerLayer == 0);
    CHECK_NEAR(e.burstTime, 50e-6, 1e-9);
    CHECK_NEAR(e.burstDepth, 0.03, 1e-6);
    CHECK(e.burstLayer == 0);
    CHECK(!e.stopped);
}

TEST(fuze_never_triggers) {
    phys::PenetrationResult r = linearHistory(true);
    for (phys::PenetrationSample& s : r.history) s.velocity = 800 - 0.05 * (800 - s.velocity);  // loses 5 m/s
    const phys::FuzeEvent e = phys::fuzeEvent(baseFuze(20e-6), 800, r);
    CHECK(!e.triggered);
    CHECK(e.burstLayer == -1);
    phys::FuzeSpec none;
    CHECK(!phys::fuzeEvent(none, 800, linearHistory(true)).triggered);
}

TEST(fuze_burst_behind_stack) {
    // Trigger at 30 us, burst at 130 us: 30 us past the last sample at 700 m/s residual.
    const phys::FuzeEvent e = phys::fuzeEvent(baseFuze(100e-6), 800, linearHistory(true));
    CHECK(e.triggered);
    CHECK_NEAR(e.burstTime, 130e-6, 1e-9);
    CHECK_NEAR(e.burstDepth, 0.06 + 700 * 30e-6, 1e-6);
    CHECK(e.burstLayer == -1);
    CHECK(!e.stopped);
}

TEST(fuze_burst_stopped) {
    const phys::FuzeEvent e = phys::fuzeEvent(baseFuze(100e-6), 800, linearHistory(false));
    CHECK(e.triggered);
    CHECK(e.stopped);
    CHECK_NEAR(e.burstDepth, 0.06, 1e-9);
    CHECK(e.burstLayer == 0);
}

TEST(armour_outcome_end_covers_burst) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    const std::string dir = test::dataDir();
    CHECK(db.load(dir + "/armour_materials.json", dir + "/projectiles.json", report, fatal));
    ArmourCase c;
    c.penetrator.kind = phys::PenetratorKind::CompactAp;
    c.penetrator.diameter = 0.075;
    c.penetrator.length = 0.3;
    c.penetrator.density = 7850;
    c.penetrator.mass = 6.8;
    c.penetrator.fuze = baseFuze(1000e-6);
    c.velocity = 620;
    for (const ArmourPreset& p : db.presets())
        if (p.id == "rha_100") c.layers = p.layers;
    c.layers[0].thickness = 0.05;
    const ArmourOutcome o = evaluateArmour(db, c);
    CHECK(o.fuze.triggered);
    CHECK(o.result.endTime >= o.fuze.burstTime + 20e-6 + 10 * 0.05 / 6900 - 1e-12);
    if (!o.fuze.triggered) std::printf("    no trigger, end %.0f us\n", o.result.endTime * 1e6);
}
