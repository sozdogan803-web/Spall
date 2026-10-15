// Phase 4 material models of the deformation simulation: ceramics, elastomers, fibre composites.
#include "test.h"

#include "data/armour_blocks.h"
#include "data/projectile_db.h"
#include "physics/mpm_material.h"
#include "sim/armour_run.h"
#include "sim/mpm_scene.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const char* kNoProjectiles = R"({"schema":1,"projectiles":[]})";

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

// A generic rod case against the given layers.
ArmourCase rodCase(std::vector<PresetLayer> layers, double obliquity) {
    const ProjectileDb& shipped = shippedDb();
    const ProjectileEntry& e = shipped.projectiles()[static_cast<size_t>(shipped.findProjectile("generic_30mm_apfsds"))];
    ArmourCase c;
    c.penetrator = e.penetrator;
    c.velocity = e.velocity;
    c.obliquityDeg = obliquity;
    c.layers = std::move(layers);
    return c;
}

MpmScene buildScene(const ProjectileDb& db, const ArmourCase& c) {
    return buildMpmScene(db, c, RodMaterial::Tungsten, phys::shapeForCase(nullptr, c.penetrator), evaluateArmour(db, c),
                         MpmQuality::Low);
}

phys::MpmMaterial ceramicSet() {
    phys::MpmMaterial m;
    m.model = phys::MpmModel::Ceramic;
    m.density = 3700;
    m.shear = 152e9;
    m.bulk = 231e9;
    m.hel = 6e9;
    return m;
}

}  // namespace

TEST(mpm_model_ids_and_hel_state) {
    CHECK_EQ_STR(phys::mpmModelId(phys::MpmModel::Metal), "metal");
    CHECK_EQ_STR(phys::mpmModelId(phys::MpmModel::Ceramic), "ceramic");
    CHECK_EQ_STR(phys::mpmModelId(phys::MpmModel::Elastomer), "elastomer");
    CHECK_EQ_STR(phys::mpmModelId(phys::MpmModel::Composite), "composite");

    const phys::MpmMaterial m = ceramicSet();
    const phys::MpmHelState h = phys::mpmHelState(m);
    CHECK_NEAR(h.strain, 6e9 / (231e9 + 4.0 / 3.0 * 152e9), 1e-12);
    CHECK_NEAR(h.pressure, 231e9 * h.strain, 1);
    CHECK_NEAR(h.strength, 2 * 152e9 * h.strain, 1);
    // Uniaxial strain: the axial stress at the limit is P_HEL + 2/3 sigma_HEL = HEL.
    CHECK_NEAR(h.pressure + 2.0 / 3.0 * h.strength, m.hel, 1);
}

TEST(mpm_signal_speed_and_viscous_step) {
    phys::MpmMaterial c;
    c.model = phys::MpmModel::Composite;
    c.density = 1000;
    c.shear = 1e9;
    c.bulk = 4e9;
    c.fibreModulus = 40e9;
    CHECK_NEAR(phys::mpmSignalSpeed(c), std::sqrt((4e9 + 4.0 / 3.0 * 1e9 + 40e9) / 1000), 1e-6);
    c.model = phys::MpmModel::Metal;  // the fibre modulus only counts for composites
    CHECK_NEAR(phys::mpmSignalSpeed(c), phys::mpmSoundSpeed(c), 1e-9);

    phys::MpmMaterial e;
    e.model = phys::MpmModel::Elastomer;
    e.density = 1100;
    e.viscosity = 200;
    CHECK_NEAR(phys::mpmViscousStep(e, 1e-3), 0.25 * 1100 * 1e-6 / 200, 1e-15);
    e.viscosity = 0;
    CHECK(std::isinf(phys::mpmViscousStep(e, 1e-3)));
    CHECK(std::isinf(phys::mpmViscousStep(phys::MpmMaterial{}, 1e-3)));
}

TEST(mpm_model_parsing) {
    const char* materials = R"({"schema":1,"materials":[
      {"id":"cer","name":"c","group":"ceramic","density_kgm3":3500,"hardness_bhn":0,"resistance_gpa":5,
       "ke_efficiency":1,"confidence":"low","sources":["s"]},
      {"id":"cer_big","name":"cb","group":"ceramic","density_kgm3":3500,"hardness_bhn":0,"resistance_gpa":20,
       "ke_efficiency":1,"mpm":{"shear_gpa":100},"confidence":"low","sources":["s"]},
      {"id":"cer_metal","name":"cm","group":"ceramic","density_kgm3":3500,"hardness_bhn":0,"resistance_gpa":5,
       "ke_efficiency":1,"mpm":{"model":"metal","yield_gpa":2},"confidence":"low","sources":["s"]},
      {"id":"glassy","name":"g","group":"other","density_kgm3":2500,"hardness_bhn":0,"resistance_gpa":3,
       "ke_efficiency":1,"mpm":{"model":"ceramic","hel_gpa":6,"intact_a":0.9,"fractured_max":0.4,"tensile_gpa":0.1},
       "confidence":"low","sources":["s"]},
      {"id":"rub","name":"r","group":"other","density_kgm3":1100,"hardness_bhn":0,"resistance_gpa":0.1,
       "ke_efficiency":1,"mpm":{"model":"elastomer","viscosity_pas":300,"viscous_limit_gpa":0.05,"failure_strain":4},
       "confidence":"low","sources":["s"]},
      {"id":"lam","name":"l","group":"other","density_kgm3":970,"hardness_bhn":0,"resistance_gpa":0.5,
       "ke_efficiency":1,"mpm":{"model":"composite","fibre_modulus_gpa":40,"delam_shear_gpa":0.006,"yield_gpa":0.03},
       "confidence":"low","sources":["s"]},
      {"id":"bad_model","name":"x","group":"other","density_kgm3":1000,"hardness_bhn":0,"resistance_gpa":0.1,
       "ke_efficiency":1,"mpm":{"model":"jelly"},"confidence":"low","sources":["s"]},
      {"id":"bad_key","name":"x","group":"other","density_kgm3":1000,"hardness_bhn":0,"resistance_gpa":0.1,
       "ke_efficiency":1,"mpm":{"model":"elastomer","hel_gpa":5},"confidence":"low","sources":["s"]},
      {"id":"bad_yield","name":"x","group":"ceramic","density_kgm3":3500,"hardness_bhn":0,"resistance_gpa":5,
       "ke_efficiency":1,"mpm":{"yield_gpa":2},"confidence":"low","sources":["s"]},
      {"id":"no_hel","name":"x","group":"other","density_kgm3":2500,"hardness_bhn":0,"resistance_gpa":3,
       "ke_efficiency":1,"mpm":{"model":"ceramic"},"confidence":"low","sources":["s"]},
      {"id":"no_fibre","name":"x","group":"other","density_kgm3":970,"hardness_bhn":0,"resistance_gpa":0.5,
       "ke_efficiency":1,"mpm":{"model":"composite"},"confidence":"low","sources":["s"]},
      {"id":"hot_ceramic","name":"x","group":"other","density_kgm3":1500,"hardness_bhn":0,"resistance_gpa":0.2,
       "ke_efficiency":1,"reactive":true,"mpm":{"model":"ceramic","hel_gpa":3},"confidence":"low","sources":["s"]},
      {"id":"det_rubber","name":"x","group":"other","density_kgm3":1500,"hardness_bhn":0,"resistance_gpa":0.2,
       "ke_efficiency":1,"reactive":true,"mpm":{"model":"elastomer","detonation_kms":6},"confidence":"low",
       "sources":["s"]},
      {"id":"bad_range","name":"x","group":"other","density_kgm3":970,"hardness_bhn":0,"resistance_gpa":0.5,
       "ke_efficiency":1,"mpm":{"model":"composite","fibre_modulus_gpa":40,"fibre_failure_strain":2},
       "confidence":"low","sources":["s"]}
    ]})";
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    CHECK(db.loadText(materials, kNoProjectiles, report, fatal));
    CHECK(db.materials().size() == 6u);
    CHECK(report.skipped == 8);

    // Ceramic-group fallback: an alumina-like JH-2 set with HEL = 2 x the resistance.
    const phys::MpmMaterial& cer = db.materials()[0].mpm;
    CHECK(cer.model == phys::MpmModel::Ceramic);
    CHECK_NEAR(cer.hel, 10e9, 1);
    CHECK_NEAR(cer.intactA, 0.93, 1e-12);
    // A block without a model keeps the fallback's; the HEL is clamped to 25 GPa.
    const phys::MpmMaterial& big = db.materials()[1].mpm;
    CHECK(big.model == phys::MpmModel::Ceramic);
    CHECK_NEAR(big.hel, 25e9, 1);
    CHECK_NEAR(big.shear, 100e9, 1);
    // A ceramic-group material may still name the metal model.
    const phys::MpmMaterial& cm = db.materials()[2].mpm;
    CHECK(cm.model == phys::MpmModel::Metal);
    CHECK_NEAR(cm.yield, 2e9, 1);

    const phys::MpmMaterial& g = db.materials()[3].mpm;
    CHECK(g.model == phys::MpmModel::Ceramic);
    CHECK_NEAR(g.hel, 6e9, 1);
    CHECK_NEAR(g.intactA, 0.9, 1e-12);
    CHECK_NEAR(g.fracturedMax, 0.4, 1e-12);
    CHECK_NEAR(g.tensile, 0.1e9, 1);
    CHECK_NEAR(g.intactN, 0.6, 1e-12);  // default

    const phys::MpmMaterial& r = db.materials()[4].mpm;
    CHECK(r.model == phys::MpmModel::Elastomer);
    CHECK_NEAR(r.viscosity, 300, 1e-9);
    CHECK_NEAR(r.viscousLimit, 0.05e9, 1);
    CHECK_NEAR(r.failureStrain, 4, 1e-12);

    const phys::MpmMaterial& l = db.materials()[5].mpm;
    CHECK(l.model == phys::MpmModel::Composite);
    CHECK_NEAR(l.fibreModulus, 40e9, 1);
    CHECK_NEAR(l.delamShear, 0.006e9, 1);
    CHECK_NEAR(l.yield, 0.03e9, 1);
    CHECK_NEAR(l.fibreFailureStrain, 0.03, 1e-12);  // default

    CHECK(report.messages.size() == 8u);
    auto has = [&](size_t i, const char* what) {
        return i < report.messages.size() && report.messages[i].find(what) != std::string::npos;
    };
    CHECK(has(0, "mpm.model") && has(0, "jelly"));
    CHECK(has(1, "mpm.hel_gpa") && has(1, "elastomer"));
    CHECK(has(2, "mpm.yield_gpa") && has(2, "ceramic"));
    CHECK(has(3, "mpm.hel_gpa") && has(3, "required"));
    CHECK(has(4, "mpm.fibre_modulus_gpa") && has(4, "required"));
    CHECK(has(5, "mpm.model") && has(5, "metal"));
    CHECK(has(6, "mpm.detonation_kms") && has(6, "elastomer"));
    CHECK(has(7, "mpm.fibre_failure_strain"));
}

TEST(mpm_shipped_models) {
    const ProjectileDb& db = shippedDb();
    for (const ArmourMaterialEntry& e : db.materials()) {
        const bool ceramic = e.id == "alumina" || e.id == "sic" || e.id == "b4c" || e.id == "glass";
        CHECK((e.mpm.model == phys::MpmModel::Ceramic) == ceramic);
        if (!ceramic) continue;
        CHECK(e.material.ceramic);
        const phys::MpmHelState h = phys::mpmHelState(e.mpm);
        CHECK(h.strain > 0.005 && h.strain < 0.1);
        CHECK(h.strength > 1e9 && h.strength < 20e9);
        CHECK(e.mpm.tensile > 0 && e.mpm.tensile < h.pressure);
        CHECK(e.mpm.fracturedB < e.mpm.intactA);
    }
}

TEST(mpm_pack_material) {
    float out[kMpmMaterialFloats];
    const phys::MpmMaterial metal = phys::mpmTungstenAlloy();
    mpmPackMaterial(metal, out);
    CHECK_NEAR(out[0], metal.density, 1e-3);
    CHECK_NEAR(out[3], metal.yield, metal.yield * 1e-6);
    CHECK_NEAR(out[11], phys::mpmSoundSpeed(metal), 1e-2);
    CHECK_NEAR(out[15], metal.initiation, metal.initiation * 1e-6);
    CHECK(out[16] == 0.0f);
    for (int i = 17; i < kMpmMaterialFloats; ++i) CHECK(out[i] == 0.0f);

    const phys::MpmMaterial c = ceramicSet();
    mpmPackMaterial(c, out);
    const phys::MpmHelState h = phys::mpmHelState(c);
    CHECK(out[16] == 1.0f);
    CHECK_NEAR(out[17], h.strength, h.strength * 1e-6);
    CHECK_NEAR(out[18], h.pressure, h.pressure * 1e-6);
    CHECK_NEAR(out[19], c.tensile, c.tensile * 1e-6);
    CHECK_NEAR(out[20], c.intactA, 1e-6);
    CHECK_NEAR(out[23], c.fracturedM, 1e-6);
    CHECK_NEAR(out[24], c.fracturedMax * h.strength, h.strength * 1e-6);
    CHECK_NEAR(out[25], c.fractureD1, 1e-9);
    CHECK_NEAR(out[26], c.fractureD2, 1e-6);

    // A brittle metal: the crack strength, 1 / Weibull modulus and the factor that makes the
    // flaw spread's mean one.
    phys::MpmMaterial b = phys::mpmPenetratorSteel();
    b.brittleStress = 2e9;
    b.flawModulus = 10;
    mpmPackMaterial(b, out);
    CHECK(out[16] == 0.0f);
    CHECK_NEAR(out[20], 2e9, 2e3);
    CHECK_NEAR(out[21], 0.1, 1e-7);
    CHECK_NEAR(out[22], 1.0 / std::tgamma(1.1), 1e-6);

    phys::MpmMaterial e;
    e.model = phys::MpmModel::Elastomer;
    e.viscosity = 300;
    e.viscousLimit = 5e7;
    mpmPackMaterial(e, out);
    CHECK(out[16] == 2.0f);
    CHECK_NEAR(out[20], 300, 1e-4);
    CHECK_NEAR(out[21], 5e7, 1);

    phys::MpmMaterial l;
    l.model = phys::MpmModel::Composite;
    l.fibreModulus = 40e9;
    l.fibreFailureStrain = 0.035;
    l.delamTension = 5e6;
    l.delamShear = 6e6;
    mpmPackMaterial(l, out);
    CHECK(out[16] == 3.0f);
    CHECK_NEAR(out[20], 40e9, 40e3);
    CHECK_NEAR(out[21], 0.035, 1e-7);
    CHECK_NEAR(out[22], 5e6, 1);
    CHECK_NEAR(out[23], 6e6, 1);
}

TEST(mpm_scene_face_tangent_counts_and_viscous_step) {
    const ProjectileDb& db = shippedDb();
    const int rha = db.findMaterial("rha"), alumina = db.findMaterial("alumina");
    CHECK(rha >= 0 && alumina >= 0);
    for (double obliquity : {0.0, 60.0}) {
        const ArmourCase c = rodCase({{alumina, 0.03, 0}, {rha, 0.02, 0}}, obliquity);
        const MpmScene s = buildScene(db, c);
        CHECK(s.supported);
        const double t = obliquity * 3.14159265358979323846 / 180;
        CHECK_NEAR(s.faceTangent[0], -std::sin(t), 1e-12);
        CHECK_NEAR(s.faceTangent[1], std::cos(t), 1e-12);
        int free = 0;
        for (const MpmSeed& p : s.particles) free += p.material == 1 && !(p.flags & kMpmAnchored);
        CHECK(free > 100);
        CHECK(s.ceramicCount == free);
        CHECK(s.compositeCount == 0);
        CHECK(s.reactiveCount == 0);
    }

    // A strongly viscous elastomer sets the step below the elastic limit.
    const char* materials = R"({"schema":1,"materials":[
      {"id":"st","name":"s","group":"steel","density_kgm3":7850,"hardness_bhn":300,"ke_efficiency":1,
       "confidence":"low","sources":["s"]},
      {"id":"goo","name":"g","group":"other","density_kgm3":1100,"hardness_bhn":0,"resistance_gpa":0.1,
       "ke_efficiency":1,"mpm":{"model":"elastomer","shear_gpa":0.002,"bulk_gpa":2.5,"viscosity_pas":100000},
       "confidence":"low","sources":["s"]},
      {"id":"lam","name":"l","group":"other","density_kgm3":970,"hardness_bhn":0,"resistance_gpa":0.5,
       "ke_efficiency":1,"mpm":{"model":"composite","fibre_modulus_gpa":40},"confidence":"low","sources":["s"]}
    ]})";
    ProjectileDb custom;
    LoadReport report;
    std::string fatal;
    CHECK(custom.loadText(materials, kNoProjectiles, report, fatal));
    CHECK(report.skipped == 0);
    const ArmourCase c = rodCase({{0, 0.01, 0}, {1, 0.01, 0}, {2, 0.02, 0}}, 30);
    const MpmScene s = buildScene(custom, c);
    CHECK(s.supported);
    const double elastic = kMpmCfl * s.dx / (s.maxSoundSpeed + s.velocity);
    const double viscous = phys::mpmViscousStep(custom.materials()[1].mpm, s.dx);
    CHECK(viscous < elastic);
    CHECK_NEAR(s.dt, viscous, 1e-18);
    CHECK(s.compositeCount > 0);
    // The laminate's stiffness along its fibres counts in the signal speed.
    CHECK(s.maxSoundSpeed >= phys::mpmSignalSpeed(custom.materials()[2].mpm));
}

TEST(mpm_ceramic_strength) {
    const phys::MpmMaterial m = ceramicSet();
    const phys::MpmHelState h = phys::mpmHelState(m);
    // Intact at the HEL pressure, by the JH-2 form.
    CHECK_NEAR(phys::mpmCeramicStrength(m, h.pressure, 0),
               h.strength * m.intactA * std::pow((h.pressure + m.tensile) / h.pressure, m.intactN), 1);
    // Rising with pressure, intact and fractured; fractured below intact.
    double lastIntact = 0, lastFractured = 0;
    for (const double p : {0.2e9, 0.5e9, 1e9, 2e9}) {
        const double intact = phys::mpmCeramicStrength(m, p, 0), fractured = phys::mpmCeramicStrength(m, p, 1);
        CHECK(intact > lastIntact);
        CHECK(fractured > lastFractured);
        CHECK(fractured < intact);
        lastIntact = intact;
        lastFractured = fractured;
    }
    // The fractured strength is capped; D blends the two linearly.
    CHECK_NEAR(phys::mpmCeramicStrength(m, 1e12, 1), m.fracturedMax * h.strength, 1);
    const double p = 1.5e9;
    CHECK_NEAR(phys::mpmCeramicStrength(m, p, 0.25),
               0.75 * phys::mpmCeramicStrength(m, p, 0) + 0.25 * phys::mpmCeramicStrength(m, p, 1), 1);
    // No fractured strength without pressure; intact strength down to p = -T.
    CHECK_NEAR(phys::mpmCeramicStrength(m, 0, 1), 0.0, 0);
    CHECK_NEAR(phys::mpmCeramicStrength(m, -1e9, 1), 0.0, 0);
    CHECK(phys::mpmCeramicStrength(m, 0, 0) > 0);
    CHECK_NEAR(phys::mpmCeramicStrength(m, -m.tensile, 0), 0.0, 0);
    // Fracture strain: D1 ((p + T) / P_HEL)^D2, at least 1e-4.
    CHECK_NEAR(phys::mpmCeramicFractureStrain(m, h.pressure),
               m.fractureD1 * std::pow((h.pressure + m.tensile) / h.pressure, m.fractureD2), 1e-12);
    CHECK_NEAR(phys::mpmCeramicFractureStrain(m, -m.tensile), phys::kMinCeramicFracture, 0);
}

namespace {

// A ceramic point at pressure p (Kirchhoff, from the volumetric strain) with in-plane shear
// strain s: strains (s, -s, 0) + tr / 3.
phys::MpmPoint ceramicPoint(const phys::MpmMaterial& m, double p, double s) {
    phys::MpmPoint pt;
    const double tr = -p / m.bulk;
    pt.strain[0] = s + tr / 3;
    pt.strain[1] = -s + tr / 3;
    pt.strain[2] = tr / 3;
    return pt;
}

struct Stress {
    double pressure, vonMises;
};

Stress stressOf(const phys::MpmMaterial& m, const phys::MpmPoint& pt) {
    double s[3];
    phys::mpmKirchhoff(m, pt.strain, s);
    return {-(s[0] + s[1] + s[2]) / 3, phys::mpmVonMises(s)};
}

}  // namespace

TEST(mpm_ceramic_return_map) {
    const phys::MpmMaterial m = ceramicSet();
    const double p = 2e9;
    const double intact = phys::mpmCeramicStrength(m, p, 0);
    // Shear strain at the intact strength under p: sqrt(3/2) 2G |dev| = sigma, |dev| = s sqrt(2).
    const double sAt = intact / (std::sqrt(1.5) * 2 * m.shear) / std::sqrt(2.0);

    // Below the strength: elastic, no fracture.
    {
        phys::MpmPoint pt = ceramicPoint(m, p, 0.9 * sAt);
        CHECK_NEAR(phys::mpmReturnMap(m, pt), 0.0, 0);
        CHECK_NEAR(pt.fracture, 0.0, 0);
        CHECK(!pt.comminuted);
        CHECK_NEAR(stressOf(m, pt).pressure, p, 1e3);
    }
    // Just past it: returned onto the intact strength at the same pressure, and D grows by
    // the plastic strain over the fracture strain.
    {
        phys::MpmPoint pt = ceramicPoint(m, p, sAt + 2e-4);
        const double dp = phys::mpmReturnMap(m, pt);
        CHECK_NEAR(dp, std::sqrt(2.0 / 3.0) * 2e-4 * std::sqrt(2.0), 1e-9);
        CHECK_NEAR(pt.fracture, dp / phys::mpmCeramicFractureStrain(m, p), 1e-9);
        CHECK(pt.fracture > 0 && pt.fracture < 1);
        CHECK(!pt.comminuted && !pt.failed);
        const Stress st = stressOf(m, pt);
        CHECK_NEAR(st.vonMises, intact, 1e-6 * intact);
        CHECK_NEAR(st.pressure, p, 1e3);
        CHECK(pt.temperature > phys::kRoomTemperature);
    }
    // Driven on: D reaches 1 and the point is comminuted; then it holds only the fractured
    // strength under pressure and is not failed.
    {
        phys::MpmPoint pt = ceramicPoint(m, p, 10 * sAt);
        phys::mpmReturnMap(m, pt);
        CHECK_NEAR(pt.fracture, 1.0, 0);
        CHECK(pt.comminuted && !pt.failed);
        phys::MpmPoint again = ceramicPoint(m, p, 10 * sAt);
        again.fracture = pt.fracture;
        again.comminuted = true;
        phys::mpmReturnMap(m, again);
        const Stress st = stressOf(m, again);
        CHECK_NEAR(st.vonMises, phys::mpmCeramicStrength(m, p, 1), 1e-6 * intact);
        CHECK(st.vonMises > 0 && st.vonMises < 0.5 * intact);
        CHECK_NEAR(st.pressure, p, 1e3);
    }
    // Tension: intact ceramic holds a mean tension below T, and comminutes past it.
    {
        phys::MpmPoint pt = ceramicPoint(m, -0.5 * m.tensile, 0);
        phys::mpmReturnMap(m, pt);
        CHECK(!pt.comminuted);
        CHECK_NEAR(stressOf(m, pt).pressure, -0.5 * m.tensile, 1e3);
        phys::MpmPoint torn = ceramicPoint(m, -1.5 * m.tensile, 0.5 * sAt);
        phys::mpmReturnMap(m, torn);
        CHECK(torn.comminuted && !torn.failed);
        CHECK_NEAR(torn.fracture, 1.0, 0);
        const Stress st = stressOf(m, torn);
        CHECK_NEAR(st.pressure, 0.0, 1);
        CHECK_NEAR(st.vonMises, 0.0, 1);
    }
    // Fracture lowers the tension it holds: past T (1 - D) comminutes.
    {
        phys::MpmPoint pt = ceramicPoint(m, -0.5 * m.tensile, 0);
        pt.fracture = 0.6;
        phys::mpmReturnMap(m, pt);
        CHECK(pt.comminuted);
    }
    // Comminuted ceramic takes no tension at all.
    {
        phys::MpmPoint pt = ceramicPoint(m, -0.1 * m.tensile, 0.2 * sAt);
        pt.fracture = 1;
        pt.comminuted = true;
        phys::mpmReturnMap(m, pt);
        const Stress st = stressOf(m, pt);
        CHECK_NEAR(st.pressure, 0.0, 1);
        CHECK_NEAR(st.vonMises, 0.0, 1);
    }
    // Melting fails it.
    {
        phys::MpmPoint pt = ceramicPoint(m, p, 0.5 * sAt);
        pt.temperature = m.meltK;
        phys::mpmReturnMap(m, pt);
        CHECK(pt.failed);
        CHECK_NEAR(stressOf(m, pt).vonMises, 0.0, 1);
    }
}

namespace {

phys::MpmMaterial elastomerSet() {
    phys::MpmMaterial m;
    m.model = phys::MpmModel::Elastomer;
    m.density = 1070;
    m.shear = 20e6;
    m.bulk = 4.9e9;
    m.meltK = 500;
    m.heat = 1800;
    m.failureStrain = 4;
    m.spall = 0.02e9;
    m.viscosity = 400;
    m.viscousLimit = 0;
    return m;
}

}  // namespace

TEST(mpm_elastomer_stress) {
    const phys::MpmMaterial m = elastomerSet();
    double s[3], h[3];
    // Stress-free at rest.
    const double rest[3] = {0, 0, 0};
    phys::mpmElastomerKirchhoff(m, rest, s);
    for (double v : s) CHECK_NEAR(v, 0, 0);
    // Hencky at small strain.
    const double small[3] = {1e-5, -2e-5, 0.5e-5};
    phys::mpmElastomerKirchhoff(m, small, s);
    phys::mpmKirchhoff(m, small, h);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(s[i], h[i], 1e-4 * std::abs(h[i]) + 1);
    // A pure volume change carries only the pressure K ln J.
    const double lnJ = -0.03;
    const double volumetric[3] = {lnJ / 3, lnJ / 3, lnJ / 3};
    phys::mpmElastomerKirchhoff(m, volumetric, s);
    for (double v : s) CHECK_NEAR(v, m.bulk * lnJ, 1e-3);
    // Stretched threefold at constant volume: G (lambda1^2 - lambda2^2), stiffer than Hencky.
    const double stretch[3] = {std::log(3.0), -0.5 * std::log(3.0), -0.5 * std::log(3.0)};
    phys::mpmElastomerKirchhoff(m, stretch, s);
    phys::mpmKirchhoff(m, stretch, h);
    CHECK_NEAR(s[0] - s[1], m.shear * (9 - 1.0 / 3.0), 1e-3);
    CHECK_NEAR(s[1], s[2], 1e-6);
    CHECK_NEAR(s[0] + s[1] + s[2], 0, 1e-3);
    CHECK(s[0] - s[1] > 2 * (h[0] - h[1]));
}

TEST(mpm_viscous_stress) {
    phys::MpmMaterial m = elastomerSet();
    double tau[4];
    // Simple shear: tau_xy = eta J gamma-dot; the work it does is eta J gamma-dot^2.
    const double g = 1e3, J = 0.95;
    const double shear[2][2] = {{0, g}, {0, 0}};
    double power = phys::mpmViscousStress(m, shear, J, tau);
    CHECK_NEAR(tau[0], 0, 1e-9);
    CHECK_NEAR(tau[1], 0, 1e-9);
    CHECK_NEAR(tau[3], 0, 1e-9);
    CHECK_NEAR(tau[2], m.viscosity * J * g, 1e-6);
    CHECK_NEAR(power, m.viscosity * J * g * g, 1e-3);
    // A rigid rotation is free.
    const double spin[2][2] = {{0, g}, {-g, 0}};
    power = phys::mpmViscousStress(m, spin, 1, tau);
    for (double v : tau) CHECK_NEAR(v, 0, 1e-9);
    CHECK_NEAR(power, 0, 1e-9);
    // In-plane expansion: deviatoric only (no out-of-plane stretching, so tau_zz < 0).
    const double grow[2][2] = {{g, 0}, {0, g}};
    power = phys::mpmViscousStress(m, grow, 1, tau);
    CHECK_NEAR(tau[0] + tau[1] + tau[3], 0, 1e-6);
    CHECK_NEAR(tau[0], 2 * m.viscosity * g / 3, 1e-6);
    CHECK(tau[3] < 0);
    CHECK(power > 0);
    // The cap keeps the direction and limits the von Mises value.
    m.viscousLimit = 1e6;
    const double fast[2][2] = {{0, 1e6}, {0, 0}};
    power = phys::mpmViscousStress(m, fast, 1, tau);
    CHECK_NEAR(std::sqrt(3.0) * tau[2], 1e6, 1e-3);
    CHECK_NEAR(tau[0], 0, 1e-9);
    CHECK_NEAR(power, tau[2] * 1e6, 1e-3 * power);
    // Below the cap nothing changes.
    power = phys::mpmViscousStress(m, shear, 1, tau);
    CHECK_NEAR(tau[2], m.viscosity * g, 1e-6);
}

TEST(mpm_elastomer_return_map) {
    const phys::MpmMaterial m = elastomerSet();
    auto point = [](double a, double b, double c) {
        phys::MpmPoint p;
        p.strain[0] = a;
        p.strain[1] = b;
        p.strain[2] = c;
        return p;
    };
    // Large stretches stay elastic: no plasticity, nothing changes.
    phys::MpmPoint p = point(std::log(3.0), -std::log(3.0), 0);
    CHECK_NEAR(phys::mpmReturnMap(m, p), 0, 0);
    CHECK(!p.failed);
    CHECK_NEAR(p.strain[0], std::log(3.0), 1e-15);
    CHECK_NEAR(p.strain[1], -std::log(3.0), 1e-15);
    CHECK_NEAR(p.plasticStrain, 0, 0);
    CHECK_NEAR(p.temperature, phys::kRoomTemperature, 0);
    // Past the failure strain (engineering stretch 4.5 > 4) it fails and keeps no shear.
    p = point(std::log(5.5), -std::log(5.5), 0);
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    for (double e : p.strain) CHECK_NEAR(e, 0, 1e-15);
    // Tension past the spall stress fails it; compression does not.
    const double spallStrain = m.spall / m.bulk;
    p = point(0.4 * spallStrain, 0.4 * spallStrain, 0.4 * spallStrain);
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    for (double e : p.strain) CHECK_NEAR(e, 0, 1e-15);
    p = point(-0.01, -0.01, -0.01);
    phys::mpmReturnMap(m, p);
    CHECK(!p.failed);
    CHECK_NEAR(p.strain[0], -0.01, 1e-15);
    // Decomposition: hot enough, it fails and keeps only its compression.
    p = point(-0.02, 0.01, -0.01);
    p.temperature = m.meltK;
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    for (double e : p.strain) CHECK_NEAR(e, -0.02 / 3, 1e-15);
}

TEST(mpm_shipped_elastomers_and_bulging_preset) {
    const ProjectileDb& db = shippedDb();
    const int rubber = db.findMaterial("rubber"), polyurea = db.findMaterial("polyurea");
    CHECK(rubber >= 0 && polyurea >= 0);
    for (const ArmourMaterialEntry& e : db.materials()) {
        const bool stretchy = e.id == "rubber" || e.id == "polyurea";
        const bool elastomer = stretchy || e.id == "ballistic_gelatin";
        CHECK((e.mpm.model == phys::MpmModel::Elastomer) == elastomer);
        if (!elastomer) continue;
        CHECK(e.mpm.viscosity > 0);
        CHECK(e.mpm.viscousLimit > 0);
        CHECK(e.mpm.shear < 0.01 * e.mpm.bulk);
        if (stretchy) CHECK(e.mpm.failureStrain >= 3);  // the gelatin tears long before
    }
    // Polyurea is the stiffer, more viscous of the two.
    const phys::MpmMaterial& r = db.materials()[static_cast<size_t>(rubber)].mpm;
    const phys::MpmMaterial& pu = db.materials()[static_cast<size_t>(polyurea)].mpm;
    CHECK(pu.shear > r.shear && pu.viscosity > r.viscosity && pu.viscousLimit > r.viscousLimit);

    const ArmourPreset* preset = nullptr;
    for (const ArmourPreset& p : db.presets())
        if (p.id == "bulging_sandwich") preset = &p;
    CHECK(preset != nullptr);
    if (!preset) return;
    CHECK_NEAR(preset->obliquityDeg, 60, 0);
    CHECK(preset->layers.size() == 4u);
    if (preset->layers.size() != 4u) return;
    CHECK(preset->layers[1].material == rubber);
    CHECK_NEAR(preset->layers[1].thickness, 0.006, 1e-12);
    CHECK_NEAR(preset->layers[3].gap, 0.150, 1e-12);
    const MpmScene s = buildScene(db, rodCase(preset->layers, preset->obliquityDeg));
    CHECK(s.supported);
    CHECK(s.dt <= phys::mpmViscousStep(r, s.dx));
    int rubberParticles = 0;
    for (const MpmSeed& p : s.particles)
        rubberParticles += !(p.flags & kMpmProjectile) && s.materials[static_cast<size_t>(p.material)].model ==
                                                               phys::MpmModel::Elastomer;
    CHECK(rubberParticles > 100);
}

TEST(mpm_shipped_gelatin_and_witness_plate) {
    const ProjectileDb& db = shippedDb();
    const int gelatin = db.findMaterial("ballistic_gelatin"), witness = db.findMaterial("al2024");
    const int rubber = db.findMaterial("rubber"), rha = db.findMaterial("rha");
    CHECK(gelatin >= 0 && witness >= 0 && rubber >= 0 && rha >= 0);
    if (gelatin < 0 || witness < 0 || rubber < 0 || rha < 0) return;
    const phys::MpmMaterial& g = db.materials()[static_cast<size_t>(gelatin)].mpm;
    const phys::MpmMaterial& r = db.materials()[static_cast<size_t>(rubber)].mpm;
    const phys::MpmMaterial& w = db.materials()[static_cast<size_t>(witness)].mpm;
    // 10 % gelatin: about water in density and sound speed (bulk modulus), far softer than rubber
    // in shear, and it tears at a stretch of order one, not several.
    CHECK(g.model == phys::MpmModel::Elastomer);
    CHECK(g.density > 1000 && g.density < 1100);
    CHECK(g.bulk > 2.0e9 && g.bulk < 3.0e9);
    CHECK(g.shear > 0 && g.shear < 0.05 * r.shear);
    CHECK(g.failureStrain > 0.2 && g.failureStrain < 3);
    // The witness sheet is a ductile light alloy, softer than the armour steel.
    CHECK(w.model == phys::MpmModel::Metal);
    CHECK(w.density > 2600 && w.density < 2900);
    CHECK(w.yield > 0.2e9 && w.yield < db.materials()[static_cast<size_t>(rha)].mpm.yield);
    CHECK(w.failureStrain > 0.1);

    auto find = [&](const char* id) -> const ArmourPreset* {
        for (const ArmourPreset& p : db.presets())
            if (p.id == id) return &p;
        return nullptr;
    };
    // The block: one free gelatin block, and nothing clamped.
    const ArmourPreset* block = find("gelatin_block");
    CHECK(block != nullptr);
    if (block && block->layers.size() == 1u) {
        const PresetLayer& l = block->layers[0];
        CHECK(l.material == gelatin);
        CHECK(isBlockLayer(l) && l.blockCount == 1);
        CHECK(l.thickness > 0.15 && l.blockWidth >= 0.1);
        const MpmScene s = buildScene(db, rodCase(block->layers, block->obliquityDeg));
        CHECK(s.supported);
        int gel = 0, anchored = 0;
        for (const MpmSeed& p : s.particles) {
            if (p.flags & kMpmProjectile) continue;
            const bool isGel = s.materials[static_cast<size_t>(p.material)].model == phys::MpmModel::Elastomer;
            gel += isGel;
            anchored += isGel && (p.flags & kMpmAnchored);
        }
        CHECK(gel > 100);
        CHECK(anchored == 0);
        CHECK(s.dt <= phys::mpmViscousStep(g, s.dx));
    } else if (block) {
        CHECK(false);
    }
    // The witness plate: a thin full sheet behind a steel plate, held at its edges.
    const ArmourPreset* sheet = find("witness_plate");
    CHECK(sheet != nullptr);
    if (sheet && sheet->layers.size() == 2u) {
        CHECK(sheet->layers[0].material == rha);
        CHECK(sheet->layers[1].material == witness);
        CHECK(sheet->layers[1].thickness <= 0.005);
        CHECK(sheet->layers[1].gap >= 0.05);
        CHECK(!isBlockLayer(sheet->layers[1]));
        const MpmScene s = buildScene(db, rodCase(sheet->layers, sheet->obliquityDeg));
        CHECK(s.supported);
        int held = 0, plate = 0;
        for (const MpmSeed& p : s.particles) {
            if ((p.flags & kMpmProjectile) || p.material != 2) continue;
            ++plate;
            held += (p.flags & kMpmAnchored) != 0;
        }
        CHECK(plate > 20);
        CHECK(held > 0 && held < plate);
    } else if (sheet) {
        CHECK(false);
    }
    // Both: armour, a witness sheet behind it, then the block.
    const ArmourPreset* both = find("witness_gelatin");
    CHECK(both != nullptr);
    if (both && both->layers.size() == 3u) {
        CHECK(both->layers[1].material == witness);
        CHECK(both->layers[2].material == gelatin);
        CHECK(isBlockLayer(both->layers[2]) && both->layers[2].blockCount == 1);
        CHECK(both->layers[2].gap > 0);
        const MpmScene s = buildScene(db, rodCase(both->layers, both->obliquityDeg));
        CHECK(s.supported);
    } else if (both) {
        CHECK(false);
    }
}

TEST(mpm_every_shipped_preset_builds_a_scene) {
    const ProjectileDb& db = shippedDb();
    CHECK(db.presets().size() >= 23u);
    for (const ArmourPreset& p : db.presets()) {
        const MpmScene s = buildScene(db, rodCase(p.layers, p.obliquityDeg));
        if (!s.supported) std::printf("    preset %s: %s\n", p.id.c_str(), s.message.c_str());
        CHECK(s.supported);
        CHECK(s.particles.size() > 100u);
    }
}

TEST(mpm_shipped_screen_presets) {
    const ProjectileDb& db = shippedDb();
    auto find = [&](const char* id) -> const ArmourPreset* {
        for (const ArmourPreset& p : db.presets())
            if (p.id == id) return &p;
        return nullptr;
    };
    // Particles of layer i that are held at their edges / found with |along the face| in (lo, hi).
    auto count = [](const MpmScene& s, int layer) {
        int n = 0;
        for (const MpmSeed& q : s.particles) n += !(q.flags & kMpmProjectile) && q.layer == layer;
        return n;
    };
    auto inBand = [](const MpmScene& s, int layer, double lo, double hi) {
        int n = 0;
        for (const MpmSeed& q : s.particles)
            n += !(q.flags & kMpmProjectile) && q.layer == layer && std::abs(q.y) > lo && std::abs(q.y) < hi;
        return n;
    };

    // Spaced slats and bars: free pieces in front of a hull plate, the middle one on the shot line.
    struct Screen {
        const char* id;
        int count;
        double thickness, width, gap;
    };
    for (const Screen& sc : {Screen{"slat_screen", 7, 0.008, 0.03, 0.03}, Screen{"bar_screen", 5, 0.02, 0.02, 0.05}}) {
        const ArmourPreset* p = find(sc.id);
        CHECK(p != nullptr);
        if (!p || p->layers.size() != 2u) {
            CHECK(false);
            continue;
        }
        const PresetLayer& s0 = p->layers[0];
        CHECK(isBlockLayer(s0) && s0.blockCount == sc.count);
        CHECK_NEAR(s0.thickness, sc.thickness, 1e-12);
        CHECK_NEAR(s0.blockWidth, sc.width, 1e-12);
        CHECK_NEAR(s0.blockGap, sc.gap, 1e-12);
        CHECK(layerCoversShotLine(s0));
        CHECK(!isBlockLayer(p->layers[1]) && p->layers[1].gap >= 0.2);
        const MpmScene s = buildScene(db, rodCase(p->layers, p->obliquityDeg));
        CHECK(s.supported);
        CHECK(count(s, 0) > 20);
        CHECK(count(s, 1) > 100);
        // Nothing seeded in the gap beside the middle piece, and pieces further out on both sides.
        CHECK(inBand(s, 0, 0.5 * sc.width + 1e-4, 0.5 * sc.width + sc.gap - 1e-4) == 0);
        CHECK(inBand(s, 0, 0.5 * sc.width + sc.gap, 1.0) > 0);
        for (const MpmSeed& q : s.particles)
            if (!(q.flags & kMpmProjectile) && q.layer == 0) CHECK((q.flags & kMpmAnchored) == 0);
    }

    // A sloped glacis: a steep plate pair, thick along the line of sight.
    const ArmourPreset* glacis = find("sloped_glacis");
    CHECK(glacis != nullptr);
    if (glacis && glacis->layers.size() == 2u) {
        CHECK(glacis->obliquityDeg >= 60 && glacis->obliquityDeg <= phys::kMaxObliquityDeg);
        CHECK(glacis->layers[1].gap == 0);
        const double normal = glacis->layers[0].thickness + glacis->layers[1].thickness;
        CHECK(normal / std::cos(glacis->obliquityDeg * 3.14159265358979323846 / 180.0) > 2.0 * normal);
        const MpmScene s = buildScene(db, rodCase(glacis->layers, glacis->obliquityDeg));
        CHECK(s.supported);
        CHECK(count(s, 0) > 20 && count(s, 1) > 50);
    } else if (glacis) {
        CHECK(false);
    }

    // A perforated plate: strips with slots between them, the slots narrower than the strips.
    const ArmourPreset* perforated = find("perforated_plate");
    CHECK(perforated != nullptr);
    if (perforated && perforated->layers.size() == 2u) {
        const PresetLayer& l = perforated->layers[0];
        CHECK(isBlockLayer(l) && l.blockCount >= 7);
        CHECK(l.blockGap > 0 && l.blockGap < l.blockWidth);
        CHECK(layerCoversShotLine(l));
        CHECK(perforated->layers[1].gap >= 0.1);
        const MpmScene s = buildScene(db, rodCase(perforated->layers, perforated->obliquityDeg));
        CHECK(s.supported);
        CHECK(inBand(s, 0, 0.5 * l.blockWidth + 1e-4, 0.5 * l.blockWidth + l.blockGap - 1e-4) == 0);
        CHECK(inBand(s, 0, 0.5 * l.blockWidth + l.blockGap, 1.0) > 0);
    } else if (perforated) {
        CHECK(false);
    }

    // A stack of witness sheets: thin, full-width, evenly spaced behind a steel plate.
    const ArmourPreset* stack = find("witness_stack");
    CHECK(stack != nullptr);
    if (stack && stack->layers.size() >= 4u) {
        CHECK(stack->layers.size() <= static_cast<size_t>(kMaxArmourLayers));
        const int witness = db.findMaterial("al2024");
        const MpmScene s = buildScene(db, rodCase(stack->layers, stack->obliquityDeg));
        CHECK(s.supported);
        for (size_t i = 1; i < stack->layers.size(); ++i) {
            CHECK(stack->layers[i].material == witness);
            CHECK(stack->layers[i].thickness <= 0.005);
            CHECK(!isBlockLayer(stack->layers[i]) && stack->layers[i].gap >= 0.05);
            CHECK(count(s, static_cast<int>(i)) > 20);
        }
    } else if (stack) {
        CHECK(false);
    }
}

TEST(mpm_soft_material_shear_range) {
    // A shear modulus of tens of kilopascals loads; a thousandth of that is refused.
    auto text = [](const char* shear) {
        return std::string(R"({"schema":1,"materials":[
      {"id":"jelly","name":"j","group":"other","density_kgm3":1040,"hardness_bhn":0,"resistance_gpa":0.001,
       "ke_efficiency":0.1,"mpm":{"model":"elastomer","shear_gpa":)") + shear + R"(,"bulk_gpa":2.4},
       "confidence":"low","sources":["s"]}]})";
    };
    ProjectileDb db;
    LoadReport report;
    std::string fatal;
    CHECK(db.loadText(text("3e-5"), kNoProjectiles, report, fatal));
    CHECK(report.skipped == 0);
    CHECK(db.materials().size() == 1u);
    if (db.materials().size() == 1u) CHECK_NEAR(db.materials()[0].mpm.shear, 3e4, 1e-6);
    ProjectileDb tiny;
    LoadReport tinyReport;
    CHECK(tiny.loadText(text("1e-8"), kNoProjectiles, tinyReport, fatal));
    CHECK(tinyReport.skipped == 1);
}

namespace {

phys::MpmMaterial compositeSet() {
    phys::MpmMaterial m;
    m.model = phys::MpmModel::Composite;
    m.density = 970;
    m.shear = 0.7e9;
    m.bulk = 4e9;
    m.yield = 0.03e9;
    m.hardening = 0.05e9;
    m.hardeningExp = 0.5;
    m.softeningExp = 1;
    m.meltK = 420;
    m.heat = 1900;
    m.failureStrain = 1.5;
    m.spall = 0.3e9;
    m.fibreModulus = 40e9;
    m.fibreFailureStrain = 0.035;
    m.delamTension = 5e6;
    m.delamShear = 6e6;
    return m;
}

}  // namespace

TEST(mpm_fibre_stress) {
    const phys::MpmMaterial m = compositeSet();
    double tau[4];
    // At rest, no stress.
    const double rest[2] = {1, 0};
    phys::mpmFibreStress(m, rest, false, tau);
    for (double t : tau) CHECK_NEAR(t, 0, 0);
    // Stretched along x: E_f ln(stretch) in xx only.
    const double along[2] = {1.02, 0};
    phys::mpmFibreStress(m, along, false, tau);
    CHECK_NEAR(tau[0], m.fibreModulus * std::log(1.02), 1);
    CHECK_NEAR(tau[1], 0, 0);
    CHECK_NEAR(tau[2], 0, 0);
    CHECK_NEAR(tau[3], 0, 0);
    // The same stretch at 45 degrees: the stress along the fibre is unchanged.
    const double c = 1.02 / std::sqrt(2.0);
    const double diagonal[2] = {c, c};
    phys::mpmFibreStress(m, diagonal, false, tau);
    CHECK_NEAR(tau[0], 0.5 * m.fibreModulus * std::log(1.02), 1);
    CHECK_NEAR(tau[1], 0.5 * m.fibreModulus * std::log(1.02), 1);
    CHECK_NEAR(tau[2], 0.5 * m.fibreModulus * std::log(1.02), 1);
    CHECK_NEAR(tau[3], 0, 0);
    // Tension only: compressed fibres buckle, and broken ones carry nothing.
    const double shortened[2] = {0, 0.97};
    phys::mpmFibreStress(m, shortened, false, tau);
    for (double t : tau) CHECK_NEAR(t, 0, 0);
    phys::mpmFibreStress(m, along, true, tau);
    for (double t : tau) CHECK_NEAR(t, 0, 0);
    // They break above the failure strain (ln 1.03 < 0.035 < ln 1.04), whatever the direction.
    const double below[2] = {0, 1.03}, above[2] = {-1.04 / std::sqrt(2.0), 1.04 / std::sqrt(2.0)};
    CHECK(!phys::mpmFibresBreak(m, below));
    CHECK(phys::mpmFibresBreak(m, above));
    CHECK(!phys::mpmFibresBreak(m, shortened));
}

TEST(mpm_delamination) {
    const phys::MpmMaterial m = compositeSet();
    const double fx[2] = {1, 0}, fy[2] = {0, 1.01};
    auto delam = [&](double xx, double yy, double xy, double J, const double* f) {
        const double tau[4] = {xx, yy, xy, 0};
        return phys::mpmDelaminates(m, tau, J, f);
    };
    // Fibres along x: tension across the plies is yy.
    CHECK(!delam(0, 4e6, 0, 1, fx));
    CHECK(delam(0, 6e6, 0, 1, fx));
    // Tension along the fibres and compression across the plies do not delaminate.
    CHECK(!delam(1e9, 0, 0, 1, fx));
    CHECK(!delam(0, -1e9, 0, 1, fx));
    // Interlaminar shear, either sign.
    CHECK(!delam(0, 0, 5e6, 1, fx));
    CHECK(delam(0, 0, 7e6, 1, fx));
    CHECK(delam(0, 0, -7e6, 1, fx));
    // Cauchy, not Kirchhoff: an expanded particle's stress is tau / J.
    CHECK(!delam(0, 6e6, 0, 1.5, fx));
    // Fibres along y (not unit length): tension across the plies is xx.
    CHECK(delam(6e6, 0, 0, 1, fy));
    CHECK(!delam(0, 6e6, 0, 1, fy));
    // At 45 degrees, equal biaxial tension acts across the plies with no shear on them.
    const double f45[2] = {1, 1};
    CHECK(!delam(4e6, 4e6, 0, 1, f45));
    CHECK(delam(6e6, 6e6, 0, 1, f45));
    // There, xy alone is pure tension across the plies (-xy) and along them (+xy).
    CHECK(delam(0, 0, -6e6, 1, f45));
    CHECK(!delam(0, 0, 6e6, 1, f45));
}

TEST(mpm_composite_return_map) {
    const phys::MpmMaterial m = compositeSet();
    auto point = [](double a, double b, double c) {
        phys::MpmPoint p;
        p.strain[0] = a;
        p.strain[1] = b;
        p.strain[2] = c;
        return p;
    };
    // The matrix yields like the metal model, heats, and stays bonded.
    phys::MpmPoint p = point(0.05, -0.05, 0);
    phys::MpmMaterial metal = m;
    metal.model = phys::MpmModel::Metal;
    phys::MpmPoint q = p;
    const double dp = phys::mpmReturnMap(m, p);
    CHECK(dp > 0);
    CHECK_NEAR(dp, phys::mpmReturnMap(metal, q), 1e-15);
    CHECK_NEAR(p.strain[0], q.strain[0], 1e-15);
    CHECK(p.temperature > phys::kRoomTemperature);
    CHECK(!p.delaminated && !p.failed);
    // Past its failure strain it delaminates rather than fails; compressed across the plies,
    // the sheets keep their strains.
    p = point(-0.02, 0.01, -0.01);
    p.plasticStrain = 1.6;
    phys::mpmReturnMap(m, p);
    CHECK(p.delaminated && !p.failed);
    CHECK_NEAR(p.strain[0], -0.02, 1e-15);
    CHECK_NEAR(p.strain[1], 0.01, 1e-15);
    CHECK_NEAR(p.strain[2], -0.01, 1e-15);
    // Tension past the spall stress delaminates it; the plies open (no stress across them)
    // while the sheets keep their tension along the fibres and out of plane.
    const double spallStrain = m.spall / m.bulk;
    p = point(0.4 * spallStrain, 0.4 * spallStrain, 0.4 * spallStrain);
    phys::mpmReturnMap(m, p);
    CHECK(p.delaminated && !p.failed);
    CHECK_NEAR(p.strain[0], 0.4 * spallStrain, 1e-15);
    CHECK_NEAR(p.strain[2], 0.4 * spallStrain, 1e-15);
    CHECK(p.strain[1] < 0);
    const double trace = p.strain[0] + p.strain[1] + p.strain[2];
    CHECK_NEAR(2 * m.shear * (p.strain[1] - trace / 3) + m.bulk * trace, 0, 1e-3);
    CHECK(2 * m.shear * (p.strain[0] - trace / 3) + m.bulk * trace > 0);
    // Delaminated, each sheet still yields like the matrix.
    p = point(0.05, -0.05, 0);
    p.delaminated = true;
    q = point(0.05, -0.05, 0);
    CHECK_NEAR(phys::mpmReturnMap(m, p), phys::mpmReturnMap(metal, q), 1e-15);
    CHECK(p.plasticStrain > 0 && !p.failed);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(p.strain[i], q.strain[i], 1e-15);
    // Broken fibres alone leave the matrix bonded; delaminated with broken fibres it has failed.
    p = point(0.001, -0.001, 0);
    p.fibreBroken = true;
    phys::mpmReturnMap(m, p);
    CHECK(!p.delaminated && !p.failed);
    p.delaminated = true;
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    // Melted it has failed.
    p = point(-0.01, 0, 0);
    p.temperature = m.meltK;
    phys::mpmReturnMap(m, p);
    CHECK(p.failed);
    for (double e : p.strain) CHECK_NEAR(e, -0.01 / 3, 1e-15);
}

TEST(mpm_shipped_composites_and_preset) {
    const ProjectileDb& db = shippedDb();
    const int aramid = db.findMaterial("aramid"), uhmwpe = db.findMaterial("uhmwpe");
    CHECK(aramid >= 0 && uhmwpe >= 0);
    for (const ArmourMaterialEntry& e : db.materials()) {
        const bool composite = e.id == "aramid" || e.id == "uhmwpe";
        CHECK((e.mpm.model == phys::MpmModel::Composite) == composite);
        if (!composite) continue;
        // Fibres far stiffer than the matrix, breaking at a few percent; weak interlaminar strengths.
        CHECK(e.mpm.fibreModulus > 5 * e.mpm.shear);
        CHECK(e.mpm.fibreFailureStrain > 0.01 && e.mpm.fibreFailureStrain < 0.1);
        CHECK(e.mpm.delamTension < e.mpm.yield);
        CHECK(e.mpm.delamShear < e.mpm.yield);
    }
    // UHMWPE delaminates more readily than aramid.
    const phys::MpmMaterial& a = db.materials()[static_cast<size_t>(aramid)].mpm;
    const phys::MpmMaterial& u = db.materials()[static_cast<size_t>(uhmwpe)].mpm;
    CHECK(u.delamTension < a.delamTension && u.delamShear < a.delamShear);

    const ArmourPreset* preset = nullptr;
    for (const ArmourPreset& p : db.presets())
        if (p.id == "ceramic_composite") preset = &p;
    CHECK(preset != nullptr);
    if (!preset) return;
    CHECK_NEAR(preset->obliquityDeg, 0, 0);
    CHECK(preset->layers.size() == 2u);
    if (preset->layers.size() != 2u) return;
    CHECK(preset->layers[0].material == db.findMaterial("alumina"));
    CHECK_NEAR(preset->layers[0].thickness, 0.020, 1e-12);
    CHECK(preset->layers[1].material == uhmwpe);
    CHECK_NEAR(preset->layers[1].thickness, 0.025, 1e-12);
    const MpmScene s = buildScene(db, rodCase(preset->layers, preset->obliquityDeg));
    CHECK(s.supported);
    CHECK(s.ceramicCount > 100);
    CHECK(s.compositeCount > 100);
    // The step resolves the fibres' wave speed.
    CHECK(s.dt * phys::mpmSignalSpeed(u) < s.dx);
}

TEST(mpm_part_materials_known) {
    for (const char* id : {"gilding_metal", "lead", "cap_steel", "tungsten_carbide", "mild_steel", "case_steel",
                           "incendiary", "core_steel", "tungsten_alloy", "du", "light_alloy", "propellant"}) {
        const auto m = phys::mpmPartMaterial(id);
        CHECK(m.has_value());
        if (!m) continue;
        CHECK(m->density > 0);
        CHECK(m->shear > 0);
        CHECK(m->bulk > 0);
    }
    CHECK(!phys::mpmPartMaterial("nope").has_value());
    CHECK_NEAR(phys::mpmPartMaterial("core_steel")->density, phys::mpmPenetratorSteel().density, 1e-9);
    CHECK_NEAR(phys::mpmPartMaterial("tungsten_alloy")->density, phys::mpmTungstenAlloy().density, 1e-9);
    CHECK_NEAR(phys::mpmPartMaterial("du")->density, phys::mpmDepletedUranium().density, 1e-9);
    CHECK_NEAR(phys::mpmPartMaterial("light_alloy")->density, phys::mpmLightAlloy().density, 1e-9);
    CHECK_NEAR(phys::mpmPartMaterial("lead")->density, 11340, 1e-9);
    CHECK_NEAR(phys::mpmPartMaterial("tungsten_carbide")->shear, 250e9, 1);
}

TEST(mpm_filler_is_desensitised) {
    const phys::MpmMaterial f = phys::mpmFiller(1650, 6900, 4e6);
    CHECK(phys::mpmReactive(f));
    CHECK(f.initiation >= 1e29);
    CHECK_NEAR(f.productsGamma, 3, 1e-12);
    CHECK_NEAR(f.detonationSpeed, 6900, 1e-9);
    CHECK_NEAR(f.density, 1650, 1e-9);
    CHECK_NEAR(f.energy, 4e6, 1e-9);
}
