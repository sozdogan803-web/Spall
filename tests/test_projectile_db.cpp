#include "test.h"

#include "core/json.h"
#include "data/projectile_db.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

bool loadShipped(ProjectileDb& db, LoadReport& report) {
    std::string fatal;
    const std::string dir = test::dataDir();
    const bool ok = db.load(dir + "/armour_materials.json", dir + "/projectiles.json", report, fatal);
    if (!ok) std::printf("    load failed: %s\n", fatal.c_str());
    for (const std::string& m : report.messages) std::printf("    %s\n", m.c_str());
    return ok;
}

const char* kMaterials = R"({"schema":1,"materials":[
  {"id":"rha","name":"RHA","group":"steel","density_kgm3":7850,"hardness_bhn":300,"ke_efficiency":1,
   "confidence":"medium","sources":["s"]},
  {"id":"al","name":"Al","group":"light_metal","density_kgm3":2660,"hardness_bhn":80,"resistance_gpa":1.7,
   "ke_efficiency":0.9,"color":"#C0C8D0","confidence":"low","sources":["s"]}
],"presets":[
  {"id":"ok","name":"ok","obliquity_deg":30,"layers":[{"material":"rha","thickness_mm":50},
                                                       {"material":"al","thickness_mm":20,"gap_mm":100}]},
  {"id":"bad","name":"bad","obliquity_deg":0,"layers":[{"material":"unobtainium","thickness_mm":50}]}
]})";

std::string projectilesJson(const std::string& entries) {
    return std::string(R"({"schema":1,"projectiles":[)") + entries + "]}";
}

std::string entry(const std::string& id, const std::string& kind, const std::string& fields) {
    return R"({"id":")" + id + R"(","name":")" + id + R"(","kind":")" + kind +
           R"(","country":"X","year":1990,"gun":"g","velocity_mps":1500,"confidence":"low","sources":["s"])" +
           (fields.empty() ? "" : "," + fields) + "}";
}

} // namespace

TEST(projectile_db_loads_shipped_catalogue) {
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    CHECK(report.skipped == 0);
    // Only shape mass mismatches may warn (listed, not rejected).
    int massWarnings = 0;
    for (const std::string& m : report.messages) massWarnings += m.find("the shape's parts weigh") != std::string::npos;
    CHECK(report.warnings == massWarnings);
    CHECK(db.materials().size() >= 15);
    CHECK(db.presets().size() >= 8);
    CHECK(db.projectiles().size() >= 30);
    int kinds[3] = {0, 0, 0};
    for (const ProjectileEntry& e : db.projectiles()) ++kinds[static_cast<int>(e.penetrator.kind)];
    CHECK(kinds[0] >= 8 && kinds[1] >= 10 && kinds[2] >= 10);
    CHECK(db.findMaterial("rha") >= 0);
    CHECK(db.findProjectile("m829") >= 0);
    // The reference material is RHA with the hardness-derived resistance.
    const ArmourMaterialEntry& rha = db.materials()[static_cast<size_t>(db.findMaterial("rha"))];
    CHECK(rha.resistanceFromHardness);
    CHECK_NEAR(rha.material.resistance, phys::steelResistanceFromBhn(300), 1.0);
    for (const ArmourPreset& p : db.presets()) {
        CHECK(!p.layers.empty() && p.layers.size() <= static_cast<size_t>(kMaxArmourLayers));
        for (const PresetLayer& l : p.layers) CHECK(l.material >= 0 && l.material < static_cast<int>(db.materials().size()));
    }
}

TEST(projectile_db_jets_reproduce_published_depth) {
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    for (const ProjectileEntry& e : db.projectiles()) {
        if (e.penetrator.kind != phys::PenetratorKind::ShapedCharge) continue;
        CHECK(e.hasPublished);
        const double model = phys::rhaPenetration(e.penetrator, 0);
        if (std::fabs(model / e.publishedRha - 1.0) > 0.02)
            std::printf("    %s: model %.0f mm, published %.0f mm\n", e.id.c_str(), model * 1e3, e.publishedRha * 1e3);
        CHECK(std::fabs(model / e.publishedRha - 1.0) <= 0.02);
    }
}

TEST(projectile_db_kinetic_rounds_near_published) {
    // Kinetic rounds are not fitted: the model is checked against the published claims.
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    for (const ProjectileEntry& e : db.projectiles()) {
        if (e.penetrator.kind == phys::PenetratorKind::ShapedCharge ||
            e.penetrator.kind == phys::PenetratorKind::HeWarhead)
            continue;
        const double model = phys::rhaPenetration(e.penetrator, e.publishedVelocity);
        // A sanity floor, not a fit: rifle-calibre cores reach only a few millimetres.
        if (model <= 0.005) std::printf("    %s: model %.1f mm\n", e.id.c_str(), model * 1e3);
        CHECK(model > 0.005);
        if (!e.hasPublished) continue;
        if (std::fabs(model / e.publishedRha - 1.0) >= 0.2)
            std::printf("    %s: model %.0f mm at %.0f m/s, published %.0f mm\n", e.id.c_str(), model * 1e3,
                        e.publishedVelocity, e.publishedRha * 1e3);
        CHECK(std::fabs(model / e.publishedRha - 1.0) < 0.2);
    }
}

TEST(projectile_db_inline_validation) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    const std::string entries =
        entry("rod", "long_rod", R"("rod_material":"du","diameter_mm":25,"length_mm":600)") + "," +
        entry("shot", "compact_ap", R"("diameter_mm":88,"mass_kg":10.2,"length_mm":360)") + "," +
        entry("heat", "shaped_charge", R"("cone_diameter_mm":85,"standoff_cd":2.2,"published_rha_mm":500)") + "," +
        entry("noclaim", "shaped_charge", R"("cone_diameter_mm":85,"standoff_cd":2.2)") + "," +
        entry("badkind", "laser", "") + "," +
        entry("badrod", "long_rod", R"("rod_material":"gold","diameter_mm":25,"length_mm":600)") + "," +
        entry("rod", "long_rod", R"("rod_material":"steel","diameter_mm":25,"length_mm":600)");
    CHECK(db.loadText(kMaterials, projectilesJson(entries), report, fatal));
    CHECK(db.materials().size() == 2);
    CHECK(db.presets().size() == 1);
    CHECK(db.projectiles().size() == 3);
    CHECK(report.skipped == 5);  // bad preset, missing claim, unknown kind, unknown rod material, duplicate id
    const ProjectileEntry& rod = db.projectiles()[0];
    CHECK(rod.penetrator.uranium);
    CHECK_NEAR(rod.penetrator.density, 18600, 1e-9);
    CHECK_NEAR(rod.penetrator.length, 0.6, 1e-12);
    const ProjectileEntry& shot = db.projectiles()[1];
    CHECK_NEAR(shot.penetrator.mass, 10.2, 1e-12);
    CHECK(shot.rodMaterial == RodMaterial::Steel);
    CHECK_NEAR(phys::rhaPenetration(db.projectiles()[2].penetrator, 0), 0.5, 0.01);
    CHECK(db.materials()[1].color == 0xC0C8D0u);
    CHECK_NEAR(db.presets()[0].layers[1].gap, 0.1, 1e-12);

    CHECK(!db.loadText(R"({"schema":2,"materials":[]})", projectilesJson(""), report, fatal));
    CHECK(fatal.find("schema") != std::string::npos);
    CHECK(db.projectiles().empty());
}

namespace {

std::string shippedExplosives() {
    std::string text;
    json::readFile(test::dataDir() + "/explosives.json", text);
    return text;
}

// A 50 x 150 mm steel shot (2.31 kg as a flat-nosed cylinder) with the given shape block.
std::string shotWithShape(const std::string& id, const std::string& shape, double massKg = 2.31) {
    char mass[32];
    std::snprintf(mass, sizeof(mass), "%.4f", massKg);
    return entry(id, "compact_ap",
                 std::string(R"("diameter_mm":50,"length_mm":150,"mass_kg":)") + mass + R"(,"shape":)" + shape);
}

} // namespace

TEST(projectile_shape_parsed) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    const std::string entries =
        shotWithShape("aphe", R"({"style":"apcbc","nose_crh":2,"cap":{"length_mm":30,"material":"cap_steel"},
                                  "filler":{"explosive":"tnt","diameter_mm":20,"length_mm":40},
                                  "fuze":{"type":"base_delay","delay_us":1500}})");
    CHECK(db.loadText(kMaterials, projectilesJson(entries), report, fatal, shippedExplosives()));
    for (const std::string& m : report.messages) std::printf("    %s\n", m.c_str());
    CHECK(db.findExplosive("tnt") >= 0);
    CHECK(db.projectiles().size() == 1);
    if (db.projectiles().size() != 1) return;
    const phys::ShapeSpec& s = db.projectiles()[0].shape;
    CHECK(s.present);
    CHECK(s.style == phys::ShapeStyle::Apcbc);
    CHECK(s.noseCrh && std::fabs(*s.noseCrh - 2) < 1e-12);
    CHECK_NEAR(s.cap.length, 0.030, 1e-12);
    CHECK_EQ_STR(s.cap.material, "cap_steel");
    CHECK_EQ_STR(s.filler.explosive, "tnt");
    CHECK_NEAR(s.filler.diameter, 0.020, 1e-12);
    CHECK_NEAR(s.filler.length, 0.040, 1e-12);
    const Explosive& tnt = db.explosives()[static_cast<size_t>(db.findExplosive("tnt"))];
    CHECK_NEAR(s.filler.density, tnt.density, 1e-9);
    CHECK_NEAR(s.filler.detonationSpeed, tnt.detonationVelocity, 1e-9);
    CHECK_NEAR(s.filler.energy, 4e6 * tnt.reFactor, 1e-6);
    CHECK(s.fuze.type == phys::FuzeType::BaseDelay);
    CHECK_NEAR(s.fuze.delay, 1.5e-3, 1e-12);
    CHECK_NEAR(s.fuze.triggerDv, 30, 1e-12);
    CHECK_NEAR(s.refLength, 0.150, 1e-12);
    CHECK_NEAR(s.refDiameter, 0.050, 1e-12);
}

TEST(projectile_shape_rejects) {
    const char* bad[][2] = {
        {"badstyle", R"({"style":"saucer"})"},
        {"negative", R"({"style":"ap","boat_tail_mm":-5})"},
        {"badmaterial", R"({"style":"apc","cap":{"material":"adamantium"}})"},
        {"badexplosive", R"({"style":"apc","filler":{"explosive":"unobtainite"},"fuze":{"type":"base_delay"}})"},
        {"nofuze", R"({"style":"apc","filler":{"explosive":"tnt"}})"},
        {"offsize", R"({"style":"ap","diameter_mm":51.5})"},
    };
    for (const auto& b : bad) {
        ProjectileDb db;
        LoadReport report;
        std::string fatal;
        CHECK(db.loadText(kMaterials, projectilesJson(shotWithShape(b[0], b[1])), report, fatal, shippedExplosives()));
        CHECK(db.projectiles().empty());
        CHECK(report.skipped == 2);  // the fixture's bad preset and this entry
        bool named = false;
        for (const std::string& m : report.messages) named |= m.find(b[0]) != std::string::npos;
        if (!named || !db.projectiles().empty()) std::printf("    %s not rejected\n", b[0]);
        CHECK(named);
    }
}

TEST(projectile_shape_mass_warning) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    const std::string shape = R"({"style":"ap","nose_crh":0})";
    const double parts = 7850 * 3.14159265358979 * 0.025 * 0.025 * 0.150;
    CHECK(db.loadText(kMaterials, projectilesJson(shotWithShape("fair", shape, parts)), report, fatal,
                      shippedExplosives()));
    CHECK(db.projectiles().size() == 1);
    CHECK(report.warnings == 0);
    CHECK(db.loadText(kMaterials, projectilesJson(shotWithShape("light", shape, parts / 1.3)), report, fatal,
                      shippedExplosives()));
    CHECK(db.projectiles().size() == 1);
    CHECK(report.warnings == 1);
}

TEST(projectile_shape_absent_is_generic) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    CHECK(db.loadText(kMaterials, projectilesJson(entry("shot", "compact_ap", R"("diameter_mm":88,"mass_kg":10.2,"length_mm":360)")),
                      report, fatal));
    CHECK(db.projectiles().size() == 1);
    CHECK(!db.projectiles()[0].shape.present);
}

TEST(catalogue_small_arms_ap) {
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    const char* ids[] = {"556_m995",   "762_m61",     "762_m993",    "762x39_bz",   "762x54_b32",
                         "792_smk",    "127x108_b32", "145x114_b32", "145x114_bs41"};
    for (const char* id : ids) {
        const int i = db.findProjectile(id);
        if (i < 0) std::printf("    %s missing\n", id);
        CHECK(i >= 0);
        if (i < 0) continue;
        const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(i)];
        const phys::Penetrator& p = e.penetrator;
        CHECK(p.kind == phys::PenetratorKind::CompactAp);
        CHECK(e.shape.present && e.shape.style == phys::ShapeStyle::Bullet);
        const phys::ProjectileShape shape = phys::buildShape(e.shape, p);
        CHECK(p.diameter < shape.diameter);  // the core inside the calibre
        double mass = 0;
        bool incendiary = false;
        for (const phys::ShapePart& part : shape.parts) {
            incendiary |= part.role == phys::PartRole::Incendiary;
            if (!part.penetrator) continue;
            const double rho = part.material.empty() ? p.density : phys::mpmPartMaterial(part.material)->density;
            mass += phys::partMass(part, rho);
        }
        bool listed = false;
        for (const std::string& m : report.messages) listed |= m.find(e.id + ":") != std::string::npos;
        if (std::fabs(mass / p.mass - 1) > 0.2 && !listed)
            std::printf("    %s: core %.2f g, catalogue %.2f g\n", id, mass * 1e3, p.mass * 1e3);
        CHECK(std::fabs(mass / p.mass - 1) <= 0.2 || listed);
        const std::string s = id;
        const bool api = s.find("b32") != std::string::npos || s.find("bz") != std::string::npos ||
                         s.find("bs41") != std::string::npos;
        CHECK(incendiary == api);
        CHECK(!e.sources.empty());
    }
}

TEST(catalogue_kinetic_rounds_have_shapes) {
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    CHECK(report.skipped == 0);
    for (const std::string& m : report.messages) CHECK(m.find("the shape's parts weigh") != std::string::npos);
    for (const ProjectileEntry& e : db.projectiles()) {
        const phys::Penetrator& p = e.penetrator;
        if (p.kind == phys::PenetratorKind::ShapedCharge || p.kind == phys::PenetratorKind::HeWarhead) continue;
        if (!e.shape.present) std::printf("    %s has no shape\n", e.id.c_str());
        CHECK(e.shape.present);
        const phys::ProjectileShape shape = phys::buildShape(e.shape, p);
        CHECK(!shape.parts.empty());
        CHECK(shape.length >= p.length * 0.95 && shape.diameter >= p.diameter * 0.98);
        if (!(p.mass > 0)) continue;
        const bool wholeRound = e.shape.style == phys::ShapeStyle::Ap || e.shape.style == phys::ShapeStyle::Apc ||
                                e.shape.style == phys::ShapeStyle::Apbc || e.shape.style == phys::ShapeStyle::Apcbc;
        double mass = 0;
        for (const phys::ShapePart& part : shape.parts) {
            if (!wholeRound && !part.penetrator) continue;
            double rho = part.density;
            if (!(rho > 0)) rho = part.material.empty() ? p.density : phys::mpmPartMaterial(part.material)->density;
            mass += phys::partMass(part, rho);
        }
        bool listed = false;
        for (const std::string& m : report.messages) listed |= m.find(e.id + ":") != std::string::npos;
        std::printf("    %s: parts %.3f kg, catalogue %.3f kg\n", e.id.c_str(), mass, p.mass);
        CHECK(std::fabs(mass / p.mass - 1) <= 0.2 || listed);
    }
}

TEST(catalogue_aphe_have_fillers) {
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    const char* ids[] = {"88mm_pzgr39_kwk36", "88mm_pzgr39_43", "75mm_pzgr39_42", "75mm_m61",  "76mm_m62",
                         "76mm_br350a",       "85mm_br365",     "122mm_br471",    "90mm_m82"};
    for (const char* id : ids) {
        const int i = db.findProjectile(id);
        if (i < 0) std::printf("    %s missing\n", id);
        CHECK(i >= 0);
        if (i < 0) continue;
        const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(i)];
        bool known = false;
        for (const Explosive& x : db.explosives()) known |= x.id == e.shape.filler.explosive;
        if (!known) std::printf("    %s: filler '%s'\n", id, e.shape.filler.explosive.c_str());
        CHECK(known);
        CHECK(e.shape.filler.diameter > 0 && e.shape.filler.length > 0);
        CHECK(e.penetrator.fuze.type == phys::FuzeType::BaseDelay);
        CHECK(e.notes.find("treated as solid") == std::string::npos);
    }
}

TEST(catalogue_he_missiles) {
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    const char* ids[] = {"hydra70_m151", "s8of", "s13of", "agm176_griffin", "agm114m"};
    for (const char* id : ids) {
        const int i = db.findProjectile(id);
        if (i < 0) std::printf("    %s missing\n", id);
        CHECK(i >= 0);
        if (i < 0) continue;
        const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(i)];
        CHECK(e.penetrator.kind == phys::PenetratorKind::HeWarhead);
        CHECK(e.shape.style == phys::ShapeStyle::HeMissile);
        CHECK(e.penetrator.fuze.type == phys::FuzeType::NoseContact);
        bool known = false;
        for (const Explosive& x : db.explosives()) known |= x.id == e.shape.filler.explosive;
        CHECK(known);
        CHECK(!e.sources.empty());
        CHECK(e.shape.massKg > 0 && e.penetrator.mass == e.shape.massKg);
        const phys::ProjectileShape shape = phys::shapeForCase(&e.shape, e.penetrator);
        for (const std::string& w : shape.warnings) std::printf("    %s: %s\n", id, w.c_str());
        CHECK(shape.warnings.empty());
    }
}

TEST(catalogue_heat_bodies) {
    // Every shaped charge is a rocket or grenade carrying its charge; the RPG-7 rounds and the
    // Malyutka give their body diameter and explosive.
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    int charges = 0;
    for (const ProjectileEntry& e : db.projectiles()) {
        if (e.penetrator.kind != phys::PenetratorKind::ShapedCharge) continue;
        ++charges;
        const phys::ProjectileShape shape = phys::shapeForCase(e.shape.present ? &e.shape : nullptr, e.penetrator);
        bool liner = false, filler = false;
        for (const phys::ShapePart& part : shape.parts) {
            liner |= part.role == phys::PartRole::Liner && part.penetrator;
            filler |= part.role == phys::PartRole::Filler;
        }
        if (!liner || !filler) std::printf("    %s: no liner or filler\n", e.id.c_str());
        CHECK(liner && filler);
        CHECK_NEAR(shape.penetratorDiameter, e.penetrator.coneDiameter, 1e-9);
        for (const std::string& w : shape.warnings) std::printf("    %s: %s\n", e.id.c_str(), w.c_str());
        CHECK(shape.warnings.empty());
        CHECK(e.penetrator.fuze.type == phys::FuzeType::None);  // the charge fires on contact (scene)
    }
    CHECK(charges >= 10);
    for (const char* id : {"pg7v", "pg7vl", "malyutka_9m14p1"}) {
        const int i = db.findProjectile(id);
        CHECK(i >= 0);
        if (i < 0) continue;
        const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(i)];
        CHECK(e.shape.present && e.shape.style == phys::ShapeStyle::HeatRocket);
        CHECK(e.shape.diameter > e.penetrator.coneDiameter);
        bool known = false;
        for (const Explosive& x : db.explosives()) known |= x.id == e.shape.filler.explosive;
        CHECK(known && e.shape.filler.density > 0);
    }
}

TEST(preset_layer_block_fields_parse) {
    // ERA is a row of separate tiles (steel / explosive / steel) on a full base plate.
    ProjectileDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    for (const char* id : {"reactive_sandwich", "bulging_sandwich"}) {
        const ArmourPreset* p = nullptr;
        for (const ArmourPreset& q : db.presets())
            if (q.id == id) p = &q;
        CHECK(p != nullptr);
        if (!p) continue;
        CHECK(p->layers.size() == 4);
        for (size_t i = 0; i < 3 && i < p->layers.size(); ++i) {
            CHECK_NEAR(p->layers[i].blockWidth, 0.25, 1e-12);
            CHECK_NEAR(p->layers[i].blockGap, 0.01, 1e-12);
            CHECK(p->layers[i].offset == 0);
        }
        CHECK(p->layers.back().blockWidth == 0);  // the base plate is whole
    }
    for (const ArmourPreset& q : db.presets())
        if (q.id == "rha_100") CHECK(q.layers[0].blockWidth == 0 && q.layers[0].blockGap == 0 && q.layers[0].offset == 0);

    // Too narrow a tile is an error; 0 is a full plate.
    const std::string bad = std::string(kMaterials).replace(std::string(kMaterials).find("\"thickness_mm\":50}"), 18,
                                                          "\"thickness_mm\":50,\"block_width_mm\":5}");
    ProjectileDb db2;
    LoadReport report2;
    std::string fatal;
    CHECK(db2.loadText(bad, projectilesJson(entry("rod", "long_rod", R"("rod_material":"steel","diameter_mm":25,"length_mm":600)")),
                       report2, fatal));
    bool skipped = false;
    for (const std::string& m : report2.messages) skipped |= m.find("block_width_mm") != std::string::npos;
    CHECK(skipped);
    bool okLoaded = false;
    for (const ArmourPreset& q : db2.presets()) okLoaded |= q.id == "ok";
    CHECK(!okLoaded);

    // Tiles 300 mm wide, 20 mm apart, shifted 40 mm, in the JSON's own units.
    const std::string tiled = std::string(kMaterials).replace(
        std::string(kMaterials).find("\"thickness_mm\":50}"), 18,
        "\"thickness_mm\":50,\"block_width_mm\":300,\"block_gap_mm\":20,\"offset_mm\":-40}");
    ProjectileDb db3;
    LoadReport report3;
    CHECK(db3.loadText(tiled, projectilesJson(entry("rod", "long_rod", R"("rod_material":"steel","diameter_mm":25,"length_mm":600)")),
                       report3, fatal));
    for (const ArmourPreset& q : db3.presets())
        if (q.id == "ok") {
            CHECK_NEAR(q.layers[0].blockWidth, 0.3, 1e-12);
            CHECK_NEAR(q.layers[0].blockGap, 0.02, 1e-12);
            CHECK_NEAR(q.layers[0].offset, -0.04, 1e-12);
            CHECK(q.layers[1].blockWidth == 0);
        }
}
