#include "physics/penetration.h"
#include "test.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace phys;

namespace {

ArmourLayer steel(double thickness, double bhn = 300, double gap = 0) {
    ArmourLayer l;
    l.material.density = kRhaDensity;
    l.material.hardnessBhn = bhn;
    l.material.resistance = steelResistanceFromBhn(bhn);
    l.thickness = thickness;
    l.gap = gap;
    return l;
}

Penetrator rod(double diameter, double lengthOverDiameter, bool uranium = false) {
    Penetrator p;
    p.kind = PenetratorKind::LongRod;
    p.diameter = diameter;
    p.length = diameter * lengthOverDiameter;
    p.uranium = uranium;
    p.density = uranium ? 18600 : 17600;
    return p;
}

Penetrator shot(double diameter, double mass, double length) {
    Penetrator p;
    p.kind = PenetratorKind::CompactAp;
    p.diameter = diameter;
    p.mass = mass;
    p.length = length;
    p.density = 7850;
    return p;
}

}  // namespace

TEST(tate_hydrodynamic_limit) {
    // Without strength the rod penetrates L * sqrt(rho_p / rho_t) (times the L/D factor).
    Penetrator p = rod(0.02, 20);
    p.strength = 0;
    ArmourLayer target = steel(10.0);
    target.material.resistance = 0;
    const double depth = simulatePenetration(p, 1500, 0, {target}).depth;
    const double expected = lengthFactor(20) * p.length * std::sqrt(p.density / kRhaDensity);
    CHECK_NEAR(depth / expected, 1.0, 0.01);
}

TEST(tate_matches_lanz_odermatt) {
    for (int du = 0; du <= 1; ++du) {
        double worst = 0;
        for (double bhn : {250.0, 300.0, 400.0, 500.0})
            for (double v : {1300.0, 1600.0, 1900.0})
                for (double ld : {10.0, 20.0, 30.0})
                    for (double theta : {0.0, 30.0, 60.0}) {
                        const Penetrator p = rod(0.02, ld, du == 1);
                        const double depth = simulatePenetration(p, v, theta, {steel(10.0, bhn)}).depth;
                        const double lo = lanzOdermattRatio(du == 1, ld, v, p.density, kRhaDensity, bhn, theta) * p.length;
                        worst = std::max(worst, std::fabs(depth / lo - 1.0));
                    }
        CHECK(worst < (du == 1 ? 0.12 : 0.06));
    }
}

TEST(lanz_odermatt_reference_values) {
    // Tungsten, L/D 30, 1.7 km/s, BHN 300: P/L = 0.894; DU: 0.906.
    CHECK_NEAR(lanzOdermattRatio(false, 30, 1700, 17600, 7850, 300, 0), 0.894, 0.003);
    CHECK_NEAR(lanzOdermattRatio(true, 30, 1700, 18600, 7850, 300, 0), 0.906, 0.003);
}

TEST(tate_obliquity_line_of_sight) {
    const Penetrator p = rod(0.025, 25);
    const double d0 = simulatePenetration(p, 1650, 0, {steel(10.0)}).depth;
    const double d60 = simulatePenetration(p, 1650, 60, {steel(10.0)}).depth;
    CHECK_NEAR(d60 / d0, std::pow(0.5, -0.224), 0.02);
}

TEST(rod_perforation_and_residuals) {
    const Penetrator p = rod(0.025, 30);
    const double semi = rhaPenetration(p, 1650);
    const PenetrationResult thin = simulatePenetration(p, 1650, 0, {steel(0.5 * semi)});
    CHECK(thin.perforated);
    CHECK(thin.residualVelocity > 0 && thin.residualVelocity < 1650);
    CHECK(thin.residualLength > 0 && thin.residualLength < p.length);
    CHECK(thin.layers[0].perforated);
    const PenetrationResult thick = simulatePenetration(p, 1650, 0, {steel(1.2 * semi)});
    CHECK(!thick.perforated);
    CHECK_NEAR(thick.depth, semi, 0.01 * semi);
    CHECK_NEAR(thick.layers[0].depth, semi, 0.01 * semi);
    // Depth into a witness block behind a thin plate is less than the full semi-infinite depth.
    const double dop = residualPenetration(p, 1650, 0, {steel(0.5 * semi)});
    CHECK(dop > 0.2 * semi && dop < semi);
    // History is monotonic in time and depth.
    bool ok = true;
    for (size_t i = 1; i < thick.history.size(); ++i)
        ok = ok && thick.history[i].time >= thick.history[i - 1].time && thick.history[i].depth >= thick.history[i - 1].depth - 1e-12;
    CHECK(ok);
}

TEST(rod_ballistic_limit) {
    const Penetrator p = rod(0.022, 30);
    const std::vector<ArmourLayer> stack = {steel(0.3), steel(0.2, 300, 0.3)};
    const double vbl = ballisticLimit(p, 0, stack);
    CHECK(vbl > 500 && vbl < 3000);
    CHECK(simulatePenetration(p, vbl + 5, 0, stack).perforated);
    CHECK(!simulatePenetration(p, vbl - 5, 0, stack).perforated);
    const double thicker = ballisticLimit(p, 0, {steel(0.35), steel(0.2, 300, 0.3)});
    CHECK(thicker > vbl);
    CHECK(ballisticLimit(p, 0, {steel(5.0)}) < 0);  // not perforable below 3 km/s
}

TEST(demarre_and_lambert_jonas) {
    // 88 mm, 10.2 kg shot at 773 m/s: about 127 mm of RHA at 0 deg.
    const Penetrator p88 = shot(0.088, 10.2, 0.36);
    CHECK_NEAR(rhaPenetration(p88, 773), 0.127, 0.004);
    CHECK_NEAR(deMarreLimitVelocity(rhaPenetration(p88, 773), 0.088, 10.2), 773, 0.5);
    // Residual velocity is continuous at the limit and increases with impact velocity.
    CHECK(lambertJonasResidual(700, 700, 2.4, 0.9) == 0);
    CHECK(lambertJonasResidual(700.7, 700, 2.4, 0.9) < 0.1 * 700);
    double prev = 0;
    bool monotonic = true;
    for (double v = 710; v < 2000; v += 10) {
        const double vr = lambertJonasResidual(v, 700, 2.4, 0.9);
        monotonic = monotonic && vr > prev && vr < v;
        prev = vr;
    }
    CHECK(monotonic);
}

TEST(compact_ap_stack) {
    const Penetrator p = shot(0.088, 10.2, 0.36);
    const std::vector<ArmourLayer> mono = {steel(0.08)};
    const double vMono = ballisticLimit(p, 0, mono);
    CHECK_NEAR(vMono, deMarreLimitVelocity(0.08, 0.088, 10.2), 1.0);
    const PenetrationResult r = simulatePenetration(p, vMono * 1.2, 0, mono);
    CHECK(r.perforated);
    CHECK(r.residualVelocity > 0 && r.residualVelocity < vMono * 1.2);
    const PenetrationResult s = simulatePenetration(p, vMono * 0.8, 0, mono);
    CHECK(!s.perforated);
    // It stops where a plate that thick would just have been perforated (De Marre inverted).
    CHECK_NEAR(s.depth, 0.08 * std::pow(0.8, 1 / 0.7), 1e-4);
    // Touching layers act as one plate; the same steel split by a gap is easier to perforate.
    CHECK_NEAR(ballisticLimit(p, 0, {steel(0.04), steel(0.04)}), vMono, 1.0);
    CHECK(ballisticLimit(p, 0, {steel(0.04), steel(0.04, 300, 0.2)}) < vMono);
    // Obliquity raises the limit velocity.
    CHECK(ballisticLimit(p, 45, mono) > vMono);
}

TEST(compact_ap_depth_independent_of_block_thickness) {
    // A thick block is semi-infinite to a shot that stops in it: .30-06 M2 AP (6.2 mm, 5.2 g
    // core) at 850 m/s digs about a centimetre into 100, 300 or 600 mm of RHA alike.
    const Penetrator p = shot(0.0062, 0.0052, 0.027);
    const double semi = rhaPenetration(p, 850);
    CHECK(semi > 0.007 && semi < 0.016);
    for (double block : {0.1, 0.3, 0.6}) {
        const PenetrationResult r = simulatePenetration(p, 850, 0, {steel(block)});
        CHECK(!r.perforated);
        CHECK_NEAR(r.depth, semi, 0.01 * semi);
        CHECK_NEAR(r.history.back().depth, r.depth, 0.01 * semi);
    }
    // At obliquity the model's plate is its line-of-sight thickness: the shot runs the same path.
    const PenetrationResult r60 = simulatePenetration(p, 850, 60, {steel(0.6)});
    CHECK_NEAR(r60.depth, semi, 0.02 * semi);
}

TEST(jet_continuous_limit) {
    // Continuous jet, no target strength: P = S ((v_tip / v_min)^(1/gamma) - 1).
    JetParameters j{};
    j.tipVelocity = 8000;
    j.minVelocity = 2000;
    j.breakupTime = 1.0;
    j.drift = 0;
    j.virtualOrigin = 0.3;
    j.craterRadius = 0.01;
    j.jetRadius = 0.002;
    ArmourLayer target = steel(50.0);
    target.material.resistance = 0;
    const double gamma = std::sqrt(kRhaDensity / 8930.0);
    const double expected = 0.3 * (std::pow(4.0, 1.0 / gamma) - 1.0);
    CHECK_NEAR(simulateJet(j, 8930, 0.1, 0, {target}).depth / expected, 1.0, 0.01);
}

TEST(jet_particulated_limit) {
    // Fully particulated jet: P = t_b (v_tip - v_min) / gamma.
    JetParameters j{};
    j.tipVelocity = 8000;
    j.minVelocity = 2000;
    j.breakupTime = 50e-6;
    j.drift = 0;
    j.virtualOrigin = 10.0;
    j.craterRadius = 0.01;
    j.jetRadius = 0.002;
    ArmourLayer target = steel(50.0);
    target.material.resistance = 0;
    const double gamma = std::sqrt(kRhaDensity / 8930.0);
    CHECK_NEAR(simulateJet(j, 8930, 0.1, 0, {target}).depth / (50e-6 * 6000.0 / gamma), 1.0, 0.01);
}

TEST(jet_standoff_curve_and_fit) {
    Penetrator p;
    p.kind = PenetratorKind::ShapedCharge;
    p.coneDiameter = 0.085;
    p.standoff = 2.2;
    p.jetQuality = fitJetQuality(p, 0.5);
    CHECK_NEAR(rhaPenetration(p, 0), 0.5, 0.01);
    // Penetration rises with standoff to an optimum, then falls as the particles drift.
    Penetrator near = p, mid = p, far = p;
    near.standoff = 1;
    mid.standoff = 6;
    far.standoff = 25;
    CHECK(rhaPenetration(mid, 0) > rhaPenetration(near, 0));
    CHECK(rhaPenetration(far, 0) < rhaPenetration(mid, 0));
    // A jet that perforates a plate leaves a residual jet that still penetrates a witness block.
    const PenetrationResult r = simulatePenetration(p, 0, 0, {steel(0.2)});
    CHECK(r.perforated);
    CHECK(r.residualVelocity > 2000 && r.residualVelocity < 9000);
    const double dop = residualPenetration(p, 0, 0, {steel(0.2)});
    CHECK(dop > 0.1 && dop < 0.35);
    // Softer, lighter material stops less jet per unit thickness than steel.
    ArmourLayer al;
    al.material.density = 2660;
    al.material.resistance = 1.7e9;
    al.thickness = 0.2;
    CHECK(residualPenetration(p, 0, 0, {al}) > dop);
}

TEST(he_warhead_arrivals_only) {
    Penetrator p;
    p.kind = PenetratorKind::HeWarhead;
    p.diameter = 0.07;
    p.length = 1.4;
    const std::vector<ArmourLayer> layers = {steel(0.001), steel(0.1, 300, 0.3)};
    const PenetrationResult r = simulatePenetration(p, 700, 0, layers);
    CHECK(r.history.size() == 2u);
    CHECK(r.layers.size() == 2u);
    CHECK(!r.perforated);
    CHECK_NEAR(r.layers[1].front, 0.301, 1e-9);
    for (size_t i = 0; i < r.history.size(); ++i) {
        CHECK_NEAR(r.history[i].velocity, 700, 1e-9);
        CHECK_NEAR(r.history[i].depth, r.layers[i].front, 1e-9);
        CHECK_NEAR(r.history[i].time, r.layers[i].front / 700, 1e-12);
    }
    CHECK(!std::string(penetratorKindLabel(PenetratorKind::HeWarhead)).empty());
    CHECK(std::string(penetratorKindId(PenetratorKind::HeWarhead)) == "he_warhead");
}

TEST(he_warhead_end_time_clamped) {
    Penetrator p;
    p.kind = PenetratorKind::HeWarhead;
    p.diameter = 0.07;
    p.fuze.type = FuzeType::NoseContact;
    p.fuze.fillerLength = 0.1;
    p.fuze.detonationSpeed = 7000;
    const std::vector<ArmourLayer> layers = {steel(0.001), steel(0.1, 300, 0.3)};
    const double last = 0.301 / 700;
    PenetrationResult r = simulatePenetration(p, 700, 0, layers);
    CHECK_NEAR(r.endTime, last + 10 * 0.1 / 7000 + 150e-6 * 0.7, 1e-9);
    p.diameter = 0.001;
    p.fuze.fillerLength = 0.001;
    r = simulatePenetration(p, 700, 0, layers);
    CHECK_NEAR(r.endTime, last + 100e-6, 1e-9);
    p.diameter = 2;
    p.fuze.fillerLength = 2;
    r = simulatePenetration(p, 700, 0, layers);
    CHECK_NEAR(r.endTime, last + 1e-3, 1e-9);
}
