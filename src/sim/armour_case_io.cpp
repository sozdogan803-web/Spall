#include "sim/armour_case_io.h"

#include "core/json.h"
#include "data/armour_blocks.h"
#include "data/catalogue_json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kSchema = 1;
constexpr size_t kMaxNameChars = 48;

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char ch : s) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    out += '"';
    return out;
}

bool parseKind(std::string_view id, phys::PenetratorKind& out) {
    for (int i = 0; i < static_cast<int>(phys::PenetratorKind::Count); ++i)
        if (id == phys::penetratorKindId(static_cast<phys::PenetratorKind>(i))) {
            out = static_cast<phys::PenetratorKind>(i);
            return true;
        }
    return false;
}

bool parseRod(std::string_view id, RodMaterial& out) {
    for (int i = 0; i < static_cast<int>(RodMaterial::Count); ++i)
        if (id == rodMaterialId(static_cast<RodMaterial>(i))) {
            out = static_cast<RodMaterial>(i);
            return true;
        }
    return false;
}

// The figures the setup window edits, keyed like the catalogue's (millimetres).
void writePenetrator(std::string& out, const phys::Penetrator& p) {
    out += "  \"penetrator\": {\n    \"kind\": " + quoted(phys::penetratorKindId(p.kind));
    if (p.kind == phys::PenetratorKind::ShapedCharge) {
        out += ",\n    \"cone_diameter_mm\": " + num(p.coneDiameter * 1e3);
        out += ",\n    \"standoff_cd\": " + num(p.standoff);
        out += ",\n    \"jet_quality\": " + num(p.jetQuality);
    } else {
        out += ",\n    \"diameter_mm\": " + num(p.diameter * 1e3);
        out += ",\n    \"length_mm\": " + num(p.length * 1e3);
        out += ",\n    \"density\": " + num(p.density);
        out += ",\n    \"mass_kg\": " + num(p.mass);
        out += std::string(",\n    \"uranium\": ") + (p.uranium ? "true" : "false");
    }
    out += "\n  },\n";
}

// Overlays a saved "penetrator" object on `p` (the catalogue entry's, or a bare one).
void readPenetrator(const json::Value& obj, phys::Penetrator& p, catalogue::EntryReader& top) {
    catalogue::EntryReader r(obj);
    const std::string kind = r.str("kind");
    phys::PenetratorKind k = p.kind;
    if (r.ok() && !parseKind(kind, k)) r.fail("kind", "unknown '" + kind + "'");
    if (r.ok() && k != p.kind) {
        p = phys::Penetrator{};  // another kind of round: nothing of the catalogue entry carries over
        p.kind = k;
    }
    if (k == phys::PenetratorKind::ShapedCharge) {
        p.coneDiameter = r.range("cone_diameter_mm", 5, 1000) * 1e-3;
        p.standoff = r.range("standoff_cd", 0, 50);
        p.jetQuality = r.range("jet_quality", -2, 3);
    } else {
        p.diameter = r.range("diameter_mm", 1, 500) * 1e-3;
        p.length = r.range("length_mm", 1, 5000) * 1e-3;
        p.density = r.range("density", 500, 30000);
        p.mass = r.range("mass_kg", 0, 1000);
        p.uranium = r.flag("uranium", p.uranium);
    }
    if (!r.ok()) top.fail("penetrator", r.error());
}

std::string layerJson(const ProjectileDb& db, const PresetLayer& l) {
    const size_t m = l.material >= 0 && static_cast<size_t>(l.material) < db.materials().size()
                         ? static_cast<size_t>(l.material)
                         : 0;
    std::string out = "    { \"material\": " + quoted(db.materials().empty() ? "" : db.materials()[m].id);
    out += ", \"thickness_mm\": " + num(l.thickness * 1e3);
    if (l.gap != 0) out += ", \"gap_mm\": " + num(l.gap * 1e3);
    if (isBlockLayer(l)) {
        out += ", \"block_width_mm\": " + num(l.blockWidth * 1e3);
        out += ", \"block_gap_mm\": " + num(l.blockGap * 1e3);
        out += ", \"offset_mm\": " + num(l.offset * 1e3);
        out += ", \"block_count\": " + std::to_string(l.blockCount);
    }
    out += " }";
    return out;
}

}  // namespace

std::string caseToJson(const ProjectileDb& db, const SavedCase& s) {
    std::string out = "{\n  \"schema\": " + std::to_string(kSchema) + ",\n";
    out += "  \"name\": " + quoted(s.name) + ",\n";
    if (s.projectile >= 0 && static_cast<size_t>(s.projectile) < db.projectiles().size())
        out += "  \"projectile\": " + quoted(db.projectiles()[static_cast<size_t>(s.projectile)].id) + ",\n";
    out += std::string("  \"custom\": ") + (s.custom ? "true" : "false") + ",\n";
    out += "  \"rod_material\": " + quoted(rodMaterialId(s.rodMaterial)) + ",\n";
    out += "  \"velocity_mps\": " + num(s.c.velocity) + ",\n";
    out += "  \"obliquity_deg\": " + num(s.c.obliquityDeg) + ",\n";
    if (s.custom || s.projectile < 0) writePenetrator(out, s.c.penetrator);
    out += "  \"layers\": [\n";
    for (size_t i = 0; i < s.c.layers.size(); ++i)
        out += layerJson(db, s.c.layers[i]) + (i + 1 < s.c.layers.size() ? ",\n" : "\n");
    out += "  ]\n}\n";
    return out;
}

bool caseFromJson(const ProjectileDb& db, std::string_view text, SavedCase& out, std::string& error) {
    error.clear();
    json::Value doc;
    std::string parseError;
    if (!json::parse(text, doc, parseError)) {
        error = parseError;
        return false;
    }
    if (!doc.isObject()) {
        error = "a case file is a JSON object";
        return false;
    }
    const json::Value* schema = doc.find("schema");
    if (!schema || schema->asNumber(-1) != kSchema) {
        error = "unsupported or missing \"schema\" (expected " + std::to_string(kSchema) + ")";
        return false;
    }

    catalogue::EntryReader r(doc);
    SavedCase s;
    s.name = r.str("name", false);
    const std::string projectile = r.str("projectile", false);
    const ProjectileEntry* entry = nullptr;
    if (r.ok() && !projectile.empty()) {
        s.projectile = db.findProjectile(projectile);
        if (s.projectile < 0)
            r.fail("projectile", "unknown '" + projectile + "'");
        else
            entry = &db.projectiles()[static_cast<size_t>(s.projectile)];
    }
    s.custom = r.flag("custom", false);
    if (entry) {
        s.c.penetrator = entry->penetrator;
        s.rodMaterial = entry->rodMaterial;
    }
    if (r.ok() && r.has("rod_material")) {
        const std::string rod = r.str("rod_material");
        if (r.ok() && !parseRod(rod, s.rodMaterial)) r.fail("rod_material", "unknown '" + rod + "'");
    }
    s.c.velocity = r.range("velocity_mps", 10, 3000);
    s.c.obliquityDeg = r.range("obliquity_deg", -phys::kMaxObliquityDeg, phys::kMaxObliquityDeg);
    if (r.ok()) {
        const json::Value* pen = doc.find("penetrator");
        if (pen && !pen->isObject())
            r.fail("penetrator", "must be an object");
        else if (pen && (s.custom || !entry))
            readPenetrator(*pen, s.c.penetrator, r);
        else if (!entry)
            r.fail("penetrator", "missing (a case without a catalogue projectile carries its figures)");
    }

    const json::Value* layers = doc.find("layers");
    if (r.ok()) {
        if (!layers || !layers->isArray() || layers->size() == 0 || layers->size() > kMaxArmourLayers)
            r.fail("layers", "must be an array of 1-" + std::to_string(kMaxArmourLayers) + " layers");
        else
            for (const json::Value& l : layers->asArray()) {
                catalogue::EntryReader lr(l);
                PresetLayer layer;
                const std::string material = lr.str("material");
                layer.material = db.findMaterial(material);
                if (lr.ok() && layer.material < 0) lr.fail("material", "unknown '" + material + "'");
                layer.thickness = lr.range("thickness_mm", 0.1, 5000) * 1e-3;
                layer.gap = lr.optRange("gap_mm", 0, 10000, 0) * 1e-3;
                const double width = lr.optRange("block_width_mm", 0, 5000, 0);
                if (lr.ok() && width > 0 && width < 10)
                    lr.fail("block_width_mm", "must be 0 (a full plate) or at least 10 mm");
                layer.blockWidth = width * 1e-3;
                layer.blockGap = lr.optRange("block_gap_mm", 0, 1000, 0) * 1e-3;
                layer.offset = lr.optRange("offset_mm", -5000, 5000, 0) * 1e-3;
                layer.blockCount = static_cast<int>(std::lround(lr.optRange("block_count", 0, 64, 0)));
                if (!lr.ok()) {
                    r.fail("layers", lr.error());
                    break;
                }
                s.c.layers.push_back(layer);
            }
    }
    if (!r.ok()) {
        error = r.error();
        return false;
    }
    out = std::move(s);
    return true;
}

std::string safeCaseName(std::string_view name) {
    std::string out;
    bool pendingGap = false;
    for (const unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '-') {
            if (pendingGap && !out.empty()) out += '_';
            pendingGap = false;
            out += static_cast<char>(std::tolower(ch));
        } else {
            pendingGap = true;  // '_' and every other character: one separator, never first or last
        }
        if (out.size() >= kMaxNameChars) break;
    }
    out.resize(std::min(out.size(), kMaxNameChars));
    return out.empty() ? "case" : out;
}

std::string historyCsv(const ArmourOutcome& out) {
    std::string csv = "time_us,depth_mm,velocity_m_s,length_mm,rate_m_s,layer\n";
    char row[200];
    for (const phys::PenetrationSample& s : out.result.history) {
        std::snprintf(row, sizeof(row), "%.9g,%.9g,%.9g,%.9g,%.9g,%d\n", s.time * 1e6, s.depth * 1e3, s.velocity,
                      s.length * 1e3, s.rate, s.layer);
        csv += row;
    }
    return csv;
}

std::string mpmHistoryCsv(const std::vector<MpmMeasure>& measures) {
    std::string csv =
        "time_us,nose_mm,tail_mm,reach_mm,residual_length_mm,velocity_m_s,max_speed_m_s,max_temperature_k,"
        "max_stress_gpa,max_plastic_strain,momentum_x_kg_m_s_per_m,momentum_y_kg_m_s_per_m,intact,failed_plate,"
        "failed_strain,failed_spall,failed_melt,reacted,plate_speed_m_s,comminuted,delaminated,fibre_broken,"
        "lead_velocity_m_s,jet_broken,fuze_triggered,fuze_time_us,filler_lit\n";
    char row[600];
    for (const MpmMeasure& m : measures) {
        if (!m.valid) continue;
        std::snprintf(row, sizeof(row),
                      "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%d,%d,%d,%.9g,%d,%d,%d,"
                      "%.9g,%d,%d,%.9g,%d\n",
                      m.time * 1e6, m.nose * 1e3, m.tail * 1e3, m.reach * 1e3, m.residualLength * 1e3, m.velocity,
                      m.maxSpeed, m.maxTemperature, m.maxStress * 1e-9, m.maxPlasticStrain, m.momentumX, m.momentumY,
                      m.intact, m.failedPlate, m.failedStrain, m.failedSpall, m.failedMelt, m.reacted, m.plateSpeed,
                      m.comminuted, m.delaminated, m.fibreBroken, m.leadVelocity, m.jetBroken,
                      m.fuzeTriggered ? 1 : 0, m.fuzeTriggered ? m.fuzeTime * 1e6 : 0.0, m.fillerLit);
        csv += row;
    }
    return csv;
}
