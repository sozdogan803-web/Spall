#include "test.h"

#include "data/projectile_db.h"
#include "physics/gurney.h"
#include "physics/mpm_material.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

phys::MpmPoint point(double e1, double e2, double e3) {
    phys::MpmPoint p;
    p.strain[0] = e1;
    p.strain[1] = e2;
    p.strain[2] = e3;
    return p;
}

double vonMises(const phys::MpmMaterial& m, const phys::MpmPoint& p) {
    double s[3];
    phys::mpmKirchhoff(m, p.strain, s);
    return phys::mpmVonMises(s);
}

}  // namespace

TEST(mpm_sound_speed_and_yield) {
    const phys::MpmMaterial w = phys::mpmTungstenAlloy();
    CHECK_NEAR(phys::mpmSoundSpeed(w), std::sqrt((285e9 + 4.0 / 3.0 * 160e9) / 17600), 1e-6);
    CHECK_NEAR(phys::mpmYieldStress(w, 0, phys::kRoomTemperature), 1.5e9, 1);
    CHECK(phys::mpmYieldStress(w, 0.5, phys::kRoomTemperature) > 1.5e9);
    CHECK(phys::mpmYieldStress(w, 0, 1000) < 1.5e9);
    CHECK_NEAR(phys::mpmYieldStress(w, 0.3, w.meltK + 10), 0, 1e-6);
}

TEST(mpm_return_map_elastic_inside_yield) {
    const phys::MpmMaterial m = phys::mpmPenetratorSteel();
    phys::MpmPoint p = point(1e-3, -1e-3, 0);  // von Mises ~ 0.28 GPa, below 1.5 GPa
    const double before = vonMises(m, p);
    CHECK(before < m.yield);
    CHECK_NEAR(phys::mpmReturnMap(m, p), 0, 0);
    CHECK_NEAR(p.strain[0], 1e-3, 1e-15);
    CHECK_NEAR(p.strain[1], -1e-3, 1e-15);
    CHECK_NEAR(p.plasticStrain, 0, 0);
    CHECK(!p.failed);
}

TEST(mpm_return_map_lands_on_yield_surface) {
    phys::MpmMaterial m = phys::mpmPenetratorSteel();
    m.hardening = 0;  // perfectly plastic: the surface does not move during the step
    phys::MpmPoint p = point(0.03, -0.02, -0.005);
    const double trace = 0.03 - 0.02 - 0.005;
    CHECK(vonMises(m, p) > m.yield);
    const double dp = phys::mpmReturnMap(m, p);
    CHECK(dp > 0);
    CHECK_NEAR(p.plasticStrain, dp, 1e-15);
    // Thermal softening from this step's heat lowers the surface the next step; this step's
    // stress sits on the room-temperature surface.
    CHECK_NEAR(vonMises(m, p), m.yield, m.yield * 1e-9);
    // Plastic flow is isochoric.
    CHECK_NEAR(p.strain[0] + p.strain[1] + p.strain[2], trace, 1e-15);
    CHECK(p.temperature > phys::kRoomTemperature);
    CHECK_NEAR(p.temperature - phys::kRoomTemperature, phys::kTaylorQuinney * m.yield * dp / (m.density * m.heat),
               1e-9);
}

TEST(mpm_plane_strain_out_of_plane_stress) {
    // In-plane deviatoric strain only: plane strain gives sigma_zz = (s1 + s2) / 2 elastically.
    const phys::MpmMaterial m = phys::mpmPenetratorSteel();
    const double e[3] = {2e-3, -2e-3, 0};
    double s[3];
    phys::mpmKirchhoff(m, e, s);
    CHECK_NEAR(s[2], 0.5 * (s[0] + s[1]), 1);
    CHECK_NEAR(s[0], 2 * m.shear * 2e-3, 1);
}

TEST(mpm_failure_rules) {
    phys::MpmMaterial m = phys::mpmPenetratorSteel();
    // Spall: tensile mean stress beyond the spall strength fails the point and drops tension.
    phys::MpmPoint p = point(0.01, 0.01, 0.01);  // K * 0.03 = 4.8 GPa > 3.5 GPa
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    CHECK_NEAR(p.strain[0] + p.strain[1] + p.strain[2], 0, 0);
    // A failed point keeps compression, without shear.
    phys::MpmPoint c = point(-0.01, -0.02, 0);
    c.failed = true;
    phys::mpmReturnMap(m, c);
    CHECK_NEAR(c.strain[0], -0.01, 1e-15);
    CHECK_NEAR(c.strain[1], -0.01, 1e-15);
    CHECK_NEAR(c.strain[2], -0.01, 1e-15);
    // Accumulated plastic strain beyond the failure strain.
    phys::MpmPoint s = point(0.05, -0.05, 0);
    s.plasticStrain = m.failureStrain + 0.01;
    phys::mpmReturnMap(m, s);
    CHECK(s.failed);
    // Melting.
    phys::MpmPoint t = point(0, 0, 0);
    t.temperature = m.meltK;
    phys::mpmReturnMap(m, t);
    CHECK(t.failed);
}

TEST(mpm_flaw_factor_is_a_deterministic_weibull) {
    // The strength of a point relative to the material's mean: Weibull spread, mean one, the same
    // for the same point every time (the GPU hashes the point's initial position the same way).
    double sum10 = 0, sum4 = 0, sq10 = 0, sq4 = 0, min4 = 1e9;
    const int n = 20000;
    for (int i = 0; i < n; ++i) {
        const double f10 = phys::mpmFlawFactor(uint32_t(i), 7u, 10);
        const double f4 = phys::mpmFlawFactor(uint32_t(i), 7u, 4);
        CHECK(f10 == phys::mpmFlawFactor(uint32_t(i), 7u, 10));
        CHECK(f10 >= phys::kMinFlawFactor && f4 >= phys::kMinFlawFactor);
        sum10 += f10;
        sum4 += f4;
        sq10 += f10 * f10;
        sq4 += f4 * f4;
        min4 = std::min(min4, f4);
    }
    const double mean10 = sum10 / n, mean4 = sum4 / n;
    CHECK_NEAR(mean10, 1.0, 0.02);
    CHECK_NEAR(mean4, 1.0, 0.03);
    const double sd10 = std::sqrt(sq10 / n - mean10 * mean10), sd4 = std::sqrt(sq4 / n - mean4 * mean4);
    CHECK(sd10 > 0.05 && sd10 < 0.2);
    CHECK(sd4 > 1.5 * sd10);  // a lower Weibull modulus is a wider spread
    CHECK(min4 < 0.5);        // and has some weak flaws
    CHECK(phys::mpmFlawFactor(1u, 2u, 10) != phys::mpmFlawFactor(2u, 1u, 10));
}

TEST(mpm_brittle_metal_cracks_in_tension_not_compression) {
    phys::MpmMaterial m = phys::mpmPenetratorSteel();
    m.spall = 1e12;  // out of reach: only the brittle rule can fail it
    m.failureStrain = 10;
    m.brittleStress = 2e9;
    // Hydrostatic tension K tr = 1.6 GPa < 2 GPa: holds. 2.16 GPa: cracks, and drops its tension.
    phys::MpmPoint low = point(0.01 / 3, 0.01 / 3, 0.01 / 3);
    phys::mpmReturnMap(m, low);
    CHECK(!low.failed);
    phys::MpmPoint high = point(0.0135 / 3, 0.0135 / 3, 0.0135 / 3);
    phys::mpmReturnMap(m, high);
    CHECK(high.failed);
    CHECK(high.strain[0] + high.strain[1] + high.strain[2] <= 1e-15);
    // A flawed point (strength factor 0.7: 1.4 GPa) cracks at the load that the sound one holds.
    phys::MpmPoint flawed = point(0.01 / 3, 0.01 / 3, 0.01 / 3);
    flawed.flaw = 0.7;
    phys::mpmReturnMap(m, flawed);
    CHECK(flawed.failed);
    // The same tension in a ductile steel holds, and compression never cracks a brittle one.
    phys::MpmMaterial ductile = m;
    ductile.brittleStress = 0;
    phys::MpmPoint tough = point(0.0135 / 3, 0.0135 / 3, 0.0135 / 3);
    phys::mpmReturnMap(ductile, tough);
    CHECK(!tough.failed);
    phys::MpmPoint squeezed = point(-0.02, -0.02, -0.02);
    phys::mpmReturnMap(m, squeezed);
    CHECK(!squeezed.failed);
}

TEST(mpm_brittle_metal_cracks_on_the_largest_principal_stress) {
    // Uniaxial strain 1.1 %: the mean stress (1.76 GPa) is far below the spall strength, but the
    // stress along the pull (2.93 GPa) passes the brittle strength: a crack opens across it.
    phys::MpmMaterial m = phys::mpmPenetratorSteel();
    m.yield = 10e9;  // stays elastic
    m.brittleStress = 2.5e9;
    phys::MpmPoint p = point(0.011, 0, 0);
    double s[3];
    phys::mpmKirchhoff(m, p.strain, s);
    CHECK(m.bulk * 0.011 < m.spall);
    CHECK(s[0] > m.brittleStress);
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    // Under the strength it holds.
    phys::MpmPoint q = point(0.008, 0, 0);
    phys::mpmReturnMap(m, q);
    CHECK(!q.failed);
}

TEST(mpm_brittle_part_materials) {
    for (const char* id : {"tungsten_carbide", "cap_steel", "case_steel", "core_steel"}) {
        const auto m = phys::mpmPartMaterial(id);
        CHECK(m.has_value());
        if (!m) continue;
        CHECK(m->brittleStress > 0.5e9 && m->brittleStress < 5e9);
        CHECK(m->flawModulus >= 4 && m->flawModulus <= 30);
    }
    // Soft or tough metals deform instead of cracking.
    for (const char* id : {"gilding_metal", "lead", "mild_steel", "incendiary", "tungsten_alloy", "du", "light_alloy"})
        CHECK(phys::mpmPartMaterial(id)->brittleStress == 0);
    // Carbide is the most brittle of them: the widest spread, the weakest in tension for its stiffness.
    CHECK(phys::mpmPartMaterial("tungsten_carbide")->flawModulus < phys::mpmPartMaterial("core_steel")->flawModulus);
}

TEST(mpm_propellant_grain_crumbles) {
    // A rocket motor's grain is far weaker than any metal and cracks at a small tension: it breaks
    // into crumbs when the round hits instead of flowing like a ductile solid.
    const auto g = phys::mpmPartMaterial("propellant");
    CHECK(g.has_value());
    if (!g) return;
    CHECK(g->density > 1500 && g->density < 1900);
    CHECK(g->brittleStress > 0 && g->brittleStress < 0.2e9);
    CHECK(g->flawModulus >= 4 && g->flawModulus <= 30);
    CHECK(g->yield < phys::mpmLightAlloy().yield);
    CHECK(g->spall < phys::mpmLightAlloy().spall);
    CHECK(g->detonationSpeed == 0);  // a grain burns, it does not detonate
}

TEST(mpm_shipped_material_blocks) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    const std::string dir = test::dataDir();
    CHECK(db.load(dir + "/armour_materials.json", dir + "/projectiles.json", report, fatal));
    CHECK(report.skipped == 0);
    for (const ArmourMaterialEntry& e : db.materials()) {
        CHECK(e.mpmFromData);
        CHECK_NEAR(e.mpm.density, e.material.density, 0);
        CHECK(e.mpm.shear > 0 && e.mpm.bulk > 0 && e.mpm.meltK > phys::kRoomTemperature);
        CHECK(std::isfinite(phys::mpmSoundSpeed(e.mpm)));
    }
    const int rha = db.findMaterial("rha");
    CHECK(rha >= 0);
    CHECK_NEAR(db.materials()[rha].mpm.yield, 1.0e9, 1);
    CHECK_NEAR(db.materials()[rha].mpm.shear, 77.5e9, 1);
}

TEST(mpm_material_fallback_and_errors) {
    const char* projectiles = R"({"schema":1,"projectiles":[]})";
    const char* materials = R"({"schema":1,"materials":[
      {"id":"hard","name":"h","group":"steel","density_kgm3":7850,"hardness_bhn":500,"ke_efficiency":1,
       "confidence":"low","sources":["s"]},
      {"id":"part","name":"p","group":"light_metal","density_kgm3":2700,"hardness_bhn":100,"resistance_gpa":2,
       "ke_efficiency":1,"mpm":{"yield_gpa":0.3},"confidence":"low","sources":["s"]},
      {"id":"bad","name":"b","group":"steel","density_kgm3":7850,"hardness_bhn":300,"ke_efficiency":1,
       "mpm":{"shear_gpa":-1},"confidence":"low","sources":["s"]},
      {"id":"bad2","name":"b2","group":"steel","density_kgm3":7850,"hardness_bhn":300,"ke_efficiency":1,
       "mpm":3,"confidence":"low","sources":["s"]}
    ]})";
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    CHECK(db.loadText(materials, projectiles, report, fatal));
    CHECK(report.skipped == 2);
    CHECK(db.materials().size() == 2u);
    const ArmourMaterialEntry& hard = db.materials()[0];
    CHECK(!hard.mpmFromData);
    CHECK_NEAR(hard.mpm.yield, 3.3e6 * 500, 1);
    CHECK(hard.mpm.failureStrain < 1.0);
    const ArmourMaterialEntry& part = db.materials()[1];
    CHECK(part.mpmFromData);
    CHECK_NEAR(part.mpm.yield, 0.3e9, 1);
    CHECK_NEAR(part.mpm.shear, 26.9e9, 1);  // from the aluminium fallback
    CHECK(report.messages.size() == 2u);
    CHECK(report.messages[0].find("mpm.shear_gpa") != std::string::npos);
}

TEST(mpm_rod_materials) {
    CHECK_NEAR(rodMaterialMpm(RodMaterial::Tungsten).density, rodMaterialDensity(RodMaterial::Tungsten), 0);
    CHECK_NEAR(rodMaterialMpm(RodMaterial::Uranium).density, rodMaterialDensity(RodMaterial::Uranium), 0);
    CHECK_NEAR(rodMaterialMpm(RodMaterial::Steel).density, rodMaterialDensity(RodMaterial::Steel), 0);
    // Light alloy of a dart's fittings: much lighter and weaker than the rod.
    const phys::MpmMaterial a = phys::mpmLightAlloy();
    CHECK_NEAR(a.density, 2800, 0);
    CHECK(a.yield < 0.5 * phys::mpmTungstenAlloy().yield);
    CHECK(a.meltK < 1000);
    CHECK(phys::mpmSoundSpeed(a) > 5500 && phys::mpmSoundSpeed(a) < 6500);
}

// ---------------------------------------------------------------- scene setup

#include "sim/armour_run.h"
#include "sim/mpm_scene.h"

#include <algorithm>
#include <set>

namespace {

const ProjectileDb& sceneDb() {
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

struct SceneCase {
    ArmourCase c;
    RodMaterial rod = RodMaterial::Steel;
    phys::ProjectileShape shape;  // the catalogue's, scaled to the case
    ArmourOutcome out;
};

// NaN obliquity: the preset's.
SceneCase sceneCase(const char* projectile, const char* preset, double obliquity = std::nan("")) {
    const ProjectileDb& db = sceneDb();
    SceneCase s;
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(db.findProjectile(projectile))];
    s.c.penetrator = e.penetrator;
    s.c.velocity = e.velocity;
    s.rod = e.rodMaterial;
    s.shape = phys::shapeForCase(e.shape.present ? &e.shape : nullptr, s.c.penetrator);
    for (const ArmourPreset& p : db.presets())
        if (p.id == preset) {
            s.c.obliquityDeg = p.obliquityDeg;
            s.c.layers = p.layers;
        }
    if (!std::isnan(obliquity)) s.c.obliquityDeg = obliquity;
    s.out = evaluateArmour(db, s.c);
    return s;
}

MpmScene build(const SceneCase& s, MpmQuality q = MpmQuality::Medium) {
    return buildMpmScene(sceneDb(), s.c, s.rod, s.shape, s.out, q);
}

// Generic shaped charge (80 mm copper cone, q = 0.5) at a standoff of cone diameters, against
// RHA of the given thickness.
SceneCase jetCase(double standoff, double rha = 0.1) {
    const ProjectileDb& db = sceneDb();
    SceneCase s;
    s.c.penetrator.kind = phys::PenetratorKind::ShapedCharge;
    s.c.penetrator.coneDiameter = 0.08;
    s.c.penetrator.standoff = standoff;
    s.c.penetrator.jetQuality = 0.5;
    s.c.penetrator.linerDensity = 8930;
    s.c.layers.push_back({db.findMaterial("rha"), rha, 0});
    s.out = evaluateArmour(db, s.c);
    return s;
}

struct JetRow {
    float x, kick;
    bool neck;
    int count;
};

// One entry per row across the jet, tip first.
std::vector<JetRow> jetRows(const MpmScene& scene) {
    std::vector<JetRow> rows;
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& p = scene.particles[static_cast<size_t>(i)];
        if (!rows.empty() && rows.back().x == p.x) {
            ++rows.back().count;
            continue;
        }
        rows.push_back({p.x, p.kick, (p.flags & kMpmJetNeck) != 0, 1});
    }
    std::sort(rows.begin(), rows.end(), [](const JetRow& a, const JetRow& b) { return a.x > b.x; });
    return rows;
}

}  // namespace

TEST(mpm_scene_jet_stream) {
    const SceneCase s = jetCase(2);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    CHECK(scene.jet);
    const phys::JetParameters jp = phys::jetParameters(s.c.penetrator);
    const double S = jp.virtualOrigin, t0 = S / jp.tipVelocity, ts = scene.tStart;
    // The model's own start (0.25 t0, times since formation), before breakup at this standoff.
    CHECK_NEAR(ts, s.out.result.startTime, 1e-15);
    CHECK_NEAR(ts, 0.25 * t0, 1e-12);
    CHECK(ts < jp.breakupTime);
    CHECK_NEAR(scene.jetBreakup, jp.breakupTime, 1e-15);
    CHECK_NEAR(scene.jetOrigin, S, 1e-15);
    CHECK_NEAR(scene.velocity, jp.tipVelocity, 1e-9);
    CHECK_NEAR(scene.diameter, kMpmJetReference * 0.08, 1e-15);
    CHECK_NEAR(scene.materials[0].density, 8930, 1e-9);
    CHECK_NEAR(scene.materials[0].yield, phys::mpmLinerCopper().yield, 1e-9);
    // The time step allows for the tip velocity.
    CHECK(scene.dt <= kMpmCfl * scene.dx / (scene.maxSoundSpeed + jp.tipVelocity) * (1 + 1e-12));
    CHECK(scene.bodyCount == scene.projectileCount && scene.projectileCount > 0);
    double xmin = 1e9, xmax = -1e9, area = 0;
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& p = scene.particles[static_cast<size_t>(i)];
        CHECK((p.flags & (kMpmProjectile | kMpmJet)) == (kMpmProjectile | kMpmJet));
        CHECK((p.flags & (kMpmJetBroken | kMpmFailed)) == 0);
        CHECK(p.material == 0);
        // The model's velocity at its place, and the inflow of a sheet thinning as 1 / t.
        CHECK_NEAR(p.vx, (p.x + S) / ts, 1e-4 * jp.tipVelocity);
        CHECK_NEAR(p.vy, -p.y / ts, 1e-3);
        CHECK_NEAR(p.temp, kMpmJetTemperature, 1e-3);
        CHECK(p.x > scene.originX && p.x < scene.originX + (scene.nx - 1) * scene.dx);
        CHECK(p.y > scene.originY && p.y < scene.originY + (scene.ny - 1) * scene.dx);
        xmin = std::min(xmin, static_cast<double>(p.x));
        xmax = std::max(xmax, static_cast<double>(p.x));
        area += p.volume;
    }
    CHECK_NEAR(area, scene.projectileArea, 1e-6 * area);
    CHECK_NEAR(area, 2.4 * jp.jetRadius * t0 * (jp.tipVelocity - jp.minVelocity), 0.05 * area);
    const double hj = (jp.tipVelocity - jp.minVelocity) * ts / static_cast<double>(jetRows(scene).size());
    CHECK_NEAR(xmin, jp.minVelocity * ts - S, 0.51 * hj + 1e-6);
    CHECK_NEAR(xmax, jp.tipVelocity * ts - S, 0.51 * hj + 1e-6);
    CHECK_NEAR(scene.bodyNose, jp.tipVelocity * ts - S, 1e-12);
    CHECK(xmax < 0);  // the tip starts before the first face
    // Plates follow the jet, as for kinetic rounds.
    CHECK(static_cast<int>(scene.particles.size()) > scene.projectileCount);
    CHECK(scene.particles.back().material == 1);
}

TEST(mpm_scene_jet_segments) {
    const SceneCase s = jetCase(2);
    const MpmScene scene = build(s);
    const phys::JetParameters jp = phys::jetParameters(s.c.penetrator);
    const std::vector<JetRow> rows = jetRows(scene);
    // Rows about 1.5 particle spacings apart at breakup, at least two particles across.
    const double gap = (rows.front().x - rows.back().x) / (rows.size() - 1) * jp.breakupTime / scene.tStart;
    CHECK_NEAR(gap, 1.5 * scene.spacing, 0.05 * scene.spacing);
    int necks = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].count >= 2);
        if (rows[i].neck) ++necks;
        // The kick is constant along a segment and changes at the tip row of the next (its neck).
        if (i > 0 && rows[i].kick != rows[i - 1].kick) CHECK(rows[i].neck);
        CHECK(std::abs(rows[i].kick) <= jp.drift * jp.tipVelocity * 3.47);
    }
    CHECK(!rows.front().neck);
    const double segment = std::max(kMpmJetSegment * jp.jetRadius, 3 * scene.dx);
    const double expected = (jp.tipVelocity - jp.minVelocity) * jp.breakupTime / segment;
    CHECK(necks > 10);
    CHECK(std::abs(necks - expected) <= 1.5);
    // Every particle of a neck row is a neck.
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& p = scene.particles[static_cast<size_t>(i)];
        for (const JetRow& r : rows)
            if (r.x == p.x) CHECK(((p.flags & kMpmJetNeck) != 0) == r.neck);
    }
}

TEST(mpm_scene_jet_long_standoff_seeded_broken) {
    // At 60 cone diameters the model's start is past breakup: the jet starts in pieces.
    const SceneCase s = jetCase(60, 0.05);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    const phys::JetParameters jp = phys::jetParameters(s.c.penetrator);
    const double ts = scene.tStart, tb = jp.breakupTime;
    CHECK(ts > tb);
    CHECK(scene.nx <= kMpmMaxCells && scene.ny <= kMpmMaxCells);
    int necks = 0;
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& p = scene.particles[static_cast<size_t>(i)];
        CHECK((p.flags & kMpmJetBroken) != 0);
        if (p.flags & kMpmJetNeck) ++necks;
        CHECK(((p.flags & kMpmFailed) != 0) == ((p.flags & kMpmJetNeck) != 0));
        // Kicked at breakup and drifted since, necks included.
        const double y0 = p.y - p.kick * (ts - tb);
        CHECK_NEAR(p.vy, -y0 / ts + p.kick, 1e-3);
    }
    CHECK(necks > 0);
}

#if 0  // Disabled: the HEAT warhead model is blocked in this release, so a HEAT body is not seeded.
TEST(mpm_scene_heat_body_seeded_dormant_jet) {
    // An RPG-7 round: the rocket flies in carrying its charge; the jet waits, dormant, inside the
    // cone until the liner has collapsed (the model's start), then takes over from the liner.
    const SceneCase s = sceneCase("pg7v", "rha_100");
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    CHECK(scene.jet);
    CHECK(scene.heatBody);
    CHECK(scene.tStart < 0);
    CHECK_NEAR(scene.jetRelease, s.out.result.startTime, 1e-15);
    CHECK(scene.fuzeProgrammed);
    CHECK(scene.fuzeLightTime > scene.tStart && scene.fuzeLightTime < 0);
    int dormant = 0, liner = 0, filler = 0, body = 0;
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& p = scene.particles[static_cast<size_t>(i)];
        CHECK((p.flags & kMpmProjectile) != 0);
        const bool jetOnly = (p.flags & kMpmJet) && !(p.flags & kMpmFitting);
        if (jetOnly) {
            CHECK((p.flags & kMpmDormant) != 0);
            CHECK(i < scene.bodyCount);
            ++dormant;
        } else {
            CHECK((p.flags & kMpmDormant) == 0);
            CHECK((p.flags & kMpmFitting) != 0);
            CHECK_NEAR(p.vx, s.c.velocity, 1e-3);
            CHECK(i >= scene.bodyCount);
            ++body;
            if (p.flags & kMpmJet) ++liner;
            if (p.flags & kMpmFiller) ++filler;
        }
        CHECK(p.x > scene.originX && p.x < scene.originX + (scene.nx - 1) * scene.dx);
        CHECK(p.y > scene.originY && p.y < scene.originY + (scene.ny - 1) * scene.dx);
    }
    CHECK(dormant > 0 && liner > 0 && filler > 0 && body > liner + filler);
    CHECK(scene.bodyCount == dormant);
    CHECK(scene.fillerCount == filler);
    // The bare generic charge keeps the jet alone.
    CHECK(!build(jetCase(2)).heatBody);
}
#endif

TEST(mpm_scene_rod_against_plate) {
    const SceneCase s = sceneCase("m829", "rha_100", 0);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    CHECK(!scene.jet);
    for (size_t i = 0; i < scene.particles.size(); ++i) {
        const MpmSeed& p = scene.particles[i];
        CHECK_NEAR(p.vx, static_cast<int>(i) < scene.projectileCount ? s.c.velocity : 0.0, 1e-3);
        CHECK(p.vy == 0 && p.kick == 0 && p.temp == 0);
        CHECK((p.flags & (kMpmJet | kMpmJetNeck | kMpmJetBroken)) == 0);
    }
    const phys::Penetrator& p = s.c.penetrator;
    CHECK_NEAR(scene.dx, p.diameter / mpmCellsPerDiameter(MpmQuality::Medium), 1e-12);
    CHECK(scene.particlesPerSide == 2);
    CHECK_NEAR(scene.spacing, 0.5 * scene.dx, 1e-15);
    // Projectile first, aligned with the shot line: its body, the dart's cap tip kMpmPrerollCells
    // cells before the face, so the body's nose is the cap's length further back.
    CHECK(scene.dart);
    CHECK(scene.bodyCount > 0 && scene.projectileCount > scene.bodyCount);
    double nose = -1e9, tail = 1e9, yMax = 0;
    for (int i = 0; i < scene.bodyCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        CHECK(q.flags & kMpmProjectile);
        CHECK(!(q.flags & kMpmFitting));
        CHECK(q.material == 0);
        nose = std::max(nose, static_cast<double>(q.x));
        tail = std::min(tail, static_cast<double>(q.x));
        yMax = std::max(yMax, std::abs(static_cast<double>(q.y)));
    }
    const double pre = kMpmPrerollCells * scene.dx, cap = kDartCapLength * p.diameter;
    CHECK(nose < -pre - cap && nose > -pre - cap - scene.spacing);
    CHECK(tail > -pre - cap - p.length && tail < -pre - cap - p.length + scene.spacing);
    CHECK_NEAR(scene.bodyNose, -pre - cap, 1e-12);
    CHECK_NEAR(scene.bodyLength, p.length, 0);
    CHECK(yMax < 0.5 * p.diameter);
    CHECK_NEAR(scene.projectileArea, p.length * p.diameter, 1e-9);
    const double expected = p.length * p.diameter / (scene.spacing * scene.spacing);
    CHECK(std::abs(scene.bodyCount - expected) < 0.1 * expected);
    // The fittings follow the body, in their own (last) material.
    for (int i = scene.bodyCount; i < scene.projectileCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        CHECK((q.flags & kMpmProjectile) && (q.flags & kMpmFitting));
        CHECK(q.material + 1u == scene.materials.size());
    }
    CHECK_NEAR(scene.materials.back().density, 2800, 0);
    // Plate particles: in the plate, not flagged projectile; plate area covered.
    double plateArea = 0;
    for (size_t i = static_cast<size_t>(scene.projectileCount); i < scene.particles.size(); ++i) {
        const MpmSeed& q = scene.particles[i];
        CHECK(!(q.flags & (kMpmProjectile | kMpmFitting)));
        CHECK(q.material == 1);
        CHECK(q.x > 0 && q.x < 0.1);
        plateArea += q.volume;
    }
    CHECK_NEAR(plateArea, 0.1 * 2 * scene.plateHalfExtent, 1e-6);
    CHECK_NEAR(scene.materials[0].density, p.density, 0);
    CHECK_NEAR(scene.materials[1].yield, 1.0e9, 1);
    // Time step from the CFL condition, start in the preroll, end after the model's end.
    CHECK_NEAR(scene.dt, kMpmCfl * scene.dx / (scene.maxSoundSpeed + s.c.velocity), 1e-18);
    CHECK_NEAR(scene.tStart, -(pre + cap) / s.c.velocity, 1e-15);
    CHECK(scene.tEnd >= s.out.result.endTime);
}

TEST(mpm_scene_domain_has_room_behind_and_beside) {
    // The animation can run on past the model's end: the residual rod keeps its room behind the
    // stack, and debris thrown sideways stays on the grid well beyond the plates' edges.
    const SceneCase s = sceneCase("m829", "rha_100", 0);
    const MpmScene scene = build(s);
    const phys::PenetrationResult& r = s.out.result;
    CHECK(r.perforated);
    const double D = s.c.penetrator.diameter, len = s.c.penetrator.length;
    const double behind = scene.originX + (scene.nx - 1) * scene.dx - 0.1;
    const double tEnd = 1.25 * r.endTime;
    CHECK(behind >= std::max(3 * D, std::min(r.residualVelocity * (2.5 * tEnd - r.exitTime), 3 * len)));
    CHECK(-scene.originY >= 1.9 * scene.plateHalfExtent);
    CHECK(scene.originY + (scene.ny - 1) * scene.dx >= 1.9 * scene.plateHalfExtent);
}

TEST(mpm_scene_dart_fittings) {
    const SceneCase s = sceneCase("generic_30mm_apfsds", "rha_100", 0);
    const MpmScene scene = build(s);
    CHECK(scene.supported && scene.dart);
    const double D = s.c.penetrator.diameter, R = 0.5 * D, nose = scene.bodyNose, tail = nose - scene.bodyLength;
    const double h = scene.spacing;
    double capArea = 0, finArea = 0, tracerArea = 0, capTip = -1e9;
    int finsAbove = 0, finsBelow = 0;
    for (int i = scene.bodyCount; i < scene.projectileCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        const double ay = std::abs(static_cast<double>(q.y));
        if (q.flags & kMpmTracer) {
            // Behind the tail, on the axis.
            CHECK(q.x < tail && q.x > tail - kDartTracerLength * D);
            CHECK(ay < 0.5 * kDartTracerWidth * D);
            tracerArea += q.volume;
        } else if (q.x > nose) {
            // The cap: inside the cone ahead of the nose, hollow behind its tip.
            const double outer = R * (1 - (q.x - nose) / (kDartCapLength * D));
            CHECK(ay <= outer + 1e-6);  // positions are floats
            CHECK(ay >= outer - kDartCapWall * D * 1.05);
            capTip = std::max(capTip, static_cast<double>(q.x));
            capArea += q.volume;
        } else {
            // A fin: beside the body at the tail, within the span and the swept leading edge.
            CHECK(ay > R && ay < 0.5 * kDartFinSpan * D);
            CHECK(q.x > tail && q.x < tail + kDartFinRootChord * D);
            const double chord = kDartFinRootChord + (kDartFinTipChord - kDartFinRootChord) * (ay - R) / (0.5 * kDartFinSpan * D - R);
            CHECK(q.x - tail < chord * D + 1e-6);
            (q.y > 0 ? finsAbove : finsBelow)++;
            finArea += q.volume;
        }
    }
    // Cap tip three cells before the face (the lattice row nearest the axis ends where the cone
    // narrows below it); areas near the shapes' (the cap: outer cone less the inner one, its wall
    // measured across).
    const double capSlack = (kDartCapLength * D / R + 1) * h;
    CHECK(capTip > -kMpmPrerollCells * scene.dx - capSlack && capTip < -kMpmPrerollCells * scene.dx);
    const double wall = kDartCapWall * D * std::hypot(1.0, R / (kDartCapLength * D));
    const double capExpected = R * kDartCapLength * D - (R - wall) * (R - wall) * kDartCapLength * D / R;
    CHECK(std::abs(capArea - capExpected) < 0.15 * capExpected);
    const double finExpected = (kDartFinRootChord + kDartFinTipChord) * D * (0.5 * kDartFinSpan * D - R);
    CHECK(std::abs(finArea - finExpected) < 0.1 * finExpected);
    CHECK(finsAbove == finsBelow && finsAbove > 0);
    const double tracerExpected = kDartTracerLength * kDartTracerWidth * D * D;
    CHECK(std::abs(tracerArea - tracerExpected) < 0.15 * tracerExpected);
    CHECK(scene.materials.size() == 3u);  // rod, plate, fittings
    CHECK(scene.maxSoundSpeed >= phys::mpmSoundSpeed(scene.materials[2]));

    // Short rods (L < 5 D) and compact shot get no fittings; the nose leads at 0 deg.
    SceneCase shortRod = s;
    shortRod.c.penetrator.length = 4 * D;
    shortRod.shape = phys::shapeForCase(nullptr, shortRod.c.penetrator);
    shortRod.out = evaluateArmour(sceneDb(), shortRod.c);
    SceneCase compact = sceneCase("122mm_br471", "rha_100", 0);
    compact.shape = phys::shapeForCase(nullptr, compact.c.penetrator);  // the catalogue's has a band and tracer
    for (const MpmScene& plain : {build(shortRod), build(compact)}) {
        CHECK(plain.supported && !plain.dart);
        CHECK(plain.bodyCount == plain.projectileCount);
        CHECK(plain.materials.size() == 2u);
        CHECK_NEAR(plain.tStart, -kMpmPrerollCells * plain.dx / plain.velocity, 1e-15);
        for (const MpmSeed& q : plain.particles) CHECK(!(q.flags & (kMpmFitting | kMpmTracer)));
    }
}

TEST(mpm_scene_particles_inside_plates_at_obliquity) {
    // A dart, and a shell under a cap and a windshield (negative angles: the same event mirrored).
    const std::pair<const char*, double> cases[] = {{"dm53", 0.0},  {"dm53", 30.0}, {"dm53", 60.0}, {"dm53", 80.0},
                                                    {"88mm_pzgr39_kwk36", -60.0}, {"88mm_pzgr39_kwk36", 60.0}};
    for (const auto& [projectile, ob] : cases) {
        const SceneCase s = sceneCase(projectile, "spaced_steel", ob);
        const MpmScene scene = build(s);
        CHECK(scene.supported);
        const double tanT = std::tan(std::abs(ob) * 3.14159265358979323846 / 180.0);
        const auto& layers = s.out.result.layers;
        // The whole projectile (fins and nose corners too) starts clear of the first face.
        for (int i = 0; i < scene.projectileCount; ++i) {
            const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
            CHECK(q.x + q.y * tanT < layers[0].front - kMpmPrerollCells * scene.dx + 1e-9);
        }
        int anchored = 0;
        for (size_t i = static_cast<size_t>(scene.projectileCount); i < scene.particles.size(); ++i) {
            const MpmSeed& q = scene.particles[i];
            const double along = q.x + q.y * tanT;
            const auto& l = layers[q.layer];
            CHECK(along > l.front - 1e-6 && along < l.back + 1e-6);
            if (q.flags & kMpmAnchored) ++anchored;
        }
        CHECK(anchored > 0);
        // Everything inside the grid, at least two cells from its edges.
        for (const MpmSeed& q : scene.particles) {
            const double gx = (q.x - scene.originX) / scene.dx, gy = (q.y - scene.originY) / scene.dx;
            CHECK(gx > 2 && gx < scene.nx - 3 && gy > 2 && gy < scene.ny - 3);
        }
        CHECK(scene.nx <= kMpmMaxCells && scene.ny <= kMpmMaxCells);
    }
}

TEST(mpm_scene_filler_flagged) {
    // An APHE shell: its filler particles are fittings flagged as filler; the base fuze lights
    // it at its rear, on the shot line, in initial-position cells.
    const SceneCase s = sceneCase("88mm_pzgr39_kwk36", "rha_50", 0);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    CHECK(scene.fuze.type == phys::FuzeType::BaseDelay);
    int fillers = 0;
    double rear = 1e30, front = -1e30;
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        if (!(q.flags & kMpmFiller)) continue;
        ++fillers;
        CHECK((q.flags & kMpmFitting) && (q.flags & kMpmProjectile));
        CHECK(phys::mpmReactive(scene.materials[q.material]));
        rear = std::min(rear, static_cast<double>(q.x));
        front = std::max(front, static_cast<double>(q.x));
    }
    for (size_t i = static_cast<size_t>(scene.projectileCount); i < scene.particles.size(); ++i)
        CHECK(!(scene.particles[i].flags & kMpmFiller));
    CHECK(fillers > 0 && fillers == scene.fillerCount);
    // The rear face lies half a row behind the rearmost row of filler particles.
    const double originX = (scene.fuzeOrigin[0] * scene.dx) + scene.originX;
    CHECK(originX < rear && originX > rear - scene.dx);
    CHECK(front - rear > 0.03);
    CHECK_NEAR(scene.fuzeOrigin[1] * scene.dx + scene.originY, 0, 1e-9);
    CHECK(scene.fuzeMassUnit > 0 && scene.fuzeTotalUnits > 0);
    // A round without a filler has no fuze.
    const MpmScene plain = build(sceneCase("17pdr_apcbc", "rha_50", 0));
    CHECK(plain.fuze.type == phys::FuzeType::None && plain.fillerCount == 0);
}

TEST(mpm_gurney_velocity_cylinder) {
    // Gurney's cylinder, V = sqrt(2E) / sqrt(M/C + 1/2), and Taylor's tilt of the metal's flight
    // towards the way the detonation travels, sin(theta) = V / (2 D).
    CHECK_NEAR(phys::gurneyCylinderVelocity(2700, 1.0, 1.0), 2700 / std::sqrt(1.5), 1e-9);
    CHECK_NEAR(phys::gurneyCylinderVelocity(2700, 0.0, 1.0), 2700 / std::sqrt(0.5), 1e-9);
    CHECK(phys::gurneyCylinderVelocity(2700, 10.0, 1.0) < phys::gurneyCylinderVelocity(2700, 1.0, 1.0));
    CHECK_NEAR(phys::gurneyCylinderVelocity(2700, 5.0, 0.0), 0.0, 1e-12);  // no charge, no push
    CHECK_NEAR(phys::taylorAngle(2000, 8000), std::asin(0.125), 1e-12);
    CHECK_NEAR(phys::taylorAngle(0, 8000), 0.0, 1e-12);
    CHECK_NEAR(phys::taylorAngle(20000, 8000), 1.5707963267948966, 1e-12);  // capped at a right angle
    CHECK_NEAR(phys::taylorAngle(2000, 0), 0.0, 1e-12);                      // inert: no sweep
}

TEST(mpm_scene_burst_follows_the_sweep) {
    // An APHE shell lit at its base: the case alongside the charge (between the filler's extreme
    // particles) is driven at the cylinder's Gurney speed, its flight tilted towards the nose.
    const MpmScene s = build(sceneCase("88mm_pzgr39_kwk36", "rha_50", 0));
    CHECK(s.supported && s.fillerCount > 0);
    double charge = 0, lo = 1e30, hi = -1e30, energy = 0, speed = 0;
    for (int i = 0; i < s.projectileCount; ++i) {
        const MpmSeed& q = s.particles[static_cast<size_t>(i)];
        if (!(q.flags & kMpmFiller)) continue;
        const phys::MpmMaterial& m = s.materials[q.material];
        charge += m.density * q.volume;
        energy = m.energy;
        speed = m.detonationSpeed;
        lo = std::min(lo, (q.x - s.originX) / s.dx);
        hi = std::max(hi, (q.x - s.originX) / s.dx);
    }
    double wall = 0;
    for (int i = 0; i < s.projectileCount; ++i) {
        const MpmSeed& q = s.particles[static_cast<size_t>(i)];
        const double cx = (q.x - s.originX) / s.dx;
        if ((q.flags & (kMpmFiller | kMpmFitting)) || cx < lo || cx > hi) continue;
        wall += s.materials[q.material].density * q.volume;
    }
    CHECK(wall > 0 && charge > 0 && speed > 0);
    const double expected = phys::gurneyCylinderVelocity(std::sqrt(2 * energy), wall, charge);
    CHECK(s.burstGurney > 0);
    CHECK_NEAR(s.burstGurney, expected, 1e-3 * expected);
    CHECK_NEAR(s.burstTaylor, phys::taylorAngle(s.burstGurney, speed), 1e-9);
    CHECK_NEAR(s.burstSpeed, speed, 1e-9);
    CHECK(s.burstSweep == 1);
    CHECK_NEAR(s.burstSpan[0], lo, 1e-6);
    CHECK_NEAR(s.burstSpan[1], hi, 1e-6);
    // No filler, no burst; a rocket lit at its nose sweeps the other way.
    const MpmScene plain = build(sceneCase("17pdr_apcbc", "rha_50", 0));
    CHECK(plain.burstGurney == 0 && plain.burstTaylor == 0 && plain.burstSpeed == 0);
    const MpmScene rocket = build(sceneCase("s8of", "rha_50", 0));
    CHECK(rocket.supported && rocket.burstSweep == -1 && rocket.burstGurney > 0);
}

TEST(mpm_scene_missile_motor_simulated) {
    // A Hellfire is seeded whole: its motor tube with the nozzle plate, the propellant grain and the
    // fins are particles too, so the round crumples from nose to tail rather than stopping at the warhead.
    const SceneCase s = sceneCase("agm114m", "rha_50", 0);
    const MpmScene scene = build(s);
    CHECK(scene.supported && scene.fillerCount > 0);
    const size_t first = 1 + s.c.layers.size();
    int grain = -1;
    for (size_t i = 0; i < scene.partMaterials.size(); ++i)
        if (scene.partMaterials[i] == "propellant") grain = static_cast<int>(first + i);
    CHECK(grain > 0);
    double lo = 1e30, hi = -1e30, grainHi = -1e30, grainY = 0;
    int grains = 0, fins = 0;
    const double R = 0.5 * s.c.penetrator.diameter;  // the tube's; the shape's own diameter is the fins' span
    for (int i = 0; i < scene.projectileCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        lo = std::min(lo, static_cast<double>(q.x));
        hi = std::max(hi, static_cast<double>(q.x));
        if (std::abs(q.y) > 1.02 * R) ++fins;  // beyond the tube's radius: only the fins reach there
        if (q.material != static_cast<uint16_t>(grain)) continue;
        ++grains;
        grainHi = std::max(grainHi, static_cast<double>(q.x));
        grainY = std::max(grainY, static_cast<double>(std::abs(q.y)));
    }
    CHECK(grains > 20);
    CHECK(fins > 0);
    CHECK(hi - lo > 0.95 * s.shape.length);
    // The grain lies inside the motor tube, behind the warhead.
    double warheadEnd = 0;
    for (const phys::ShapePart& part : s.shape.parts)
        if (part.role == phys::PartRole::Case) warheadEnd = part.stations.back().x;
    CHECK(warheadEnd > 0);
    CHECK(grainY < R);
    CHECK(grainHi < hi - warheadEnd + 2 * scene.spacing);
}

TEST(mpm_scene_parts_get_own_materials) {
    // An APCBC shell: its body (the engineering penetrator) is material 0; the cap, windshield,
    // band and tracer follow the plates, one material per part material, flagged as fittings.
    const SceneCase s = sceneCase("88mm_pzgr39_kwk36", "rha_100", 0);
    const MpmScene scene = build(s);
    CHECK(scene.supported && !scene.dart);
    const size_t first = 1 + s.c.layers.size();
    CHECK(scene.partMaterials.size() + first == scene.materials.size());
    auto index = [&](const char* id) {
        int found = -1;
        for (size_t i = 0; i < scene.partMaterials.size(); ++i)
            if (scene.partMaterials[i] == id) {
                CHECK(found < 0);  // one material per id
                found = static_cast<int>(first + i);
            }
        return found;
    };
    const int cap = index("cap_steel"), ws = index("light_alloy");
    CHECK(cap > 0 && ws > 0 && index("gilding_metal") > 0 && index("incendiary") > 0);
    CHECK_NEAR(scene.materials[static_cast<size_t>(cap)].density, phys::mpmPartMaterial("cap_steel")->density, 0);
    CHECK_NEAR(scene.materials[0].density, s.c.penetrator.density, 0);
    for (int i = 0; i < scene.bodyCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        CHECK(q.material == 0 && (q.flags & kMpmProjectile) && !(q.flags & kMpmFitting));
    }
    int caps = 0, shields = 0, tracers = 0;
    double capTip = -1e9, shieldTip = -1e9;
    for (int i = scene.bodyCount; i < scene.projectileCount; ++i) {
        const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
        CHECK((q.flags & kMpmProjectile) && (q.flags & kMpmFitting));
        CHECK(q.material >= first);
        if (q.material == cap) {
            ++caps;
            capTip = std::max(capTip, static_cast<double>(q.x));
        }
        if (q.material == ws) {
            ++shields;
            shieldTip = std::max(shieldTip, static_cast<double>(q.x));
        }
        if (q.flags & kMpmTracer) ++tracers;
    }
    CHECK(caps > 0 && shields > 0 && tracers > 0);
    // The windshield leads, the cap behind its tip; the round's tip starts the preroll from the face.
    CHECK(shieldTip > capTip);
    CHECK(shieldTip < -kMpmPrerollCells * scene.dx && shieldTip > -kMpmPrerollCells * scene.dx - 2 * scene.spacing);
}

TEST(mpm_scene_thin_parts_seeded_low_quality) {
    // .50 AP at Low: the 1 mm jacket and the lead sleeve (a sliver round the core) are thinner than
    // the spacing along much of their length, yet each keeps points on both sides all along it.
    const SceneCase s = sceneCase("m2_ap_50", "rha_100", 0);
    const MpmScene scene = build(s, MpmQuality::Low);
    CHECK(scene.supported);
    const size_t first = 1 + s.c.layers.size();
    int checked = 0;
    for (const phys::ShapePart& part : s.shape.parts) {
        if (part.role != phys::PartRole::Jacket && part.role != phys::PartRole::Sleeve) continue;
        ++checked;
        int m = -1;
        for (size_t i = 0; i < scene.partMaterials.size(); ++i)
            if (scene.partMaterials[i] == part.material) m = static_cast<int>(first + i);
        CHECK(m > 0);
        // Where the part has any thickness (the sleeve vanishes where the core fills the jacket).
        double length = 0;
        const double step = 0.01 * scene.spacing;
        for (double x = part.stations.front().x + 0.5 * step; x < part.stations.back().x; x += step) {
            double rIn = 0, rOut = 0;
            if (phys::partRadiiAt(part, x, rIn, rOut) && rOut > rIn) length += step;
        }
        double area = 0;
        for (const double side : {1.0, -1.0}) {
            std::set<long> rows;
            for (int i = scene.bodyCount; i < scene.projectileCount; ++i) {
                const MpmSeed& q = scene.particles[static_cast<size_t>(i)];
                if (q.material != m || q.y * side <= 0) continue;
                rows.insert(std::lround(q.x / scene.spacing));
                area += q.volume;
            }
            CHECK(rows.size() >= 2u);
            CHECK(static_cast<double>(rows.size()) > length / scene.spacing - 2);
        }
        // Thin rows carry their own thickness, so the seeded area is the part's.
        CHECK(std::abs(area - phys::partArea(part)) < 0.1 * phys::partArea(part));
    }
    CHECK(checked == 2);
}

TEST(mpm_scene_plates_have_mount_bands) {
    // Each plate: the held strip at its edges, then the mount band, then the free plate.
    const SceneCase s = sceneCase("dm53", "spaced_steel", 0);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    for (size_t layer = 0; layer < s.c.layers.size(); ++layer) {
        int mount = 0, all = 0;
        double mountLo = 1e9, mountHi = 0, anchoredLo = 1e9, freeHi = 0;
        for (size_t i = static_cast<size_t>(scene.projectileCount); i < scene.particles.size(); ++i) {
            const MpmSeed& q = scene.particles[i];
            if (q.layer != layer) continue;
            ++all;
            const double y = std::abs(static_cast<double>(q.y));
            CHECK(!((q.flags & kMpmMount) && (q.flags & kMpmAnchored)));
            if (q.flags & kMpmAnchored) anchoredLo = std::min(anchoredLo, y);
            else if (q.flags & kMpmMount) {
                ++mount;
                mountLo = std::min(mountLo, y);
                mountHi = std::max(mountHi, y);
            } else freeHi = std::max(freeHi, y);
        }
        CHECK(mount > 0 && mount < all / 3);
        CHECK(freeHi < mountLo && mountHi < anchoredLo);
        CHECK(anchoredLo - mountLo > kMpmMountCells * scene.dx - scene.spacing);
    }
}

TEST(mpm_scene_mounts_are_rigid_by_default) {
    const MpmScene scene = build(sceneCase("dm53", "spaced_steel", 0));
    CHECK(!scene.softMounts);
    CHECK(scene.mountOmega == 0);
    int anchored = 0;
    for (const MpmSeed& q : scene.particles) {
        CHECK(!(q.flags & kMpmTethered));
        if (q.flags & kMpmAnchored) ++anchored;
    }
    CHECK(anchored > 0);
}

TEST(mpm_scene_soft_mounts_tether_the_edge_strips) {
    // The strips along each plate's edges, held rigidly by default, become tethered particles that
    // never fail (the mount band's rule); the same particles are seeded either way.
    const SceneCase s = sceneCase("dm53", "spaced_steel", 0);
    const MpmScene rigid = build(s);
    const MpmScene soft = buildMpmScene(sceneDb(), s.c, s.rod, s.shape, s.out, MpmQuality::Medium, true);
    CHECK(soft.supported && soft.softMounts);
    CHECK(soft.mountOmega > 0 && soft.mountDamping > 0 && soft.mountDamping < 1);
    CHECK(soft.particles.size() == rigid.particles.size());
    int rigidHeld = 0, held = 0, tethered = 0;
    for (size_t i = 0; i < soft.particles.size(); ++i) {
        const uint32_t was = rigid.particles[i].flags, now = soft.particles[i].flags;
        if (was & kMpmAnchored) ++rigidHeld;
        if (now & kMpmAnchored) ++held;
        if (now & kMpmTethered) {
            ++tethered;
            CHECK(now & kMpmMount);
            CHECK(!(now & kMpmAnchored));
            CHECK(was & kMpmAnchored);
        }
    }
    CHECK(tethered > 0);
    CHECK(held + tethered == rigidHeld);
}

TEST(mpm_tether_acceleration_is_a_damped_spring) {
    const double w = 2e5, z = 0.4;
    CHECK_NEAR(mpmTetherAcceleration(w, z, 1e-3, 0), -w * w * 1e-3, 1e-6 * w * w * 1e-3);
    CHECK_NEAR(mpmTetherAcceleration(w, z, 0, 10), -2 * z * w * 10, 1e-6 * 2 * z * w * 10);
    // Released from 1 mm at rest, a semi-implicit integration crosses zero where the damped
    // oscillator d(t) = d0 e^(-z w t) (cos wd t + z w / wd sin wd t) does, then dies away.
    const double pi = 3.14159265358979323846, wd = w * std::sqrt(1 - z * z);
    const double dt = 1e-8, period = 2 * pi / wd;
    const double expected = (pi - std::atan(wd / (z * w))) / wd;
    double d = 1e-3, v = 0, firstCrossing = -1;
    for (double t = 0; t < 4 * period; t += dt) {
        v += dt * mpmTetherAcceleration(w, z, d, v);
        d += dt * v;
        if (firstCrossing < 0 && d <= 0) firstCrossing = t;
    }
    CHECK(std::abs(d) < 0.05e-3);
    CHECK_NEAR(firstCrossing, expected, 0.02 * period);
}

TEST(mpm_scene_thin_plate_has_two_rows) {
    // 6 mm mild steel in front of sand, a 122 mm shot: spacing ~6 mm, the plate still gets 2 rows.
    const SceneCase s = sceneCase("122mm_br471", "sandbag_wall", 70);
    const MpmScene scene = build(s, MpmQuality::Low);
    CHECK(scene.supported);
    const double th = 70 * 3.14159265358979323846 / 180.0;
    std::set<long> rows;
    for (const MpmSeed& q : scene.particles)
        if (!(q.flags & kMpmProjectile) && q.layer == 0)
            rows.insert(std::lround((q.x * std::cos(th) + q.y * std::sin(th)) * 1e5));
    CHECK(rows.size() >= 2u);
    // Compact shot: ogive nose and a filler cavity, so fewer particles than the full cylinder.
    const double cylinder = std::max(s.c.penetrator.length, s.c.penetrator.diameter) * s.c.penetrator.diameter;
    double cavity = 0;
    for (const phys::ShapePart& part : s.shape.parts)
        if (part.role == phys::PartRole::Filler) cavity += phys::partArea(part);
    CHECK(cavity > 0);
    CHECK(scene.projectileArea < cylinder - cavity);
    // At Low quality the cavity's edge rows cost up to a spacing each side.
    CHECK(scene.projectileArea > 0.75 * cylinder - cavity);
}

TEST(mpm_quality_levels) {
    // Finer and larger with each level; the default resolves a rod's diameter in 14 cells.
    CHECK(mpmCellsPerDiameter(MpmQuality::Low) == 8 && mpmParticleBudget(MpmQuality::Low) == 150000);
    CHECK(mpmCellsPerDiameter(MpmQuality::Medium) == 14 && mpmParticleBudget(MpmQuality::Medium) == 600000);
    CHECK(mpmCellsPerDiameter(MpmQuality::High) == 22 && mpmParticleBudget(MpmQuality::High) == 1600000);
    CHECK_EQ_STR(mpmQualityLabel(MpmQuality::High), "High");
    // Ultra and Insane are for slow motion and pre-rendering: millions of particles.
    CHECK(mpmCellsPerDiameter(MpmQuality::Ultra) == 128 && mpmParticleBudget(MpmQuality::Ultra) == 4000000);
    CHECK(mpmCellsPerDiameter(MpmQuality::Insane) == 256 && mpmParticleBudget(MpmQuality::Insane) == 12000000);
    // Ludicrous: a hundred million particles, about 14 GB of video memory; Insane and Ludicrous are
    // best pre-rendered.
    CHECK(MpmQuality::Count == static_cast<MpmQuality>(6));
    CHECK(mpmCellsPerDiameter(MpmQuality::Ludicrous) == 512 && mpmParticleBudget(MpmQuality::Ludicrous) == 100000000);
    CHECK_EQ_STR(mpmQualityLabel(MpmQuality::Ludicrous), "Ludicrous");
    CHECK(mpmParticlesPerSide(MpmQuality::Ludicrous) == 5 && mpmMaxParticlesPerSide(MpmQuality::Ludicrous) == 16);
    CHECK(mpmMaxCells(MpmQuality::Ludicrous) == 3 * kMpmMaxCells);
    CHECK(!mpmBestPrerendered(MpmQuality::Low) && !mpmBestPrerendered(MpmQuality::Medium));
    CHECK(!mpmBestPrerendered(MpmQuality::High) && !mpmBestPrerendered(MpmQuality::Ultra));
    CHECK(mpmBestPrerendered(MpmQuality::Insane) && mpmBestPrerendered(MpmQuality::Ludicrous));
    // Particles to a cell side: 2 (four a cell) up to High; Ultra and Insane start at 3 and 4 and
    // seed up to 6 and 9 where their grid is the limit (36 and 81 a cell).
    CHECK(mpmParticlesPerSide(MpmQuality::Low) == 2 && mpmParticlesPerSide(MpmQuality::Medium) == 2);
    CHECK(mpmParticlesPerSide(MpmQuality::High) == 2);
    CHECK(mpmParticlesPerSide(MpmQuality::Ultra) == 3 && mpmParticlesPerSide(MpmQuality::Insane) == 4);
    for (int i = 0; i < static_cast<int>(MpmQuality::Count); ++i) {
        const MpmQuality q = static_cast<MpmQuality>(i);
        CHECK(mpmMaxParticlesPerSide(q) >= mpmParticlesPerSide(q));
    }
    CHECK(mpmMaxParticlesPerSide(MpmQuality::High) == 2);
    CHECK(mpmMaxParticlesPerSide(MpmQuality::Ultra) == 6 && mpmMaxParticlesPerSide(MpmQuality::Insane) == 9);
    CHECK_EQ_STR(mpmQualityLabel(MpmQuality::Ultra), "Ultra");
    CHECK_EQ_STR(mpmQualityLabel(MpmQuality::Insane), "Insane");
    for (int i = 1; i < static_cast<int>(MpmQuality::Count); ++i) {
        const MpmQuality a = static_cast<MpmQuality>(i - 1), b = static_cast<MpmQuality>(i);
        CHECK(mpmCellsPerDiameter(b) > mpmCellsPerDiameter(a));
        CHECK(mpmParticleBudget(b) > mpmParticleBudget(a));
        CHECK(mpmParticlesPerSide(b) >= mpmParticlesPerSide(a));
    }
    // The finest levels get a wider grid: half as wide again for Ultra, twice for Insane.
    CHECK(mpmMaxCells(MpmQuality::High) == kMpmMaxCells);
    CHECK(mpmMaxCells(MpmQuality::Ultra) == 3 * kMpmMaxCells / 2 && mpmMaxCells(MpmQuality::Insane) == 2 * kMpmMaxCells);
    // A small case resolves at the level's cells per diameter (up to High; the finest levels ask for
    // more than their grid holds, so the grid sets the cell) and every level is finer than the last.
    // (Ludicrous seeds tens of millions of particles: its scenes are not built in the suite.)
    double last = 1e9;
    for (int i = 0; i <= static_cast<int>(MpmQuality::Insane); ++i) {
        const MpmQuality q = static_cast<MpmQuality>(i);
        const SceneCase s = sceneCase("generic_30mm_apfsds", "rha_100", 0);
        const MpmScene scene = build(s, q);
        const double ideal = s.c.penetrator.diameter / mpmCellsPerDiameter(q);
        if (q <= MpmQuality::High) CHECK_NEAR(scene.dx, ideal, 1e-12);
        else CHECK(scene.dx >= ideal * (1 - 1e-12));
        CHECK(scene.dx < last);
        last = scene.dx;
        CHECK(scene.particlesPerSide >= mpmParticlesPerSide(q) && scene.particlesPerSide <= mpmMaxParticlesPerSide(q));
        CHECK_NEAR(scene.spacing, scene.dx / scene.particlesPerSide, 1e-15);
    }
}

TEST(mpm_video_memory_estimate) {
    // 144 bytes a particle, 24 a grid node (cell mass and momentum, node velocity) and the
    // measure partials, a vec4 x 6 per 256 particles.
    CHECK(mpmVideoBytes(0, 0, 0) == 0);
    const size_t n = 1000000, nodes = size_t(1500) * 800;
    const size_t expect = n * 144 + nodes * 24 + ((n + 255) / 256) * 96;
    CHECK(mpmVideoBytes(n, 1500, 800) == expect);
    // Twelve million particles are well over a gigabyte and a half.
    CHECK(mpmVideoBytes(12000000, 3000, 2000) > (size_t(3) << 29));
}

TEST(mpm_fit_problem_names_the_limit) {
    const size_t GiB = size_t(1) << 30;
    // Fits, or nothing is known of the card: no problem.
    CHECK(mpmFitProblem(1000000, 1500, 800, 8 * GiB, 0).empty());
    CHECK(mpmFitProblem(100000000, 6144, 6144, 0, 0).empty());
    // A hundred million particles are 14.4 GB: too much for 7 GB free, enough for 20 GB.
    const std::string small = mpmFitProblem(100000000, 6144, 6144, 7 * GiB, 0);
    CHECK(small.find("video memory") != std::string::npos);
    CHECK(mpmFitProblem(100000000, 6144, 6144, 20 * GiB, 0).empty());
    // Only 85 % of what is free may be taken.
    const size_t need = mpmVideoBytes(10000000, 2000, 2000);
    CHECK(!mpmFitProblem(10000000, 2000, 2000, static_cast<size_t>(need / 0.90), 0).empty());
    CHECK(mpmFitProblem(10000000, 2000, 2000, static_cast<size_t>(need / 0.84), 0).empty());
    // The particles are one buffer: more than the card allows in a single storage block is refused
    // even with memory to spare.
    const std::string block = mpmFitProblem(20000000, 3000, 3000, 64 * GiB, 2 * GiB);
    CHECK(block.find("buffer") != std::string::npos);
    CHECK(mpmFitProblem(10000000, 3000, 3000, 64 * GiB, 2 * GiB).empty());  // 1.44 GB of particles
    // The grid is a buffer too: 16 bytes a node.
    CHECK(!mpmFitProblem(1000, 6144, 6144, 64 * GiB, size_t(512) << 20).empty());
}

TEST(mpm_scene_respects_particle_budget) {
    for (int i = 0; i <= static_cast<int>(MpmQuality::Insane); ++i) {
        const MpmQuality q = static_cast<MpmQuality>(i);
        const SceneCase s = sceneCase("m829", "rha_600", 0);
        const MpmScene scene = build(s, q);
        CHECK(scene.supported);
        CHECK(static_cast<double>(scene.particles.size()) < 1.1 * mpmParticleBudget(q));
        // A large case fills the budget, whatever the level.
        CHECK(static_cast<double>(scene.particles.size()) > 0.5 * mpmParticleBudget(q));
        CHECK(scene.nx <= mpmMaxCells(q) && scene.ny <= mpmMaxCells(q));
    }
}

TEST(mpm_scene_ultra_fills_cells_with_particles) {
    // A long rod's grid is mostly empty and sets the cell, so the cells hold more than the base nine
    // particles: millions in all, close to the budget, each at its share of the rod's section.
    const SceneCase s = sceneCase("m829", "rha_100", 0);
    const MpmScene scene = build(s, MpmQuality::Ultra);
    CHECK(scene.supported);
    CHECK(scene.particlesPerSide > 3 && scene.particlesPerSide <= mpmMaxParticlesPerSide(MpmQuality::Ultra));
    CHECK_NEAR(scene.spacing, scene.dx / scene.particlesPerSide, 1e-15);
    CHECK(scene.particles.size() > 2000000);
    CHECK(static_cast<double>(scene.particles.size()) < 1.1 * mpmParticleBudget(MpmQuality::Ultra));
    const double expected = s.c.penetrator.length * s.c.penetrator.diameter / (scene.spacing * scene.spacing);
    CHECK(std::abs(scene.bodyCount - expected) < 0.1 * expected);
}

TEST(mpm_scene_cuts_semi_infinite_block) {
    // A rifle AP core that stops 11 mm into 600 mm of RHA: the block is simulated only a little
    // past the round's reach, its cut face held, so the grid resolves the 6 mm core.
    const SceneCase s = sceneCase("m2_ap_30", "rha_600", 0);
    CHECK(!s.out.result.perforated);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    CHECK(scene.simThickness.size() == 1);
    if (scene.simThickness.size() != 1) return;
    CHECK(scene.simThickness[0] < 0.1 && scene.simThickness[0] > s.out.result.depth + 2 * s.c.penetrator.diameter);
    CHECK_NEAR(scene.dx, s.c.penetrator.diameter / mpmCellsPerDiameter(MpmQuality::Medium), 1e-12);
    double deepest = 0;
    int held = 0;
    for (const MpmSeed& q : scene.particles) {
        if (q.flags & kMpmProjectile) continue;
        deepest = std::max(deepest, static_cast<double>(q.x));
        if ((q.flags & kMpmAnchored) && q.x > scene.simThickness[0] - 2.5 * scene.dx && std::abs(q.y) < 0.5 * scene.plateHalfExtent) ++held;
    }
    CHECK(deepest < scene.simThickness[0]);
    CHECK(held > 10);  // the cut face is held: the block goes on behind it
    // A round that gets through keeps the whole stack.
    const SceneCase t = sceneCase("3bm42", "rha_100", 0);
    CHECK(t.out.result.perforated);
    const MpmScene through = build(t);
    CHECK(through.simThickness.size() == 1 && through.simThickness[0] == 0.1);
}

// ---------------------------------------------------------------- reactive materials (ERA)

TEST(mpm_products_pressure_and_work) {
    phys::MpmMaterial e;
    e.density = 1500;
    e.shear = 0.5e9;
    e.bulk = 8e9;
    e.heat = 1300;
    e.detonationSpeed = 7000;
    e.energy = 4e6;
    e.productsGamma = 3;
    CHECK(phys::mpmReactive(e));
    CHECK(!phys::mpmReactive(phys::MpmMaterial{}));
    CHECK_NEAR(phys::mpmProductsPressure(e, 1, 1), 2 * 1500 * 4e6, 1);
    CHECK_NEAR(phys::mpmProductsPressure(e, 0.5, 1), 6e9, 1);
    CHECK_NEAR(phys::mpmProductsPressure(e, 0, 1), 0, 0);
    CHECK_NEAR(phys::mpmProductsPressure(e, 1, 2), 12e9 / 8, 1);
    CHECK_NEAR(phys::mpmProductsTemperature(e, 1, 1), phys::kRoomTemperature + 4e6 / 1300, 1e-6);
    CHECK_NEAR(phys::mpmProductsTemperature(e, 1, 16), phys::kRoomTemperature + 4e6 / 1300 / 2, 1e-6);
    CHECK_NEAR(phys::mpmProductsTemperature(e, 0, 2), phys::kRoomTemperature, 0);

    // Expanding the fully reacted products from J = 1 to 1000 does work rho0 Q per unit volume.
    double work = 0;
    const int n = 20000;
    const double top = std::log(1000.0);
    for (int i = 0; i < n; ++i) {
        const double s0 = top * i / n, s1 = top * (i + 1) / n;
        const double j0 = std::exp(s0), j1 = std::exp(s1);
        work += 0.5 * (phys::mpmProductsPressure(e, 1, j0) + phys::mpmProductsPressure(e, 1, j1)) * (j1 - j0);
    }
    CHECK_NEAR(work / (e.density * e.energy), 1.0, 1e-3);

    // Time-step speed: the products' initial sound speed beats D and the elastic speed here.
    CHECK_NEAR(phys::mpmSignalSpeed(e), 1.5 * std::sqrt(3 * 2 * 4e6), 1e-6);
    const phys::MpmMaterial steel;
    CHECK_NEAR(phys::mpmSignalSpeed(steel), phys::mpmSoundSpeed(steel), 0);
}

TEST(mpm_reactive_catalogue) {
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    const std::string dir = test::dataDir();
    CHECK(db.load(dir + "/armour_materials.json", dir + "/projectiles.json", report, fatal));
    const int ex = db.findMaterial("sheet_explosive");
    CHECK(ex >= 0);
    const ArmourMaterialEntry& e = db.materials()[static_cast<size_t>(ex)];
    CHECK(e.material.reactive);
    CHECK(phys::mpmReactive(e.mpm));
    CHECK_NEAR(e.mpm.detonationSpeed, 7000, 1e-6);
    CHECK_NEAR(e.mpm.energy, 4e6, 1e-3);
    CHECK_NEAR(e.mpm.productsGamma, 3, 0);
    CHECK_NEAR(e.mpm.initiation, 2e9, 1);
    for (const ArmourMaterialEntry& m : db.materials())
        if (&m != &e) CHECK(!m.material.reactive && !phys::mpmReactive(m.mpm));
    bool found = false;
    for (const ArmourPreset& p : db.presets())
        if (p.id == "reactive_sandwich") {
            found = true;
            CHECK_NEAR(p.obliquityDeg, 60, 0);
            CHECK(p.layers.size() == 4u);
            CHECK(p.layers[1].material == ex);
            CHECK_NEAR(p.layers[3].gap, 0.150, 1e-9);
        }
    CHECK(found);

    const char* projectiles = R"({"schema":1,"projectiles":[]})";
    const char* materials = R"({"schema":1,"materials":[
      {"id":"ok","name":"o","group":"other","density_kgm3":1500,"hardness_bhn":0,"resistance_gpa":0.2,
       "ke_efficiency":0.2,"reactive":true,"mpm":{"detonation_kms":6},"confidence":"low","sources":["s"]},
      {"id":"nodet","name":"n","group":"other","density_kgm3":1500,"hardness_bhn":0,"resistance_gpa":0.2,
       "ke_efficiency":0.2,"reactive":true,"confidence":"low","sources":["s"]},
      {"id":"noflag","name":"f","group":"other","density_kgm3":1500,"hardness_bhn":0,"resistance_gpa":0.2,
       "ke_efficiency":0.2,"mpm":{"detonation_kms":6},"confidence":"low","sources":["s"]},
      {"id":"gamma","name":"g","group":"other","density_kgm3":1500,"hardness_bhn":0,"resistance_gpa":0.2,
       "ke_efficiency":0.2,"reactive":true,"mpm":{"detonation_kms":6,"products_gamma":9},"confidence":"low",
       "sources":["s"]}
    ]})";
    ProjectileDb bad;
    LoadReport r2;
    CHECK(bad.loadText(materials, projectiles, r2, fatal));
    CHECK(bad.materials().size() == 1u);
    CHECK(r2.skipped == 3);
    CHECK_NEAR(bad.materials()[0].mpm.detonationSpeed, 6000, 1e-6);
    CHECK_NEAR(bad.materials()[0].mpm.energy, 4e6, 1e-3);  // default
    CHECK(r2.messages.size() == 3u);
    CHECK(r2.messages[0].find("detonation_kms") != std::string::npos);
    CHECK(r2.messages[1].find("reactive") != std::string::npos);
    CHECK(r2.messages[2].find("mpm.products_gamma") != std::string::npos);
}

TEST(mpm_reactive_sandwich_case) {
    const SceneCase s = sceneCase("m829", "reactive_sandwich");
    bool warned = false;
    for (const std::string& w : s.out.result.warnings) warned |= w.find("Explosive layers") != std::string::npos;
    CHECK(warned);
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    CHECK(scene.materials.size() == 6u);  // rod, three plates, the dart's fittings
    CHECK(phys::mpmReactive(scene.materials[2]));
    CHECK(!phys::mpmReactive(scene.materials[5]));
    CHECK(!phys::mpmReactive(scene.materials[1]));
    int explosive = 0;
    for (const MpmSeed& p : scene.particles) explosive += p.material == 2;
    CHECK(explosive > 100);
}

TEST(mpm_scene_block_layer_has_gaps) {
    // ERA tiles at the preset's 60 degrees: seeded along the face with a 10 mm gap between them,
    // and free (no anchored mounting); the base plate behind is still held at its edges.
    const SceneCase s = sceneCase("m829", "reactive_sandwich");
    const MpmScene scene = build(s);
    CHECK(scene.supported);
    const double H = scene.plateHalfExtent;
    CHECK(H > 0.14);
    const double theta = std::abs(s.c.obliquityDeg) * 3.14159265358979323846 / 180.0;
    const double sinT = std::sin(theta), cosT = std::cos(theta), tanT = std::tan(theta);
    const auto& layers = s.out.result.layers;
    int beyond = 0, inGap = 0, anchoredTiles = 0, anchoredBase = 0, tiles = 0;
    for (size_t i = static_cast<size_t>(scene.projectileCount); i < scene.particles.size(); ++i) {
        const MpmSeed& q = scene.particles[i];
        const double thick = s.c.layers[q.layer].thickness;
        // Along the face from where the shot line crosses the layer's mid-plane.
        const double sigma = -(q.x - layers[q.layer].front) * sinT + q.y * cosT + 0.5 * thick * tanT;
        if (q.layer < 3) {
            ++tiles;
            if (std::abs(sigma) > 0.125 + 1e-4 && std::abs(sigma) < 0.135 - 1e-4) ++inGap;
            if (std::abs(sigma) > 0.135) ++beyond;
            if (q.flags & kMpmAnchored) ++anchoredTiles;
        } else if (q.flags & kMpmAnchored) {
            ++anchoredBase;
        }
    }
    CHECK(tiles > 0);
    CHECK(inGap == 0);
    CHECK(beyond > 0);
    CHECK(anchoredTiles == 0);
    CHECK(anchoredBase > 0);

    // Shot line down a gap: the tiles are still seeded at their real thickness either side.
    SceneCase g = s;
    for (int i = 0; i < 3; ++i) g.c.layers[static_cast<size_t>(i)].offset = 0.13;
    g.out = evaluateArmour(sceneDb(), g.c);
    const MpmScene gs = build(g);
    CHECK(gs.supported);
    int near = 0;
    for (size_t i = static_cast<size_t>(gs.projectileCount); i < gs.particles.size(); ++i) {
        const MpmSeed& q = gs.particles[i];
        if (q.layer >= 3) continue;
        const double thick = g.c.layers[q.layer].thickness;
        const double sigma = -(q.x - g.out.result.layers[q.layer].front) * sinT + q.y * cosT + 0.5 * thick * tanT;
        CHECK(std::abs(sigma) > 0.005 - 1e-4);
        if (std::abs(sigma) < 0.02) ++near;
    }
    CHECK(near > 0);
}
