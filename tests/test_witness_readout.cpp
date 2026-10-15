// Witness-sheet readouts of the deformation simulation (hole width, bulge, residual velocity).
#include "test.h"

#include "data/projectile_db.h"
#include "sim/armour_run.h"
#include "sim/mpm_scene.h"
#include "sim/witness_readout.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

const ProjectileDb& shippedDb() {
    static ProjectileDb db;
    static bool loaded = false;
    if (!loaded) {
        LoadReport report;
        std::string fatal;
        const std::string dir = test::dataDir();
        loaded = db.load(dir + "/armour_materials.json", dir + "/projectiles.json", report, fatal);
    }
    return db;
}

ArmourCase caseOf(const ProjectileDb& db, const std::vector<PresetLayer>& layers) {
    ArmourCase c;
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(db.findProjectile("generic_30mm_apfsds"))];
    c.penetrator = e.penetrator;
    c.velocity = e.velocity;
    c.layers = layers;
    return c;
}

constexpr double kSpacing = 0.0005;

// A sheet (layer 1) at x = 0.1, y = -0.02..0.02 in steps of kSpacing, and one projectile particle.
MpmScene sheetScene() {
    MpmScene s;
    s.supported = true;
    s.dx = 2 * kSpacing;
    s.spacing = kSpacing;
    MpmSeed proj{};
    proj.x = 0.0f;
    proj.y = 0.0f;
    proj.volume = 1e-7f;
    proj.material = 0;
    proj.flags = kMpmProjectile;
    s.particles.push_back(proj);
    for (int k = -40; k <= 40; ++k) {
        MpmSeed p{};
        p.x = 0.1f;
        p.y = static_cast<float>(k * kSpacing);
        p.volume = static_cast<float>(kSpacing * kSpacing);
        p.material = 2;
        p.layer = 1;
        p.flags = 1u << kMpmLayerShift;
        s.particles.push_back(p);
    }
    s.bodyCount = 1;
    return s;
}

// Particles that sit at their seeds; `edit(i, seed, state)` then changes each.
template <class F>
std::vector<MpmParticleState> states(const MpmScene& s, F edit) {
    std::vector<MpmParticleState> out(s.particles.size());
    for (size_t i = 0; i < s.particles.size(); ++i) {
        MpmParticleState& st = out[i];
        st.x = s.particles[i].x;
        st.y = s.particles[i].y;
        st.flags = s.particles[i].flags;
        st.material = s.particles[i].material;
        edit(i, s.particles[i], st);
    }
    return out;
}

}  // namespace

TEST(witness_sheet_is_a_thin_layer_set_back_from_the_stack) {
    const ProjectileDb& db = shippedDb();
    const int rha = db.findMaterial("rha"), al = db.findMaterial("al2024");
    const ArmourCase c = caseOf(db, {{rha, 0.012, 0, 0, 0, 0, 0},
                                     {al, 0.0015, 0.1, 0, 0, 0, 0},
                                     {rha, 0.05, 0, 0, 0, 0, 0},
                                     {al, 0.0015, 0.005, 0, 0, 0, 0},
                                     {al, 0.008, 0.1, 0, 0, 0, 0}});
    CHECK(!isWitnessSheet(c, 0));  // the first layer is the armour
    CHECK(isWitnessSheet(c, 1));
    CHECK(!isWitnessSheet(c, 2));  // thick
    CHECK(!isWitnessSheet(c, 3));  // too close to the plate in front (gap 5 mm)
    CHECK(!isWitnessSheet(c, 4));  // 8 mm: a plate, not a sheet
    CHECK(!isWitnessSheet(c, -1));
    CHECK(!isWitnessSheet(c, 9));
}

TEST(witness_stack_preset_has_three_sheets) {
    const ProjectileDb& db = shippedDb();
    int idx = -1;
    for (size_t i = 0; i < db.presets().size(); ++i)
        if (db.presets()[i].id == "witness_stack") idx = static_cast<int>(i);
    CHECK(idx >= 0);
    if (idx < 0) return;
    const ArmourCase c = caseOf(db, db.presets()[static_cast<size_t>(idx)].layers);
    int sheets = 0;
    for (int i = 0; i < static_cast<int>(c.layers.size()); ++i) sheets += isWitnessSheet(c, i) ? 1 : 0;
    CHECK(sheets == 3);
}

TEST(witness_readout_finds_the_hole_the_bulge_and_the_speed) {
    const MpmScene scene = sheetScene();
    // A hole 21 particles wide (-5..+5 mm) and a bulge that peaks at 3 mm out at y = +-10 mm.
    const auto st = states(scene, [](size_t, const MpmSeed& seed, MpmParticleState& s) {
        if (!(seed.flags & kMpmProjectile)) {
            if (std::abs(seed.y) <= 0.0050001f) {
                s.flags |= kMpmFailed;
            } else {
                const double bump = 0.003 * (1.0 - std::abs(std::abs(seed.y) - 0.010) / 0.010);
                s.x = seed.x + std::max(bump, 0.0);
                s.vx = 40 * std::max(bump, 0.0) / 0.003;
            }
        }
    });
    MpmMeasure m;
    m.valid = true;
    const WitnessReadout w = measureWitness(scene, st, 1, m);
    CHECK(w.valid);
    CHECK(w.layer == 1);
    CHECK_NEAR(w.holeWidth, 0.0105, 0.0006);
    CHECK_NEAR(w.bulge, 0.003, 1e-6);
    CHECK_NEAR(w.peakSpeed, 40, 1e-3);
    CHECK_NEAR(w.residualVelocity, 0, 1e-12);
}

TEST(witness_readout_without_a_hole_reports_zero_width) {
    const MpmScene scene = sheetScene();
    const auto st = states(scene, [](size_t, const MpmSeed& seed, MpmParticleState& s) {
        if (!(seed.flags & kMpmProjectile)) s.x = seed.x + 0.002;
    });
    const WitnessReadout w = measureWitness(scene, st, 1, MpmMeasure{});
    CHECK(w.valid);
    CHECK_NEAR(w.holeWidth, 0, 1e-12);
    CHECK_NEAR(w.bulge, 0.002, 1e-6);
}

TEST(witness_readout_ignores_failed_material_away_from_the_shot_line) {
    const MpmScene scene = sheetScene();
    const auto st = states(scene, [](size_t, const MpmSeed& seed, MpmParticleState& s) {
        // Only a patch 15..20 mm off the shot line has failed (an earlier fragment, say).
        if (!(seed.flags & kMpmProjectile) && seed.y > 0.015f) s.flags |= kMpmFailed;
    });
    const WitnessReadout w = measureWitness(scene, st, 1, MpmMeasure{});
    CHECK(w.valid);
    CHECK_NEAR(w.holeWidth, 0, 1e-12);
}

TEST(witness_readout_reports_the_penetrator_velocity_only_once_past_the_sheet) {
    const MpmScene scene = sheetScene();
    const auto st = states(scene, [](size_t, const MpmSeed&, MpmParticleState&) {});
    MpmMeasure m;
    m.valid = true;
    m.intact = 100;
    m.velocity = 850;
    m.nose = 0.09;  // still in front of the sheet (x = 0.1)
    CHECK_NEAR(measureWitness(scene, st, 1, m).residualVelocity, 0, 1e-12);
    m.nose = 0.12;  // through
    CHECK_NEAR(measureWitness(scene, st, 1, m).residualVelocity, 850, 1e-9);
    m.intact = 0;   // nothing intact left to speak of
    CHECK_NEAR(measureWitness(scene, st, 1, m).residualVelocity, 0, 1e-12);
}

TEST(witness_readout_is_invalid_for_mismatched_input) {
    const MpmScene scene = sheetScene();
    std::vector<MpmParticleState> wrongSize(scene.particles.size() - 3);
    CHECK(!measureWitness(scene, wrongSize, 1, MpmMeasure{}).valid);
    const auto st = states(scene, [](size_t, const MpmSeed&, MpmParticleState&) {});
    CHECK(!measureWitness(scene, st, 4, MpmMeasure{}).valid);  // no such layer in the scene
    CHECK(!measureWitness(scene, st, -1, MpmMeasure{}).valid);
}
