// Saving and loading armour cases, and the CSV exports of the time histories.
#include "test.h"

#include "data/projectile_db.h"
#include "sim/armour_case_io.h"
#include "sim/armour_run.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
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

SavedCase baseCase(const ProjectileDb& db) {
    SavedCase s;
    s.name = "Test case";
    s.projectile = db.findProjectile("generic_30mm_apfsds");
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(s.projectile)];
    s.c.penetrator = e.penetrator;
    s.c.velocity = e.velocity;
    s.rodMaterial = e.rodMaterial;
    s.c.obliquityDeg = 0;
    s.c.layers = {{db.findMaterial("rha"), 0.02, 0, 0, 0, 0, 0}};
    return s;
}

bool sameLayers(const std::vector<PresetLayer>& a, const std::vector<PresetLayer>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const PresetLayer &p = a[i], &q = b[i];
        if (p.material != q.material || p.blockCount != q.blockCount) return false;
        const double tol = 1e-9;
        if (std::abs(p.thickness - q.thickness) > tol || std::abs(p.gap - q.gap) > tol ||
            std::abs(p.blockWidth - q.blockWidth) > tol || std::abs(p.blockGap - q.blockGap) > tol ||
            std::abs(p.offset - q.offset) > tol)
            return false;
    }
    return true;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

std::vector<double> fields(const std::string& line) {
    std::vector<double> out;
    std::istringstream in(line);
    std::string cell;
    while (std::getline(in, cell, ',')) out.push_back(std::atof(cell.c_str()));
    return out;
}

}  // namespace

TEST(case_io_round_trips_a_catalogue_case) {
    const ProjectileDb& db = shippedDb();
    SavedCase s = baseCase(db);
    s.c.velocity = 1234;
    s.c.obliquityDeg = -25;
    s.c.layers = {{db.findMaterial("rha"), 0.0125, 0, 0, 0, 0, 0}, {db.findMaterial("al2024"), 0.0015, 0.1, 0, 0, 0, 0}};
    const std::string text = caseToJson(db, s);
    CHECK(!text.empty());
    SavedCase back;
    std::string error;
    CHECK(caseFromJson(db, text, back, error));
    CHECK(error.empty());
    CHECK_EQ_STR(back.name, "Test case");
    CHECK(back.projectile == s.projectile);
    CHECK(!back.custom);
    CHECK_NEAR(back.c.velocity, 1234, 1e-6);
    CHECK_NEAR(back.c.obliquityDeg, -25, 1e-9);
    CHECK(sameLayers(back.c.layers, s.c.layers));
}

TEST(case_io_round_trips_every_shipped_preset) {
    const ProjectileDb& db = shippedDb();
    CHECK(db.presets().size() >= 20);
    for (const ArmourPreset& p : db.presets()) {
        SavedCase s = baseCase(db);
        s.c.layers = p.layers;
        s.c.obliquityDeg = p.obliquityDeg;
        SavedCase back;
        std::string error;
        const bool ok = caseFromJson(db, caseToJson(db, s), back, error);
        if (!ok) std::fprintf(stderr, "preset %s: %s\n", p.id.c_str(), error.c_str());
        CHECK(ok);
        CHECK(sameLayers(back.c.layers, p.layers));
        CHECK_NEAR(back.c.obliquityDeg, p.obliquityDeg, 1e-9);
    }
}

TEST(case_io_catalogue_projectile_keeps_its_fuze_and_shape) {
    const ProjectileDb& db = shippedDb();
    int fuzed = -1;
    for (size_t i = 0; i < db.projectiles().size(); ++i)
        if (db.projectiles()[i].penetrator.fuze.type != phys::FuzeType::None) {
            fuzed = static_cast<int>(i);
            break;
        }
    CHECK(fuzed >= 0);
    if (fuzed < 0) return;
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(fuzed)];
    SavedCase s = baseCase(db);
    s.projectile = fuzed;
    s.c.penetrator = e.penetrator;
    s.c.velocity = e.velocity;
    SavedCase back;
    std::string error;
    CHECK(caseFromJson(db, caseToJson(db, s), back, error));
    CHECK(back.c.penetrator.fuze.type == e.penetrator.fuze.type);
    CHECK(back.c.penetrator.kind == e.penetrator.kind);
}

TEST(case_io_custom_penetrator_overlays_the_saved_figures) {
    const ProjectileDb& db = shippedDb();
    SavedCase s = baseCase(db);
    s.custom = true;
    s.rodMaterial = RodMaterial::Steel;
    s.c.penetrator.diameter = 0.02;
    s.c.penetrator.length = 0.4;
    s.c.penetrator.density = rodMaterialDensity(RodMaterial::Steel);
    SavedCase back;
    std::string error;
    CHECK(caseFromJson(db, caseToJson(db, s), back, error));
    CHECK(back.custom);
    CHECK(back.rodMaterial == RodMaterial::Steel);
    CHECK_NEAR(back.c.penetrator.diameter, 0.02, 1e-9);
    CHECK_NEAR(back.c.penetrator.length, 0.4, 1e-9);
    CHECK_NEAR(back.c.penetrator.density, rodMaterialDensity(RodMaterial::Steel), 1e-6);
}

TEST(case_io_custom_shaped_charge_without_a_catalogue_entry) {
    const ProjectileDb& db = shippedDb();
    SavedCase s;
    s.name = "Own charge";
    s.projectile = -1;
    s.custom = true;
    s.c.penetrator = phys::Penetrator{};
    s.c.penetrator.kind = phys::PenetratorKind::ShapedCharge;
    s.c.penetrator.coneDiameter = 0.08;
    s.c.penetrator.standoff = 3.5;
    s.c.penetrator.jetQuality = 0.9;
    s.c.velocity = 300;
    s.c.layers = {{db.findMaterial("rha"), 0.1, 0, 0, 0, 0, 0}};
    SavedCase back;
    std::string error;
    CHECK(caseFromJson(db, caseToJson(db, s), back, error));
    CHECK(back.projectile == -1);
    CHECK(back.c.penetrator.kind == phys::PenetratorKind::ShapedCharge);
    CHECK_NEAR(back.c.penetrator.coneDiameter, 0.08, 1e-9);
    CHECK_NEAR(back.c.penetrator.standoff, 3.5, 1e-9);
    CHECK_NEAR(back.c.penetrator.jetQuality, 0.9, 1e-9);
}

TEST(case_io_name_with_quotes_survives) {
    const ProjectileDb& db = shippedDb();
    SavedCase s = baseCase(db);
    s.name = "6\" slab \\ \"best\"";
    SavedCase back;
    std::string error;
    CHECK(caseFromJson(db, caseToJson(db, s), back, error));
    CHECK_EQ_STR(back.name, s.name);
}

TEST(case_io_rejects_bad_files) {
    const ProjectileDb& db = shippedDb();
    const std::string good = caseToJson(db, baseCase(db));
    SavedCase out;
    std::string error;
    CHECK(caseFromJson(db, good, out, error));

    auto replaced = [&](const std::string& from, const std::string& to) {
        std::string t = good;
        const size_t at = t.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) t.replace(at, from.size(), to);
        return t;
    };
    error.clear();
    CHECK(!caseFromJson(db, "{ not json", out, error));
    CHECK(!error.empty());
    CHECK(!caseFromJson(db, replaced("\"schema\": 1", "\"schema\": 2"), out, error));
    CHECK(!caseFromJson(db, replaced("\"rha\"", "\"no_such_steel\""), out, error));
    CHECK(error.find("no_such_steel") != std::string::npos);
    CHECK(!caseFromJson(db, replaced("generic_30mm_apfsds", "no_such_round"), out, error));
    CHECK(error.find("no_such_round") != std::string::npos);
    CHECK(!caseFromJson(db, replaced("\"obliquity_deg\": 0", "\"obliquity_deg\": 95"), out, error));
    CHECK(!caseFromJson(db, replaced("\"thickness_mm\": 20", "\"thickness_mm\": 0"), out, error));
    CHECK(!caseFromJson(db,
                        "{\"schema\":1,\"name\":\"x\",\"projectile\":\"generic_30mm_apfsds\",\"velocity_mps\":1500,"
                        "\"obliquity_deg\":0,\"layers\":[]}",
                        out, error));
}

TEST(case_io_rejects_too_many_layers) {
    const ProjectileDb& db = shippedDb();
    SavedCase s = baseCase(db);
    s.c.layers.assign(kMaxArmourLayers + 1, s.c.layers[0]);
    SavedCase out;
    std::string error;
    CHECK(!caseFromJson(db, caseToJson(db, s), out, error));
    CHECK(!error.empty());
}

TEST(case_io_safe_names) {
    CHECK_EQ_STR(safeCaseName("My Test: Case #1!"), "my_test_case_1");
    CHECK_EQ_STR(safeCaseName(""), "case");
    CHECK_EQ_STR(safeCaseName("***"), "case");
    CHECK_EQ_STR(safeCaseName("Keep-this_one"), "keep-this_one");
    const std::string evil = safeCaseName("../../Windows/System32/evil.json");
    CHECK(evil.find('/') == std::string::npos);
    CHECK(evil.find('\\') == std::string::npos);
    CHECK(evil.find('.') == std::string::npos);
    CHECK(safeCaseName(std::string(200, 'a')).size() == 48);
}

TEST(history_csv_has_a_header_and_a_row_per_sample) {
    const ProjectileDb& db = shippedDb();
    SavedCase s = baseCase(db);
    const ArmourOutcome out = evaluateArmour(db, s.c);
    CHECK(!out.result.history.empty());
    const std::vector<std::string> rows = lines(historyCsv(out));
    CHECK(rows.size() == out.result.history.size() + 1);
    if (rows.size() != out.result.history.size() + 1) return;
    CHECK_EQ_STR(rows[0], "time_us,depth_mm,velocity_m_s,length_mm,rate_m_s,layer");
    const phys::PenetrationSample& last = out.result.history.back();
    const std::vector<double> f = fields(rows.back());
    CHECK(f.size() == 6);
    if (f.size() == 6) {
        CHECK_NEAR(f[0], last.time * 1e6, 1e-3 + std::abs(last.time * 1e6) * 1e-5);
        CHECK_NEAR(f[1], last.depth * 1e3, 1e-3 + std::abs(last.depth * 1e3) * 1e-5);
        CHECK_NEAR(f[2], last.velocity, 1e-2 + std::abs(last.velocity) * 1e-5);
        CHECK_NEAR(f[5], last.layer, 1e-9);
    }
}

TEST(history_csv_of_nothing_is_just_the_header) {
    const std::vector<std::string> rows = lines(historyCsv(ArmourOutcome{}));
    CHECK(rows.size() == 1);
    if (rows.size() == 1) CHECK_EQ_STR(rows[0], "time_us,depth_mm,velocity_m_s,length_mm,rate_m_s,layer");
}

TEST(mpm_history_csv_has_a_row_per_valid_measurement) {
    std::vector<MpmMeasure> ms(3);
    ms[0].valid = true;
    ms[0].time = 1e-5;
    ms[0].nose = 0.01;
    ms[0].velocity = 1500;
    ms[0].intact = 400;
    ms[1].valid = false;  // a frame without readouts is left out
    ms[2].valid = true;
    ms[2].time = 3e-5;
    ms[2].nose = 0.03;
    ms[2].velocity = 900;
    ms[2].intact = 250;
    ms[2].failedPlate = 77;
    const std::vector<std::string> rows = lines(mpmHistoryCsv(ms));
    CHECK(rows.size() == 3);
    if (rows.size() != 3) return;
    CHECK(rows[0].rfind("time_us,nose_mm,", 0) == 0);
    size_t columns = 1;
    for (char ch : rows[0])
        if (ch == ',') ++columns;
    const std::vector<double> f = fields(rows[2]);
    CHECK(f.size() == columns);
    if (f.size() >= 2) {
        CHECK_NEAR(f[0], 30.0, 1e-6);
        CHECK_NEAR(f[1], 30.0, 1e-6);
    }
}
