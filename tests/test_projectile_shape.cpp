#include "test.h"

#include "physics/mpm_material.h"
#include "physics/projectile_shape.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace phys;

namespace {

constexpr double kPi = 3.14159265358979323846;

Penetrator pen(PenetratorKind kind, double D, double L) {
    Penetrator p;
    p.kind = kind;
    p.diameter = D;
    p.length = L;
    return p;
}

const ShapePart* findPart(const ProjectileShape& s, PartRole role) {
    for (const ShapePart& part : s.parts)
        if (part.role == role) return &part;
    return nullptr;
}

// Parts ordered nose to base, stations sane, and no two parts overlapping anywhere; returns the
// number of penetrator parts.
int checkParts(const ProjectileShape& s) {
    CHECK(!s.parts.empty());
    int penetrators = 0;
    std::vector<double> xs;
    for (size_t i = 0; i < s.parts.size(); ++i) {
        const ShapePart& part = s.parts[i];
        CHECK(part.stations.size() >= 2u);
        if (i > 0) CHECK(s.parts[i - 1].stations.front().x <= part.stations.front().x);
        if (part.penetrator) ++penetrators;
        for (size_t k = 0; k < part.stations.size(); ++k) {
            const ShapeStation& st = part.stations[k];
            CHECK(st.rInner >= 0 && st.rInner <= st.rOuter + 1e-12);
            if (k > 0) CHECK(st.x >= part.stations[k - 1].x);
            xs.push_back(st.x);
        }
    }
    std::sort(xs.begin(), xs.end());
    int overlaps = 0;
    for (size_t k = 1; k < xs.size(); ++k) {
        if (xs[k] - xs[k - 1] < 1e-12) continue;
        const double x = 0.5 * (xs[k] + xs[k - 1]);
        for (size_t i = 0; i < s.parts.size(); ++i)
            for (size_t j = i + 1; j < s.parts.size(); ++j) {
                double ai, ao, bi, bo;
                if (!partRadiiAt(s.parts[i], x, ai, ao) || !partRadiiAt(s.parts[j], x, bi, bo)) continue;
                if (std::min(ao, bo) - std::max(ai, bi) > 1e-9) ++overlaps;
            }
    }
    CHECK(overlaps == 0);
    return penetrators;
}

// The invariants every built shape keeps: checkParts, one penetrator part, and the engineering
// extent the user's sizes.
void checkCommon(const ProjectileShape& s, const Penetrator& p) {
    CHECK(checkParts(s) == 1);
    CHECK_NEAR(s.penetratorBack - s.penetratorFront, p.length, 1e-9);
    CHECK_NEAR(s.penetratorDiameter, p.diameter, 1e-9);
}

} // namespace

TEST(shape_dart_matches_old_fittings) {
    const Penetrator p = pen(PenetratorKind::LongRod, 0.025, 0.6);
    CHECK(isDartRod(p));
    const ProjectileShape s = shapeForCase(nullptr, p);
    checkCommon(s, p);
    const double D = p.diameter, R = 0.5 * D;
    const ShapePart *cap = findPart(s, PartRole::Cap), *rod = findPart(s, PartRole::Body),
                    *fins = findPart(s, PartRole::Fins), *tracer = findPart(s, PartRole::Tracer);
    CHECK(cap && rod && fins && tracer);
    if (!cap || !rod || !fins || !tracer) return;
    CHECK(rod->penetrator && !cap->penetrator && !fins->penetrator && !tracer->penetrator);
    CHECK_EQ_STR(cap->material, "light_alloy");
    CHECK_EQ_STR(fins->material, "light_alloy");
    // The rod: a plain cylinder behind the cap.
    CHECK_NEAR(s.penetratorFront, kDartCapLength * D, 1e-12);
    CHECK_NEAR(partVolume(*rod), kPi * R * R * p.length, 1e-9 * kPi * R * R * p.length);
    CHECK_NEAR(partArea(*rod), 2 * R * p.length, 1e-12);
    // Fittings: the closed forms of mpm_scene_dart_fittings.
    const double cap0 = kDartCapLength * D;
    const double wall = kDartCapWall * D * std::hypot(1.0, R / cap0);
    const double capExpected = R * cap0 - (R - wall) * (R - wall) * cap0 / R;
    CHECK_NEAR(partArea(*cap), capExpected, 1e-9 * capExpected);
    const double finExpected = (kDartFinRootChord + kDartFinTipChord) * D * (0.5 * kDartFinSpan * D - R);
    CHECK_NEAR(partArea(*fins), finExpected, 1e-9 * finExpected);
    const double tracerExpected = kDartTracerLength * kDartTracerWidth * D * D;
    CHECK_NEAR(partArea(*tracer), tracerExpected, 1e-9 * tracerExpected);
    // Fins: one fin's planform (half the section) x 4 fins x 0.1 D thick.
    CHECK_NEAR(partVolume(*fins), 0.5 * finExpected * 4 * 0.1 * D, 1e-9 * finExpected * D);
    CHECK_NEAR(partMass(*fins, 2800), 2800 * partVolume(*fins), 1e-12);
    // The fins' trailing edge is the rod's tail; the tracer is behind it.
    CHECK_NEAR(fins->stations.back().x, s.penetratorBack, 1e-12);
    CHECK_NEAR(tracer->stations.front().x, s.penetratorBack, 1e-12);
    CHECK_NEAR(s.length, cap0 + p.length + kDartTracerLength * D, 1e-12);
    CHECK_NEAR(s.diameter, kDartFinSpan * D, 1e-12);
}

TEST(shape_ap_legacy_nose) {
    // Generic compact shot: today's elliptical nose, the whole round is the penetrator.
    const Penetrator p = pen(PenetratorKind::CompactAp, 0.1, 0.3);
    const ProjectileShape s = shapeForCase(nullptr, p);
    checkCommon(s, p);
    CHECK(s.parts.size() == 1u);
    const ShapePart& body = s.parts[0];
    CHECK(body.role == PartRole::Body && body.penetrator);
    CHECK_EQ_STR(body.material, "");  // the penetrator's own material
    const double R = 0.05, L = 0.3, Ln = std::min(1.3 * R, 0.5 * L);
    const double expected = 2 * (R * L - (1 - kPi / 4) * R * Ln);
    CHECK_NEAR(partArea(body), expected, 0.01 * expected);
    CHECK(body.stations.size() >= 8u);
    // Generic short rod: flat nose, a cylinder.
    const Penetrator shortRod = pen(PenetratorKind::LongRod, 0.03, 0.09);
    const ProjectileShape c = shapeForCase(nullptr, shortRod);
    checkCommon(c, shortRod);
    CHECK_NEAR(partVolume(c.parts[0]), kPi * 0.015 * 0.015 * 0.09, 1e-12);
    CHECK_NEAR(partArea(c.parts[0]), 2 * 0.015 * 0.09, 1e-12);
    // Tangent ogive of 2 calibres: r(x) = sqrt(rho^2 - (Ln - x)^2) - (rho - R).
    ShapeSpec spec;
    spec.style = ShapeStyle::Ap;
    spec.noseCrh = 2.0;
    spec.band.fromBase = 0.03;
    spec.band.width = 0.02;
    spec.tracer.length = 0.03;
    spec.tracer.diameter = 0.03;
    spec.filler.explosive = "tnt";
    spec.filler.density = 1650;
    spec.filler.diameter = 0.03;
    spec.filler.length = 0.05;
    spec.filler.fromNose = 0.15;
    const ProjectileShape o = buildShape(spec, p);
    checkCommon(o, p);
    const double rho = 2 * 0.1, Lo = std::sqrt(rho * rho - (rho - R) * (rho - R));
    double ri, ro;
    CHECK(partRadiiAt(o.parts[0], 0.5 * Lo, ri, ro));
    CHECK_NEAR(ro, std::sqrt(rho * rho - 0.25 * Lo * Lo) - (rho - R), 1e-3 * R);  // chord between stations
    CHECK(partRadiiAt(o.parts[0], Lo + 0.01, ri, ro));
    CHECK_NEAR(ro, R, 1e-12);
    // APHE: filler cavity with the base fuze directly behind it, a band and a tracer.
    const ShapePart *filler = findPart(o, PartRole::Filler), *fuze = findPart(o, PartRole::Fuze),
                    *band = findPart(o, PartRole::Band), *tracer = findPart(o, PartRole::Tracer);
    CHECK(filler && fuze && band && tracer);
    if (!filler || !fuze || !band || !tracer) return;
    CHECK_EQ_STR(filler->material, "explosive:tnt");
    CHECK_NEAR(filler->density, 1650, 0);
    CHECK_NEAR(filler->stations.front().x, 0.15, 1e-12);
    CHECK_NEAR(fuze->stations.front().x, filler->stations.back().x, 1e-12);
    CHECK_NEAR(fuze->stations.back().x - fuze->stations.front().x, 0.4 * 0.03, 1e-12);
    CHECK_EQ_STR(fuze->material, "gilding_metal");
    CHECK_NEAR(partVolume(*filler), kPi * 0.015 * 0.015 * 0.05, 1e-12);
    CHECK_NEAR(band->stations.front().rOuter, 1.02 * R, 1e-12);
    CHECK_NEAR(band->stations.back().x, L - 0.03, 1e-12);
    CHECK_NEAR(tracer->stations.back().x, L, 1e-12);
    CHECK_NEAR(o.diameter, 2 * 1.02 * R, 1e-12);
}

TEST(shape_apcbc_parts) {
    const Penetrator p = pen(PenetratorKind::CompactAp, 0.088, 0.4);
    ShapeSpec spec;
    spec.style = ShapeStyle::Apcbc;
    spec.noseCrh = 1.5;
    spec.cap.length = 0.06;
    spec.windshield.length = 0.1;
    spec.windshield.wall = 0.003;
    spec.filler.explosive = "tnt";
    spec.filler.density = 1650;
    const ProjectileShape s = buildShape(spec, p);
    checkCommon(s, p);
    const ShapePart *ws = findPart(s, PartRole::Windshield), *cap = findPart(s, PartRole::Cap),
                    *body = findPart(s, PartRole::Body), *filler = findPart(s, PartRole::Filler),
                    *fuze = findPart(s, PartRole::Fuze);
    CHECK(ws && cap && body && filler && fuze);
    if (!ws || !cap || !body || !filler || !fuze) return;
    CHECK(body->penetrator);
    CHECK_EQ_STR(cap->material, "cap_steel");
    CHECK_EQ_STR(ws->material, "light_alloy");
    // The windshield leads; the cap's ogive meets the calibre where the windshield ends; the body's
    // tip sits 0.4 of the way into the cap.
    const double R = 0.044, rho = 1.5 * 0.088, Ln = std::sqrt(rho * rho - (rho - R) * (rho - R));
    CHECK_NEAR(ws->stations.front().x, 0, 1e-12);
    CHECK_NEAR(ws->stations.back().x, 0.1, 1e-12);
    CHECK_NEAR(cap->stations.front().x, 0.1 - Ln, 1e-12);
    CHECK_NEAR(body->stations.front().x, 0.1 - Ln + 0.4 * 0.06, 1e-12);
    CHECK_NEAR(s.penetratorFront, 0, 1e-12);
    CHECK_NEAR(s.penetratorBack, 0.4, 1e-12);
    // The default filler (0.3 D wide, 1 D long) and its fuze.
    CHECK_NEAR(filler->stations.front().rOuter, 0.15 * 0.088, 1e-12);
    CHECK_NEAR(filler->stations.back().x - filler->stations.front().x, 0.088, 1e-12);
    CHECK_NEAR(fuze->stations.front().x, filler->stations.back().x, 1e-12);
    // Parts in order: windshield, cap, body, filler, fuze.
    CHECK(s.parts[0].role == PartRole::Windshield && s.parts[1].role == PartRole::Cap);
    // A cap longer than the round still builds.
    ShapeSpec silly = spec;
    silly.cap.length = 2 * p.length;
    checkCommon(buildShape(silly, p), p);
    // APC without the windshield: the cap starts at the tip.
    ShapeSpec apc = spec;
    apc.style = ShapeStyle::Apc;
    const ProjectileShape a = buildShape(apc, p);
    checkCommon(a, p);
    CHECK(!findPart(a, PartRole::Windshield));
    CHECK_NEAR(a.parts[0].stations.front().x, 0, 1e-12);
    // APBC: windshield over the body.
    ShapeSpec apbc = spec;
    apbc.style = ShapeStyle::Apbc;
    const ProjectileShape b = buildShape(apbc, p);
    checkCommon(b, p);
    CHECK(findPart(b, PartRole::Windshield) && !findPart(b, PartRole::Cap));
}

TEST(shape_apcr_core_in_carrier) {
    const Penetrator p = pen(PenetratorKind::CompactAp, 0.038, 0.12);
    ShapeSpec spec;
    spec.style = ShapeStyle::Apcr;
    const ProjectileShape s = buildShape(spec, p);
    checkCommon(s, p);
    const ShapePart *core = findPart(s, PartRole::Core), *carrier = findPart(s, PartRole::Body);
    CHECK(core && carrier);
    if (!core || !carrier) return;
    CHECK(core->penetrator && !carrier->penetrator);
    CHECK_EQ_STR(carrier->material, "light_alloy");
    CHECK(s.diameter > p.diameter);
    CHECK(s.length > p.length);
    CHECK(carrier->stations.front().x <= core->stations.front().x);
    CHECK_NEAR(s.penetratorFront, core->stations.front().x, 1e-12);
}

TEST(shape_apds_core_only) {
    const Penetrator p = pen(PenetratorKind::CompactAp, 0.04, 0.16);
    ShapeSpec spec;
    spec.style = ShapeStyle::Apds;
    spec.core.material = "tungsten_carbide";
    const ProjectileShape s = buildShape(spec, p);
    checkCommon(s, p);
    CHECK(s.parts.size() == 1u);
    CHECK(s.parts[0].role == PartRole::Core && s.parts[0].penetrator);
    CHECK_EQ_STR(s.parts[0].material, "tungsten_carbide");
    CHECK_NEAR(s.length, 0.16, 1e-12);
    CHECK_NEAR(s.diameter, 0.04, 1e-12);
}

TEST(shape_bullet_layers) {
    const Penetrator p = pen(PenetratorKind::CompactAp, 0.0062, 0.028);
    ShapeSpec spec;
    spec.style = ShapeStyle::Bullet;
    spec.noseCrh = 3.0;
    spec.incendiary.length = 0.004;
    const ProjectileShape s = buildShape(spec, p);
    checkCommon(s, p);
    const ShapePart *jacket = findPart(s, PartRole::Jacket), *sleeve = findPart(s, PartRole::Sleeve),
                    *core = findPart(s, PartRole::Core), *inc = findPart(s, PartRole::Incendiary);
    CHECK(jacket && sleeve && core && inc);
    if (!jacket || !sleeve || !core || !inc) return;
    CHECK(core->penetrator);
    CHECK_EQ_STR(jacket->material, "gilding_metal");
    CHECK_EQ_STR(sleeve->material, "lead");
    CHECK_EQ_STR(inc->material, "incendiary");
    CHECK_NEAR(s.diameter, 1.3 * p.diameter, 1e-12);
    // The jacket's wall on the cylinder, and the incendiary directly ahead of the core.
    double ri, ro;
    CHECK(partRadiiAt(*jacket, s.length - 0.002, ri, ro));
    CHECK_NEAR(ro - ri, 0.07 * 1.3 * p.diameter, 1e-12);
    CHECK_NEAR(inc->stations.back().x, core->stations.front().x, 1e-12);
    CHECK(s.penetratorBack <= s.length);
}

TEST(shape_scales_with_user_size) {
    ShapeSpec cat;
    cat.style = ShapeStyle::Apcbc;
    cat.present = true;
    cat.length = 0.4;
    cat.diameter = 0.088;
    cat.refLength = 0.4;
    cat.refDiameter = 0.088;
    cat.noseCrh = 1.5;
    cat.cap.length = 0.3;
    cat.windshield.length = 0.1;
    cat.band.fromBase = 0.04;
    cat.band.width = 0.02;
    cat.filler.explosive = "tnt";
    cat.filler.density = 1650;
    cat.filler.fromNose = 0.2;
    cat.massKg = 10;
    // The user halves the length (shorter than the catalogue's cap) and widens the round.
    const Penetrator p = pen(PenetratorKind::CompactAp, 0.1, 0.2);
    const ProjectileShape s = shapeForCase(&cat, p);
    checkCommon(s, p);
    CHECK_NEAR(s.length, 0.2, 1e-12);
    CHECK_NEAR(s.diameter, 1.02 * 0.1, 1e-12);
    const ShapePart* ws = findPart(s, PartRole::Windshield);
    CHECK(ws && std::abs(ws->stations.back().x - 0.05) < 1e-12);
    // Unchanged sizes: built as the catalogue says.
    const Penetrator same = pen(PenetratorKind::CompactAp, 0.088, 0.4);
    const ProjectileShape t = shapeForCase(&cat, same);
    checkCommon(t, same);
    CHECK_NEAR(t.length, 0.4, 1e-12);
    CHECK(parseShapeStyle("apcbc") == ShapeStyle::Apcbc);
    CHECK(!parseShapeStyle("nope"));
    CHECK_EQ_STR(shapeStyleId(ShapeStyle::HeMissile), "he_missile");
}

TEST(shape_he_missile_parts) {
    // A missile that hits crumples whole: the nose is a thin shell, the warhead a steel case round its
    // filler, the motor a thin tube closed by a nozzle plate round a bored grain of propellant, and
    // the fins plates on the tube. Every part is seeded; the grain carries the mass the rest does not.
    Penetrator p = pen(PenetratorKind::HeWarhead, 0.07, 1.4);
    p.density = 7850;
    ShapeSpec spec;
    spec.style = ShapeStyle::HeMissile;
    spec.filler.explosive = "comp_b";
    spec.filler.density = 1700;
    spec.fuze.type = FuzeType::NoseContact;
    spec.caseWall = 0.005;
    spec.warheadLength = 0.25;
    spec.noseLength = 0.12;
    spec.massKg = 9.0;
    const ProjectileShape s = buildShape(spec, p);
    checkCommon(s, p);
    for (PartRole r : {PartRole::Nose, PartRole::Fuze, PartRole::Case, PartRole::Filler, PartRole::Motor,
                       PartRole::Propellant, PartRole::Fins})
        CHECK(findPart(s, r) != nullptr);
    CHECK(findPart(s, PartRole::Case)->penetrator);
    double mass = 0;
    int motors = 0;
    for (const ShapePart& part : s.parts) {
        CHECK(part.simulated);
        if (part.role == PartRole::Motor) ++motors;
        double rho = p.density;
        if (!part.material.empty() && part.material.rfind("explosive:", 0) != 0) rho = mpmPartMaterial(part.material)->density;
        mass += partMass(part, rho);
    }
    CHECK(motors == 1);
    CHECK_NEAR(mass, spec.massKg, 0.01 * spec.massKg);
    CHECK(s.warnings.empty());

    // The nose: a shell, hollow behind its tip.
    const ShapePart* nose = findPart(s, PartRole::Nose);
    double nIn = 0, nOut = 0;
    CHECK(partRadiiAt(*nose, 0.08, nIn, nOut) && nIn > 0.5 * nOut && nIn < nOut);

    // The motor: a thin tube, its grain a bored ring filling it, and a nozzle plate at the tail with a
    // throat; the grain stops at the plate.
    const ShapePart* motor = findPart(s, PartRole::Motor);
    const ShapePart* grain = findPart(s, PartRole::Propellant);
    CHECK_EQ_STR(grain->material, "propellant");
    const double mid = 0.5 * (motor->stations.front().x + motor->stations.back().x);
    double mIn = 0, mOut = 0, gIn = 0, gOut = 0;
    CHECK(partRadiiAt(*motor, mid, mIn, mOut) && partRadiiAt(*grain, mid, gIn, gOut));
    CHECK_NEAR(mOut, 0.035, 1e-9);
    CHECK(mOut - mIn > 0 && mOut - mIn < 0.1 * mOut);
    CHECK_NEAR(gOut, mIn, 1e-9);
    CHECK(gIn > 0 && gIn < gOut);
    const ShapeStation& tail = motor->stations.back();
    CHECK(tail.rInner > 0 && tail.rInner < mIn);
    CHECK(grain->stations.back().x < tail.x);
    CHECK_NEAR(grain->stations.front().x, motor->stations.front().x, 1e-9);

    // A round heavier than its parts and a full grain, or lighter than its parts, says so.
    spec.massKg = 40.0;
    CHECK(!buildShape(spec, p).warnings.empty());
    spec.massKg = 0.05;
    const ProjectileShape light = buildShape(spec, p);
    CHECK(!light.warnings.empty());
    CHECK(findPart(light, PartRole::Propellant) == nullptr);
}

TEST(shape_heat_rocket_parts) {
    // A HEAT rocket: the hollow nose gives the built-in standoff, the cone liner (what the jet model
    // describes) sits in the warhead's explosive, a burned-out motor tube and fins behind it.
    Penetrator p = pen(PenetratorKind::ShapedCharge, 0.025, 0.6);
    p.coneDiameter = 0.072;
    p.standoff = 1.6;
    ShapeSpec spec;
    spec.style = ShapeStyle::HeatRocket;
    spec.filler.explosive = "comp_a5";
    spec.filler.density = 1650;
    const ProjectileShape s = buildShape(spec, p);
    CHECK(checkParts(s) == 1);
    for (PartRole r : {PartRole::Nose, PartRole::Liner, PartRole::Filler, PartRole::Case, PartRole::Motor, PartRole::Fins})
        CHECK(findPart(s, r) != nullptr);
    for (const ShapePart& part : s.parts) CHECK(part.simulated);
    const ShapePart* liner = findPart(s, PartRole::Liner);
    CHECK(liner && liner->penetrator);
    if (!liner) return;
    // The cone's mouth (full radius) faces forward at the standoff; its apex is one cone diameter behind.
    CHECK_NEAR(s.penetratorFront, 1.6 * 0.072, 1e-9);
    CHECK_NEAR(s.penetratorBack, s.penetratorFront + 0.072, 1e-9);
    CHECK_NEAR(s.penetratorDiameter, 0.072, 1e-9);
    CHECK_NEAR(liner->stations.front().rInner, 0.036, 1e-6);
    CHECK(liner->stations.back().rInner < 1e-9 && liner->stations.back().rOuter < 0.01);
    CHECK(s.diameter > 0.072);
    const ShapePart* filler = findPart(s, PartRole::Filler);
    CHECK(filler && filler->material == "explosive:comp_a5" && filler->density == 1650);
    CHECK(filler && filler->stations.back().x > s.penetratorBack);  // explosive behind the apex too
    CHECK(s.warnings.empty());
    CHECK(parseShapeStyle("heat_rocket") == ShapeStyle::HeatRocket);
    CHECK_EQ_STR(partRoleId(PartRole::Liner), "liner");

    // No built-in standoff: no nose, the cone's mouth at the front.
    p.standoff = 0;
    const ProjectileShape flat = buildShape(spec, p);
    CHECK(checkParts(flat) == 1);
    CHECK(findPart(flat, PartRole::Nose) == nullptr);
    CHECK_NEAR(flat.penetratorFront, 0, 1e-9);

    // A shaped charge without a catalogue shape gets the default rocket around its cone.
    p.standoff = 2;
    const ProjectileShape def = shapeForCase(nullptr, p);
    CHECK(checkParts(def) == 1);
    CHECK(findPart(def, PartRole::Liner) != nullptr && findPart(def, PartRole::Filler) != nullptr);
    CHECK_NEAR(def.penetratorFront, 2 * 0.072, 1e-9);
    CHECK_NEAR(def.penetratorDiameter, 0.072, 1e-9);
}
