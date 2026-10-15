#include "test.h"

#include "data/projectile_db.h"
#include "sim/armour_run.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

const ProjectileDb& shippedDb() {
    static ProjectileDb db;
    static bool loaded = false;
    if (!loaded) {
        LoadReport report;
        std::string fatal;
        const std::string dir = test::dataDir();
        loaded = db.load(dir + "/armour_materials.json", dir + "/projectiles.json", report, fatal);
        if (!loaded) std::printf("    load failed: %s\n", fatal.c_str());
    }
    return db;
}

ArmourCase caseFor(const ProjectileDb& db, const char* projectile, const char* preset) {
    ArmourCase c;
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(db.findProjectile(projectile))];
    c.penetrator = e.penetrator;
    c.velocity = e.velocity;
    for (const ArmourPreset& p : db.presets())
        if (p.id == preset) {
            c.obliquityDeg = p.obliquityDeg;
            c.layers = p.layers;
        }
    return c;
}

} // namespace

TEST(armour_run_maps_catalogue_layers) {
    const ProjectileDb& db = shippedDb();
    const int al = db.findMaterial("al5083");
    CHECK(al >= 0);
    const std::vector<phys::ArmourLayer> layers = physicsLayers(db, {{al, 0.04, 0.0}, {-1, 0.01, 0.2}});
    CHECK(layers.size() == 2);
    CHECK_NEAR(layers[0].material.density, 2660, 1e-9);
    CHECK_NEAR(layers[0].thickness, 0.04, 1e-12);
    CHECK_NEAR(layers[1].material.density, phys::kRhaDensity, 1e-9);  // invalid index: RHA defaults
    CHECK_NEAR(layers[1].gap, 0.2, 1e-12);
}

TEST(armour_run_rha_plate_is_worth_its_own_thickness) {
    // DoP method on a plain RHA plate: the equivalent should be close to the plate itself.
    const ProjectileDb& db = shippedDb();
    for (const char* projectile : {"m829", "dm53", "3bm42"}) {
        const ArmourOutcome o = evaluateArmour(db, caseFor(db, projectile, "rha_100"));
        CHECK(o.result.perforated);
        CHECK(!o.rhaEquivalentIsBound);
        CHECK_NEAR(o.lineOfSight, 0.1, 1e-9);
        CHECK_NEAR(o.arealDensity, 785, 1e-6);
        CHECK(o.witnessDepth > 0 && o.witnessDepth < o.witnessReference);
        CHECK_NEAR(o.thicknessEfficiency, 1.0, 0.15);
        CHECK_NEAR(o.massEfficiency, o.thicknessEfficiency, 1e-9);
        CHECK(o.ballisticLimit > 0 && o.ballisticLimit < o.result.residualVelocity + 1e6);
        CHECK_NEAR(o.witnessReference, o.rhaSemiInfinite, 0.05 * o.rhaSemiInfinite);
    }
}

TEST(armour_run_oblique_plate_counts_line_of_sight) {
    const ProjectileDb& db = shippedDb();
    const ArmourOutcome o = evaluateArmour(db, caseFor(db, "m829", "rha_100_60"));
    CHECK_NEAR(o.lineOfSight, 0.2, 1e-9);
    CHECK_NEAR(o.arealDensity, 1570, 1e-6);
    CHECK(o.result.perforated);
    CHECK(o.rhaEquivalent > 0.15 && o.rhaEquivalent < 0.3);
}

TEST(armour_run_stopped_round_gives_a_lower_bound) {
    const ProjectileDb& db = shippedDb();
    const ArmourOutcome o = evaluateArmour(db, caseFor(db, "m829", "rha_600"));
    CHECK(!o.result.perforated);
    CHECK(o.rhaEquivalentIsBound);
    CHECK_NEAR(o.witnessDepth, 0.0, 1e-12);
    CHECK_NEAR(o.rhaEquivalent, o.witnessReference, 1e-12);
    const double v = db.projectiles()[static_cast<size_t>(db.findProjectile("m829"))].velocity;
    CHECK(o.ballisticLimit < 0 || o.ballisticLimit > v);
}

TEST(armour_run_shaped_charge_has_no_ballistic_limit) {
    const ProjectileDb& db = shippedDb();
    const ArmourOutcome o = evaluateArmour(db, caseFor(db, "pg7vl", "rha_300"));
    CHECK_NEAR(o.ballisticLimit, 0.0, 1e-12);
    CHECK(o.result.perforated);
    CHECK(o.witnessDepth > 0.1);
    CHECK_NEAR(o.witnessReference, 0.5, 0.02);
}

TEST(armour_run_light_and_ceramic_layers_beat_steel_by_mass) {
    // Light and ceramic layers stop more per kilogram than RHA in this model (mass efficiency > 1).
    const ProjectileDb& db = shippedDb();
    for (const char* preset : {"aluminium_plate", "ceramic_sandwich"}) {
        const ArmourOutcome o = evaluateArmour(db, caseFor(db, "m829", preset));
        std::printf("    m829 vs %-18s Es %.2f  Em %.2f\n", preset, o.thicknessEfficiency, o.massEfficiency);
        CHECK(o.result.perforated);
        CHECK(o.massEfficiency > 1.0);
    }
}

TEST(preset_thin_sheet_present) {
    const ProjectileDb& db = shippedDb();
    const ArmourPreset* pr = nullptr;
    for (const ArmourPreset& p : db.presets())
        if (p.id == "thin_sheet_rha_100") pr = &p;
    CHECK(pr != nullptr);
    if (!pr) return;
    CHECK(pr->layers.size() == 2u);
    CHECK(pr->obliquityDeg == 0);
    CHECK_NEAR(pr->layers[0].thickness, 0.001, 1e-9);
    CHECK_NEAR(pr->layers[1].thickness, 0.1, 1e-9);
    CHECK_NEAR(pr->layers[1].gap, 0.3, 1e-9);
    CHECK(db.materials()[static_cast<size_t>(pr->layers[0].material)].id == "mild_steel");
    CHECK(db.materials()[static_cast<size_t>(pr->layers[1].material)].id == "rha");
}

TEST(armour_shot_between_tiles_skips_layer) {
    // Shift the ERA tiles so the shot line runs down the gap between two of them: the engineering
    // model meets no sandwich, and the base plate stays where it was.
    const ProjectileDb& db = shippedDb();
    ArmourCase c = caseFor(db, "m829", "reactive_sandwich");
    CHECK(c.layers.size() == 4);
    if (c.layers.size() != 4) return;
    const ArmourOutcome hit = evaluateArmour(db, c);
    for (int i = 0; i < 3; ++i) c.layers[static_cast<size_t>(i)].offset = 0.13;
    const std::vector<phys::ArmourLayer> layers = physicsLayers(db, c.layers);
    for (int i = 0; i < 3; ++i) CHECK(layers[static_cast<size_t>(i)].thickness == 0);
    // Each missed layer's thickness becomes air in front of the next: every face stays put (the
    // tiles either side are still drawn and simulated where they are).
    CHECK_NEAR(layers[1].gap, 0.004, 1e-12);
    CHECK_NEAR(layers[2].gap, 0.004, 1e-12);
    CHECK_NEAR(layers[3].gap, 0.15 + 0.004, 1e-12);
    CHECK_NEAR(layers[3].thickness, 0.06, 1e-12);
    const ArmourOutcome missed = evaluateArmour(db, c);
    CHECK(missed.result.layers.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK_NEAR(missed.result.layers[i].front, hit.result.layers[i].front, 1e-9);
    CHECK(missed.lineOfSight < hit.lineOfSight);
    CHECK(std::isfinite(missed.result.residualVelocity));
}
