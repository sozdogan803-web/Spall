#include "test.h"

#include "data/explosive_db.h"

#include <cmath>
#include <set>
#include <string>

namespace {

bool loadShipped(ExplosiveDb& db, LoadReport& report) {
    std::string fatal;
    const bool ok = db.load(test::dataDir() + "/explosives.json", report, fatal);
    if (!ok) std::printf("    load failed: %s\n", fatal.c_str());
    return ok;
}

// A complete, valid explosive entry; `extra` is spliced in before the closing brace.
std::string explosive(const std::string& id, const std::string& extra = "") {
    return R"({"id":")" + id + R"(","name":"Test )" + id +
           R"(","composition":[{"component":"TNT","mass_pct":100}],"re_factor":1.0,
           "detonation_velocity_mps":6900,"density_gcc":1.6,"aluminum_fraction":0,"oxygen_balance_pct":-74,
           "soot_yield":0.185,"afterburn_mj_per_kg":10.42,"confidence":"high","sources":["x"],"notes":"")" +
           extra + "}";
}

std::string explosivesJson(const std::string& entries) {
    return std::string(R"({"schema":1,"explosives":[)") + entries + "]}";
}

// Replaces the first occurrence of `from` in `text` (used to corrupt one field of an entry).
std::string with(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}

} // namespace

TEST(explosive_db_loads_shipped_catalogue) {
    ExplosiveDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    CHECK(db.explosives().size() >= 25);
    CHECK(report.skipped == 0);
    for (const std::string& m : report.messages) std::printf("    note: %s\n", m.c_str());
}

TEST(explosive_db_ids_unique_and_findable) {
    ExplosiveDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    std::set<std::string> ids;
    for (const Explosive& e : db.explosives()) {
        CHECK(ids.insert(e.id).second);
        CHECK(db.findExplosive(e.id) == static_cast<int>(&e - db.explosives().data()));
    }
    CHECK(db.findExplosive("no_such_filler") == -1);
}

TEST(explosive_db_shipped_fields_are_physical) {
    ExplosiveDb db;
    LoadReport report;
    CHECK(loadShipped(db, report));
    for (const Explosive& e : db.explosives()) {
        CHECK(e.detonationVelocity > 1000 && e.detonationVelocity < 10000);
        CHECK(e.density > 500 && e.density < 2500);  // kg/m3
        CHECK(e.reFactor > 0.1 && e.reFactor < 10);
        CHECK(!e.composition.empty());
        double pct = 0;
        for (const Component& c : e.composition) pct += c.massPct;
        CHECK(pct > 90 && pct < 110);
    }
}

TEST(explosive_db_converts_density_to_si) {
    ExplosiveDb db;
    LoadReport report;
    std::string fatal;
    CHECK(db.loadText(explosivesJson(explosive("a")), report, fatal));
    CHECK(db.explosives().size() == 1);
    CHECK_NEAR(db.explosives()[0].density, 1600, 1e-9);  // g/cm3 -> kg/m3
    CHECK_EQ_STR(confidenceLabel(db.explosives()[0].confidence), "High");
}

TEST(explosive_db_skips_invalid_entries) {
    ExplosiveDb db;
    LoadReport report;
    std::string fatal;
    const std::string entries =
        explosive("good") + "," +
        explosive("good") + "," +                                                    // duplicate id
        with(explosive("fast"), "\"detonation_velocity_mps\":6900",
             "\"detonation_velocity_mps\":99999") + "," +                            // out of range
        with(explosive("nocomp"), "\"component\"", "\"part\"") + "," +               // bad composition
        with(explosive("heavy"), "\"density_gcc\":1.6", "\"density_gcc\":9") + "," + // out of range
        R"({"id":"missing","name":"No fields"})";
    CHECK(db.loadText(explosivesJson(entries), report, fatal));
    CHECK(db.explosives().size() == 1);
    CHECK(db.findExplosive("good") >= 0);
    CHECK(report.skipped == 5);
    CHECK(report.messages.size() == 5);
}

TEST(explosive_db_syntax_error_is_fatal_with_position) {
    ExplosiveDb db;
    LoadReport report;
    std::string fatal;
    CHECK(!db.loadText("{\"schema\":1,\n\"explosives\":[,]}", report, fatal));
    CHECK(fatal.find("explosives.json") != std::string::npos);
    CHECK(fatal.find("line 2") != std::string::npos);
    CHECK(!db.loadText(R"({"schema":2,"explosives":[]})", report, fatal));
    CHECK(fatal.find("schema") != std::string::npos);
    CHECK(!db.load("does/not/exist.json", report, fatal));
    CHECK(!fatal.empty());
    CHECK(db.explosives().empty());
}
