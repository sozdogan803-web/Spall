#include "data/explosive_db.h"

#include "core/json.h"
#include "data/catalogue_json.h"

#include <unordered_set>

using catalogue::EntryReader;
using catalogue::entryName;
using catalogue::parseDocument;

const char* confidenceLabel(Confidence c) {
    switch (c) {
    case Confidence::High: return "High";
    case Confidence::Medium: return "Medium";
    default: return "Low";
    }
}

bool ExplosiveDb::load(const std::string& path, LoadReport& report, std::string& fatal) {
    std::string text;
    if (!json::readFile(path, text)) {
        fatal = "Cannot read " + path;
        explosives_.clear();
        return false;
    }
    return loadText(text, report, fatal);
}

bool ExplosiveDb::loadText(std::string_view explosivesJson, LoadReport& report, std::string& fatal) {
    explosives_.clear();
    report = {};
    fatal.clear();

    json::Value doc;
    const json::Value* items = nullptr;
    if (!parseDocument(explosivesJson, "explosives.json", "explosives", doc, items, fatal)) return false;

    std::unordered_set<std::string> ids;
    for (size_t i = 0; i < items->size(); ++i) {
        const json::Value& item = (*items)[i];
        EntryReader r(item);
        Explosive e;
        e.id = r.str("id");
        e.name = r.str("name");
        if (const json::Value* composition = item.find("composition")) {
            for (const json::Value& c : composition->asArray()) {
                const json::Value* name = c.find("component");
                const json::Value* pct = c.find("mass_pct");
                if (!name || !name->isString() || !pct || !pct->isNumber())
                    r.fail("composition", "entries need \"component\" and \"mass_pct\"");
                else
                    e.composition.push_back({name->asString(), pct->asNumber()});
            }
        }
        e.reFactor = r.range("re_factor", 0.1, 10);
        e.detonationVelocity = r.range("detonation_velocity_mps", 1000, 10000);
        e.density = r.range("density_gcc", 0.5, 2.5) * 1000.0;
        e.aluminumFraction = r.range("aluminum_fraction", 0, 1);
        e.oxygenBalancePct = r.range("oxygen_balance_pct", -300, 100);
        e.sootYield = r.range("soot_yield", 0, 1);
        e.afterburnMJPerKg = r.range("afterburn_mj_per_kg", 0, 50);
        e.confidence = r.confidence();
        e.sources = r.strings("sources");
        e.notes = r.str("notes", false);
        if (r.ok() && e.id.empty()) r.fail("id", "empty");
        if (r.ok() && !ids.insert(e.id).second) r.fail("id", "duplicate");
        if (!r.ok()) {
            ++report.skipped;
            report.messages.push_back("explosives.json: skipped " + entryName(item, i) + ": " + r.error());
            continue;
        }
        explosives_.push_back(std::move(e));
    }
    return true;
}

int ExplosiveDb::findExplosive(std::string_view id) const {
    for (size_t i = 0; i < explosives_.size(); ++i)
        if (explosives_[i].id == id) return static_cast<int>(i);
    return -1;
}
