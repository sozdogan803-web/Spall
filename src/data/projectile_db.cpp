#include "data/projectile_db.h"

#include "core/json.h"
#include "data/catalogue_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <unordered_set>

using catalogue::EntryReader;
using catalogue::entryName;
using catalogue::parseDocument;

namespace {

struct GroupInfo {
    const char* id;
    const char* label;
};

constexpr GroupInfo kGroups[] = {
    {"steel", "Steel"},
    {"light_metal", "Light metal"},
    {"ceramic", "Ceramic"},
    {"other", "Other"},
};
static_assert(std::size(kGroups) == static_cast<size_t>(MaterialGroup::Count));

struct RodInfo {
    const char* id;
    const char* label;
    double density;
};

constexpr RodInfo kRods[] = {
    {"tungsten", "Tungsten alloy", 17600},
    {"du", "Depleted uranium", 18600},
    {"steel", "Steel", 7850},
};
static_assert(std::size(kRods) == static_cast<size_t>(RodMaterial::Count));

bool parseGroup(std::string_view id, MaterialGroup& out) {
    for (size_t i = 0; i < std::size(kGroups); ++i)
        if (id == kGroups[i].id) {
            out = static_cast<MaterialGroup>(i);
            return true;
        }
    return false;
}

// The fields of one object of a `shape` block; problems go to the entry's reader as
// "shape.<object>.<key>: ...". An absent object reads as all defaults.
struct ShapeFields {
    const json::Value* obj;
    std::string prefix;
    EntryReader& r;

    const json::Value* get(const char* key) const { return obj ? obj->find(key) : nullptr; }
    bool has(const char* key) const { return get(key) != nullptr; }
    bool present() const { return obj != nullptr; }

    double number(const char* key, double lo, double hi, double def) {
        const json::Value* v = get(key);
        if (!v) return def;
        if (!v->isNumber()) {
            r.fail(prefix + key, "must be a number");
            return def;
        }
        const double x = v->asNumber();
        if (!(x >= lo && x <= hi)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%g is outside %g..%g", x, lo, hi);
            r.fail(prefix + key, buf);
            return def;
        }
        return x;
    }
    // A size in mm, returned in m; 0 when absent (the style's default).
    double mm(const char* key) { return number(key, 0, 20000, 0) * 1e-3; }
    // A position in mm, returned in m; -1 when absent (the style's default).
    double position(const char* key) { return has(key) ? mm(key) : -1; }
    std::optional<double> crh(const char* key) {
        if (!has(key)) return std::nullopt;
        return number(key, 0, 20, 0);
    }
    std::string text(const char* key, const std::string& def) {
        const json::Value* v = get(key);
        if (!v) return def;
        if (!v->isString()) {
            r.fail(prefix + key, "must be a string");
            return def;
        }
        return v->asString();
    }
    // A part material id (mpmPartMaterial); `def` when absent.
    std::string material(const char* key, const std::string& def) {
        const std::string id = text(key, def);
        if (has(key) && r.ok() && !phys::mpmPartMaterial(id)) r.fail(prefix + key, "unknown material '" + id + "'");
        return id;
    }
    ShapeFields sub(const char* key) {
        const json::Value* v = get(key);
        if (v && !v->isObject()) {
            r.fail(prefix + key, "must be an object");
            v = nullptr;
        }
        return {v, prefix + key + ".", r};
    }
};

// Parses an entry's `shape` block into `s` (sizes in m) and checks that its penetrator part
// matches the entry's diameter (2 %) and length (5 %).
void parseShape(const json::Value& v, const phys::Penetrator& p, const std::vector<Explosive>& explosives,
                EntryReader& r, phys::ShapeSpec& s) {
    if (!v.isObject()) {
        r.fail("shape", "must be an object");
        return;
    }
    ShapeFields f{&v, "shape.", r};
    const std::string styleId = f.text("style", "");
    const std::optional<phys::ShapeStyle> style = phys::parseShapeStyle(styleId);
    if (!style) {
        r.fail("shape.style", "unknown '" + styleId + "'");
        return;
    }
    using phys::ShapeStyle;
    s.style = *style;
    s.length = f.mm("length_mm");
    s.diameter = f.mm("diameter_mm");
    s.noseCrh = f.crh("nose_crh");
    if (!s.noseCrh && s.style != ShapeStyle::Dart && s.style != ShapeStyle::HeMissile && s.style != ShapeStyle::HeatRocket)
        s.noseCrh = 1.5;
    s.boatTail = f.mm("boat_tail_mm");
    s.boatTailDiameter = f.mm("boat_tail_diameter_mm");
    ShapeFields band = f.sub("band");
    s.band.fromBase = band.mm("from_base_mm");
    s.band.width = band.mm("width_mm");
    ShapeFields cap = f.sub("cap");
    s.cap.length = cap.mm("length_mm");
    s.cap.material = cap.material("material", s.cap.material);
    ShapeFields ws = f.sub("windshield");
    s.windshield.length = ws.mm("length_mm");
    s.windshield.crh = ws.number("crh", 0, 20, 0);
    s.windshield.wall = ws.mm("wall_mm");
    s.windshield.material = ws.material("material", s.windshield.material);
    ShapeFields core = f.sub("core");
    s.core.diameter = core.mm("diameter_mm");
    s.core.length = core.mm("length_mm");
    s.core.noseCrh = core.crh("nose_crh");
    s.core.fromNose = core.position("from_nose_mm");
    s.core.material = core.material("material", "");
    s.bodyMaterial = f.material("body_material", "");
    ShapeFields jacket = f.sub("jacket");
    s.jacket.thickness = jacket.mm("thickness_mm");
    s.jacket.material = jacket.material("material", s.jacket.material);
    s.incendiary.length = f.sub("incendiary").mm("length_mm");
    ShapeFields fins = f.sub("fins");
    s.fins.span = fins.mm("span_mm");
    s.fins.rootChord = fins.mm("root_chord_mm");
    s.fins.tipChord = fins.mm("tip_chord_mm");
    s.fins.sweep = fins.position("sweep_mm");
    ShapeFields tracer = f.sub("tracer");
    s.tracer.length = tracer.mm("length_mm");
    s.tracer.diameter = tracer.mm("diameter_mm");
    ShapeFields dartCap = f.sub("dart_cap");
    s.dartCap.length = dartCap.mm("length_mm");
    s.dartCap.wall = dartCap.mm("wall_mm");
    s.caseWall = f.mm("case_wall_mm");
    s.warheadLength = f.mm("warhead_length_mm");
    s.noseLength = f.mm("nose_length_mm");
    s.noseMaterial = f.material("nose_material", s.noseMaterial);
    s.motorDiameter = f.mm("motor_diameter_mm");
    s.massKg = f.number("mass_kg", 0, 100000, 0);

    ShapeFields filler = f.sub("filler");
    ShapeFields fuze = f.sub("fuze");
    // A HEAT round's charge is lit by its own base fuze when the nose strikes (the scene programs it).
    const bool heat = s.style == ShapeStyle::HeatRocket;
    if (heat && (p.kind != phys::PenetratorKind::ShapedCharge)) r.fail("shape.style", "heat_rocket is for shaped charges");
    if (heat && fuze.present() && r.ok()) r.fail("shape.fuze", "a heat_rocket's charge needs no fuze block");
    if (filler.present()) {
        s.filler.explosive = filler.text("explosive", "");
        s.filler.diameter = filler.mm("diameter_mm");
        s.filler.length = filler.mm("length_mm");
        s.filler.fromNose = filler.position("from_nose_mm");
        const Explosive* x = nullptr;
        for (const Explosive& e : explosives)
            if (e.id == s.filler.explosive) x = &e;
        if (!x) {
            if (r.ok()) r.fail("shape.filler.explosive", "unknown explosive '" + s.filler.explosive + "'");
        } else {
            s.filler.density = x->density;
            s.filler.detonationSpeed = x->detonationVelocity;
            s.filler.energy = 4e6 * x->reFactor;
        }
        if (r.ok() && !fuze.present() && !heat) r.fail("shape.filler", "an explosive filler needs a fuze");
    } else if (fuze.present() && r.ok()) {
        r.fail("shape.fuze", "a fuze needs a filler");
    }
    if (fuze.present()) {
        const std::string type = fuze.text("type", "");
        if (type == "base_delay") s.fuze.type = phys::FuzeType::BaseDelay;
        else if (type == "nose_contact") s.fuze.type = phys::FuzeType::NoseContact;
        else if (r.ok()) r.fail("shape.fuze.type", "'" + type + "' is not base_delay or nose_contact");
        s.fuze.triggerDv = fuze.number("trigger_dv_mps", 0.1, 10000, s.fuze.type == phys::FuzeType::BaseDelay ? 30 : 10);
        s.fuze.delay = fuze.number("delay_us", 0, 1e6, 0) * 1e-6;
        s.fuze.functionTime = fuze.number("function_us", 0, 1e6, 0) * 1e-6;
        s.fuze.fillerLength = s.filler.length;
        s.fuze.detonationSpeed = s.filler.detonationSpeed;
    }
    if (!r.ok()) return;

    s.refLength = p.length;
    s.refDiameter = p.diameter;
    s.present = true;
    if (heat) return;  // the cone is the entry's own; nothing to match
    const phys::ProjectileShape shape = phys::buildShape(s, p);
    const double extent = shape.penetratorBack - shape.penetratorFront;
    char buf[160];
    if (!(std::fabs(shape.penetratorDiameter / p.diameter - 1) <= 0.02)) {
        std::snprintf(buf, sizeof(buf), "the penetrator part is %.1f mm across, the entry %.1f mm (2 %% allowed)",
                      shape.penetratorDiameter * 1e3, p.diameter * 1e3);
        r.fail("shape", buf);
    } else if (!(std::fabs(extent / p.length - 1) <= 0.05)) {
        std::snprintf(buf, sizeof(buf), "the penetrator part is %.1f mm long, the entry %.1f mm (5 %% allowed)",
                      extent * 1e3, p.length * 1e3);
        r.fail("shape", buf);
    }
    if (!r.ok()) s.present = false;
}

// The parts' mass to compare with the entry's mass_kg: the whole round for shells, the core
// (or rod) otherwise; 0 when there is nothing to compare (no catalogue mass, missiles).
double shapeCheckMass(const phys::ShapeSpec& s, const phys::Penetrator& p) {
    if (!s.present || !(p.mass > 0) || s.style == phys::ShapeStyle::HeMissile || s.style == phys::ShapeStyle::HeatRocket)
        return 0;
    const bool wholeRound = s.style == phys::ShapeStyle::Ap || s.style == phys::ShapeStyle::Apc ||
                            s.style == phys::ShapeStyle::Apbc || s.style == phys::ShapeStyle::Apcbc;
    double mass = 0;
    for (const phys::ShapePart& part : phys::buildShape(s, p).parts) {
        if (!wholeRound && !part.penetrator) continue;
        double rho = part.density;
        if (!(rho > 0)) {
            if (part.material.empty()) rho = p.density;
            else if (const std::optional<phys::MpmMaterial> m = phys::mpmPartMaterial(part.material)) rho = m->density;
        }
        mass += phys::partMass(part, rho);
    }
    return mass;
}

bool parseRod(std::string_view id, RodMaterial& out) {
    for (size_t i = 0; i < std::size(kRods); ++i)
        if (id == kRods[i].id) {
            out = static_cast<RodMaterial>(i);
            return true;
        }
    return false;
}

bool parseKind(std::string_view id, phys::PenetratorKind& out) {
    for (int i = 0; i < static_cast<int>(phys::PenetratorKind::Count); ++i)
        if (id == phys::penetratorKindId(static_cast<phys::PenetratorKind>(i))) {
            out = static_cast<phys::PenetratorKind>(i);
            return true;
        }
    return false;
}

bool parseColor(const std::string& s, unsigned& out) {
    if (s.size() != 7 || s[0] != '#') return false;
    unsigned value = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = s[i];
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<unsigned>(c - 'A' + 10);
        else
            return false;
        value = value * 16 + digit;
    }
    out = value;
    return true;
}

} // namespace

const char* materialGroupId(MaterialGroup g) { return kGroups[static_cast<size_t>(g)].id; }
const char* materialGroupLabel(MaterialGroup g) { return kGroups[static_cast<size_t>(g)].label; }
const char* rodMaterialId(RodMaterial m) { return kRods[static_cast<size_t>(m)].id; }
const char* rodMaterialLabel(RodMaterial m) { return kRods[static_cast<size_t>(m)].label; }
double rodMaterialDensity(RodMaterial m) { return kRods[static_cast<size_t>(m)].density; }

phys::MpmMaterial rodMaterialMpm(RodMaterial m) {
    switch (m) {
    case RodMaterial::Tungsten: return phys::mpmTungstenAlloy();
    case RodMaterial::Uranium: return phys::mpmDepletedUranium();
    default: return phys::mpmPenetratorSteel();
    }
}

bool parseMpmModel(const std::string& s, phys::MpmModel& out) {
    for (int i = 0; i < static_cast<int>(phys::MpmModel::Count); ++i)
        if (s == phys::mpmModelId(static_cast<phys::MpmModel>(i))) {
            out = static_cast<phys::MpmModel>(i);
            return true;
        }
    return false;
}

phys::MpmMaterial mpmFallback(MaterialGroup group, const phys::ArmourMaterial& a) {
    phys::MpmMaterial m;  // armour steel
    m.density = a.density;
    const double bhn = a.hardnessBhn;
    switch (group) {
    case MaterialGroup::Steel:
        m.shear = 78e9;
        m.bulk = 160e9;
        m.yield = 3.3e6 * (bhn > 0 ? bhn : 300);
        m.failureStrain = std::clamp(360.0 / std::max(bhn, 1.0), 0.3, 2.0);
        m.spall = std::clamp(0.01e9 * std::max(bhn, 1.0) * 0.7, 1.5e9, 4e9);
        break;
    case MaterialGroup::LightMetal:
        if (a.density < 3500) {  // aluminium
            m.shear = 26.9e9;
            m.bulk = 70e9;
            m.yield = 2.5e6 * bhn;
            m.hardening = 0.45e9;
            m.hardeningExp = 0.45;
            m.softeningExp = 1.0;
            m.meltK = 890;
            m.heat = 900;
            m.failureStrain = 0.8;
            m.spall = 1.1e9;
        } else {  // titanium
            m.shear = 42.5e9;
            m.bulk = 119e9;
            m.yield = 3.3e6 * bhn;
            m.hardening = 1.0e9;
            m.hardeningExp = 0.9;
            m.softeningExp = 1.1;
            m.meltK = 1878;
            m.heat = 560;
            m.failureStrain = 0.5;
            m.spall = 3e9;
        }
        break;
    case MaterialGroup::Ceramic:  // alumina-like JH-2 set (the MpmMaterial defaults)
        m.model = phys::MpmModel::Ceramic;
        m.shear = 150e9;
        m.bulk = 220e9;
        m.hel = std::clamp(2 * a.resistance, 2e9, 25e9);
        m.meltK = 2300;
        m.heat = 800;
        break;
    default:  // polymers, rubber, concrete, soil
        m.shear = 1e9;
        m.bulk = 4e9;
        m.yield = 0.05 * a.resistance;
        m.hardening = 0;
        m.hardeningExp = 1;
        m.softeningExp = 1;
        m.meltK = 600;
        m.heat = 1500;
        m.failureStrain = 0.5;
        m.spall = 0.05e9;
        break;
    }
    return m;
}

bool ProjectileDb::load(const std::string& materialsPath, const std::string& projectilesPath, LoadReport& report,
                        std::string& fatal) {
    std::string materialsText, projectilesText;
    const bool readMaterials = json::readFile(materialsPath, materialsText);
    if (!readMaterials || !json::readFile(projectilesPath, projectilesText)) {
        fatal = "Cannot read " + (readMaterials ? projectilesPath : materialsPath);
        materials_.clear();
        presets_.clear();
        projectiles_.clear();
        return false;
    }
    const std::string explosivesPath =
        (std::filesystem::path(projectilesPath).parent_path() / "explosives.json").string();
    std::string explosivesText;
    if (!json::readFile(explosivesPath, explosivesText)) {
        fatal = "Cannot read " + explosivesPath;
        materials_.clear();
        presets_.clear();
        projectiles_.clear();
        explosives_.clear();
        return false;
    }
    return loadText(materialsText, projectilesText, report, fatal, explosivesText);
}

bool ProjectileDb::loadText(std::string_view materialsJson, std::string_view projectilesJson, LoadReport& report,
                            std::string& fatal, std::string_view explosivesJson) {
    materials_.clear();
    presets_.clear();
    projectiles_.clear();
    explosives_.clear();
    report = {};
    fatal.clear();

    if (!explosivesJson.empty()) {
        ExplosiveDb explosives;
        LoadReport explosivesReport;
        if (!explosives.loadText(explosivesJson, explosivesReport, fatal)) return false;
        explosives_ = explosives.explosives();
        report = explosivesReport;
    }

    json::Value materialsDoc, projectilesDoc;
    const json::Value* materialItems = nullptr;
    const json::Value* projectileItems = nullptr;
    if (!parseDocument(materialsJson, "armour_materials.json", "materials", materialsDoc, materialItems, fatal) ||
        !parseDocument(projectilesJson, "projectiles.json", "projectiles", projectilesDoc, projectileItems, fatal))
        return false;

    auto skip = [&](const char* file, const std::string& name, const std::string& why) {
        ++report.skipped;
        report.messages.push_back(std::string(file) + ": skipped " + name + ": " + why);
    };

    std::unordered_set<std::string> ids;
    for (size_t i = 0; i < materialItems->size(); ++i) {
        const json::Value& item = (*materialItems)[i];
        EntryReader r(item);
        ArmourMaterialEntry m;
        m.id = r.str("id");
        m.name = r.str("name");
        const std::string group = r.str("group");
        if (r.ok() && !parseGroup(group, m.group)) r.fail("group", "unknown '" + group + "'");
        m.material.density = r.range("density_kgm3", 100, 25000);
        m.material.hardnessBhn = r.range("hardness_bhn", 0, 800);
        if (r.has("resistance_gpa")) {
            m.material.resistance = r.range("resistance_gpa", 0, 20) * 1e9;
        } else if (r.ok()) {
            if (m.group != MaterialGroup::Steel || m.material.hardnessBhn <= 0)
                r.fail("resistance_gpa", "required unless the material is a steel with a hardness");
            m.material.resistance = phys::steelResistanceFromBhn(m.material.hardnessBhn);
            m.resistanceFromHardness = true;
        }
        m.material.keEfficiency = r.range("ke_efficiency", 0.05, 5);
        m.material.ceramic = r.flag("ceramic", false);
        m.material.reactive = r.flag("reactive", false);
        m.mpm = mpmFallback(m.group, m.material);
        if (r.ok() && r.has("mpm")) {
            const json::Value& block = *item.find("mpm");
            if (!block.isObject()) {
                r.fail("mpm", "must be an object");
            } else {
                EntryReader b(block);
                phys::MpmMaterial& p = m.mpm;
                const phys::MpmModel fallbackModel = p.model;
                if (b.has("model")) {
                    const std::string model = b.str("model");
                    if (b.ok() && !parseMpmModel(model, p.model))
                        b.fail("model", "unknown '" + model + "' (metal, ceramic, elastomer or composite)");
                }
                using M = phys::MpmModel;
                // Keys a model does not use are rejected rather than silently ignored.
                auto only = [&](const char* key, std::initializer_list<M> models) {
                    if (!b.ok() || !b.has(key)) return;
                    for (M x : models)
                        if (p.model == x) return;
                    b.fail(key, std::string("not used by the ") + phys::mpmModelId(p.model) + " model");
                };
                for (const char* key : {"yield_gpa", "hardening_gpa", "hardening_exp", "softening_exp"})
                    only(key, {M::Metal, M::Composite});
                for (const char* key : {"failure_strain", "spall_gpa"}) only(key, {M::Metal, M::Elastomer, M::Composite});
                for (const char* key : {"detonation_kms", "energy_mjkg", "products_gamma", "initiation_gpa"})
                    only(key, {M::Metal});
                for (const char* key : {"hel_gpa", "intact_a", "intact_n", "fractured_b", "fractured_m", "tensile_gpa",
                                        "fractured_max", "fracture_d1", "fracture_d2"})
                    only(key, {M::Ceramic});
                for (const char* key : {"viscosity_pas", "viscous_limit_gpa"}) only(key, {M::Elastomer});
                for (const char* key : {"fibre_modulus_gpa", "fibre_failure_strain", "delam_tension_gpa", "delam_shear_gpa"})
                    only(key, {M::Composite});
                if (b.ok() && p.model == M::Ceramic && fallbackModel != M::Ceramic && !b.has("hel_gpa"))
                    b.fail("hel_gpa", "required with the ceramic model");
                if (b.ok() && p.model == M::Composite && !b.has("fibre_modulus_gpa"))
                    b.fail("fibre_modulus_gpa", "required with the composite model");
                p.shear = b.optRange("shear_gpa", 1e-6, 1000, p.shear / 1e9) * 1e9;
                p.bulk = b.optRange("bulk_gpa", 1e-2, 1000, p.bulk / 1e9) * 1e9;
                p.yield = b.optRange("yield_gpa", 0, 20, p.yield / 1e9) * 1e9;
                p.hardening = b.optRange("hardening_gpa", 0, 20, p.hardening / 1e9) * 1e9;
                p.hardeningExp = b.optRange("hardening_exp", 0.01, 5, p.hardeningExp);
                p.softeningExp = b.optRange("softening_exp", 0.01, 5, p.softeningExp);
                p.meltK = b.optRange("melt_k", 300, 5000, p.meltK);
                p.heat = b.optRange("heat_jkgk", 50, 5000, p.heat);
                p.failureStrain = b.optRange("failure_strain", 1e-3, 10, p.failureStrain);
                p.spall = b.optRange("spall_gpa", 1e-5, 20, p.spall / 1e9) * 1e9;
                p.detonationSpeed = b.optRange("detonation_kms", 1, 10, 0) * 1e3;
                p.energy = b.optRange("energy_mjkg", 0.5, 10, p.energy / 1e6) * 1e6;
                p.productsGamma = b.optRange("products_gamma", 1.2, 5, p.productsGamma);
                p.initiation = b.optRange("initiation_gpa", 0.1, 50, p.initiation / 1e9) * 1e9;
                p.hel = b.optRange("hel_gpa", 0.5, 40, p.hel / 1e9) * 1e9;
                p.intactA = b.optRange("intact_a", 0.05, 3, p.intactA);
                p.intactN = b.optRange("intact_n", 0.05, 2, p.intactN);
                p.fracturedB = b.optRange("fractured_b", 0, 3, p.fracturedB);
                p.fracturedM = b.optRange("fractured_m", 0.05, 2, p.fracturedM);
                p.tensile = b.optRange("tensile_gpa", 0.001, 5, p.tensile / 1e9) * 1e9;
                p.fracturedMax = b.optRange("fractured_max", 0, 3, p.fracturedMax);
                p.fractureD1 = b.optRange("fracture_d1", 1e-5, 1, p.fractureD1);
                p.fractureD2 = b.optRange("fracture_d2", 0, 3, p.fractureD2);
                p.viscosity = b.optRange("viscosity_pas", 0, 1e5, p.viscosity);
                p.viscousLimit = b.optRange("viscous_limit_gpa", 0, 5, p.viscousLimit / 1e9) * 1e9;
                p.fibreModulus = b.optRange("fibre_modulus_gpa", 0.1, 500, p.fibreModulus / 1e9) * 1e9;
                p.fibreFailureStrain = b.optRange("fibre_failure_strain", 1e-3, 0.5, p.fibreFailureStrain);
                p.delamTension = b.optRange("delam_tension_gpa", 1e-5, 5, p.delamTension / 1e9) * 1e9;
                p.delamShear = b.optRange("delam_shear_gpa", 1e-5, 5, p.delamShear / 1e9) * 1e9;
                if (!b.ok()) r.fail("mpm." + b.error().substr(0, b.error().find(':')),
                                    b.error().substr(b.error().find(':') + 2));
                m.mpmFromData = true;
            }
        }
        if (r.ok() && m.material.reactive && m.mpm.model != phys::MpmModel::Metal)
            r.fail("mpm.model", "a reactive material needs the metal model");
        if (r.ok() && m.material.reactive != phys::mpmReactive(m.mpm))
            r.fail("reactive", m.material.reactive ? "a reactive material needs mpm.detonation_kms"
                                                   : "mpm.detonation_kms needs \"reactive\": true");
        if (r.has("color")) {
            const std::string color = r.str("color");
            if (r.ok() && !parseColor(color, m.color)) r.fail("color", "must look like #RRGGBB");
        }
        m.confidence = r.confidence();
        m.sources = r.strings("sources");
        m.notes = r.str("notes", false);
        if (r.ok() && m.id.empty()) r.fail("id", "empty");
        if (r.ok() && !ids.insert(m.id).second) r.fail("id", "duplicate");
        if (!r.ok()) {
            skip("armour_materials.json", entryName(item, i), r.error());
            continue;
        }
        materials_.push_back(std::move(m));
    }

    ids.clear();
    if (const json::Value* presetItems = materialsDoc.find("presets")) {
        for (size_t i = 0; i < presetItems->size(); ++i) {
            const json::Value& item = (*presetItems)[i];
            EntryReader r(item);
            ArmourPreset p;
            p.id = r.str("id");
            p.name = r.str("name");
            p.obliquityDeg = r.range("obliquity_deg", 0, phys::kMaxObliquityDeg);
            const json::Value* layers = item.find("layers");
            if (!layers || !layers->isArray() || layers->size() == 0 || layers->size() > kMaxArmourLayers)
                r.fail("layers", "must be an array of 1-" + std::to_string(kMaxArmourLayers) + " layers");
            else
                for (const json::Value& l : layers->asArray()) {
                    EntryReader lr(l);
                    PresetLayer layer;
                    const std::string material = lr.str("material");
                    layer.material = findMaterial(material);
                    if (lr.ok() && layer.material < 0) lr.fail("material", "unknown '" + material + "'");
                    layer.thickness = lr.range("thickness_mm", 0.1, 5000) * 1e-3;
                    layer.gap = lr.optRange("gap_mm", 0, 10000, 0) * 1e-3;
                    // Separate tiles along the face (src/data/armour_blocks.h).
                    const double width = lr.optRange("block_width_mm", 0, 5000, 0);
                    if (lr.ok() && width > 0 && width < 10)
                        lr.fail("block_width_mm", "must be 0 (a full plate) or at least 10 mm");
                    layer.blockWidth = width * 1e-3;
                    layer.blockGap = lr.optRange("block_gap_mm", 0, 1000, 0) * 1e-3;
                    layer.offset = lr.optRange("offset_mm", -5000, 5000, 0) * 1e-3;
                    layer.blockCount = static_cast<int>(std::lround(lr.optRange("block_count", 0, 64, 0)));
                    if (!lr.ok()) r.fail("layers", lr.error());
                    p.layers.push_back(layer);
                }
            if (r.ok() && p.id.empty()) r.fail("id", "empty");
            if (r.ok() && !ids.insert(p.id).second) r.fail("id", "duplicate");
            if (!r.ok()) {
                skip("armour_materials.json", "preset " + entryName(item, i), r.error());
                continue;
            }
            presets_.push_back(std::move(p));
        }
    }

    ids.clear();
    for (size_t i = 0; i < projectileItems->size(); ++i) {
        const json::Value& item = (*projectileItems)[i];
        EntryReader r(item);
        ProjectileEntry e;
        phys::Penetrator& p = e.penetrator;
        e.id = r.str("id");
        e.name = r.str("name");
        const std::string kind = r.str("kind");
        if (r.ok() && !parseKind(kind, p.kind)) r.fail("kind", "unknown '" + kind + "'");
        e.country = r.str("country");
        e.year = static_cast<int>(r.range("year", 1850, 2026));
        e.gun = r.str("gun");
        e.velocity = r.range("velocity_mps", 10, 3000);
        e.confidence = r.confidence();
        e.sources = r.strings("sources");
        e.notes = r.str("notes", false);
        if (r.has("published_rha_mm")) {
            e.hasPublished = true;
            e.publishedRha = r.range("published_rha_mm", 1, 3000) * 1e-3;
        }
        e.publishedConditions = r.str("published_conditions", false);
        e.publishedVelocity = r.optRange("published_velocity_mps", 10, 3000, e.velocity);

        if (r.ok() && p.kind != phys::PenetratorKind::ShapedCharge) {
            if (p.kind == phys::PenetratorKind::LongRod || r.has("rod_material")) {
                const std::string material = r.str("rod_material");
                if (r.ok() && !parseRod(material, e.rodMaterial)) r.fail("rod_material", "unknown '" + material + "'");
            }
            p.uranium = e.rodMaterial == RodMaterial::Uranium;
            p.density = r.optRange("density_kgm3", 1000, 25000, rodMaterialDensity(e.rodMaterial));
            p.diameter = r.range("diameter_mm", 1, 200) * 1e-3;
            p.length = r.range("length_mm", 1, p.kind == phys::PenetratorKind::HeWarhead ? 4000 : 2000) * 1e-3;
            p.mass = p.kind == phys::PenetratorKind::CompactAp ? r.range("mass_kg", 1e-4, 200) : 0;
            if (r.ok() && p.kind == phys::PenetratorKind::LongRod && e.velocity < 300)
                r.fail("velocity_mps", "a long rod needs at least 300 m/s");
        } else if (r.ok()) {
            p.coneDiameter = r.range("cone_diameter_mm", 10, 500) * 1e-3;
            p.standoff = r.range("standoff_cd", 0, 20);
            p.linerDensity = r.optRange("liner_density_kgm3", 1000, 25000, 8930);
            if (r.ok() && !e.hasPublished)
                r.fail("published_rha_mm", "required for shaped charges (the jet is fitted to it)");
        }
        if (const json::Value* shape = item.find("shape"); shape && r.ok()) {
            parseShape(*shape, p, explosives_, r, e.shape);
            if (r.ok() && p.kind == phys::PenetratorKind::ShapedCharge && e.shape.style != phys::ShapeStyle::HeatRocket)
                r.fail("shape.style", "a shaped charge's shape is a heat_rocket");
            if (p.kind != phys::PenetratorKind::ShapedCharge) p.fuze = e.shape.fuze;
            if (p.kind == phys::PenetratorKind::HeWarhead) p.mass = e.shape.massKg;
        }
        if (r.ok() && e.id.empty()) r.fail("id", "empty");
        if (r.ok() && ids.count(e.id)) r.fail("id", "duplicate");
        if (!r.ok()) {
            skip("projectiles.json", entryName(item, i), r.error());
            continue;
        }

        if (p.kind == phys::PenetratorKind::ShapedCharge) {
            p.jetQuality = phys::fitJetQuality(p, e.publishedRha);
            const double model = phys::rhaPenetration(p, 0);
            if (std::fabs(model / e.publishedRha - 1.0) > 0.03) {
                ++report.warnings;
                char buf[200];
                std::snprintf(buf, sizeof(buf),
                              "projectiles.json: %s: jet fit reaches %.0f mm instead of the published %.0f mm (kept)",
                              e.id.c_str(), model * 1e3, e.publishedRha * 1e3);
                report.messages.push_back(buf);
            }
        }
        if (const double parts = shapeCheckMass(e.shape, p); parts > 0 && std::fabs(parts / p.mass - 1) > 0.2) {
            ++report.warnings;
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "projectiles.json: %s: the shape's parts weigh %.3g kg against mass_kg %.3g kg (kept)",
                          e.id.c_str(), parts, p.mass);
            report.messages.push_back(buf);
        }
        ids.insert(e.id);
        projectiles_.push_back(std::move(e));
    }
    return true;
}

int ProjectileDb::findMaterial(std::string_view id) const {
    for (size_t i = 0; i < materials_.size(); ++i)
        if (materials_[i].id == id) return static_cast<int>(i);
    return -1;
}

int ProjectileDb::findExplosive(std::string_view id) const {
    for (size_t i = 0; i < explosives_.size(); ++i)
        if (explosives_[i].id == id) return static_cast<int>(i);
    return -1;
}

int ProjectileDb::findProjectile(std::string_view id) const {
    for (size_t i = 0; i < projectiles_.size(); ++i)
        if (projectiles_[i].id == id) return static_cast<int>(i);
    return -1;
}
