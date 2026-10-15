#include "physics/mpm_material.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phys {

const char* mpmModelId(MpmModel m) {
    switch (m) {
    case MpmModel::Ceramic: return "ceramic";
    case MpmModel::Elastomer: return "elastomer";
    case MpmModel::Composite: return "composite";
    default: return "metal";
    }
}

MpmMaterial mpmTungstenAlloy() {
    // 90-93 % W heavy alloy (Johnson-Cook fits after Johnson & Cook 1983 and later work).
    MpmMaterial m;
    m.density = 17600;
    m.shear = 160e9;
    m.bulk = 285e9;
    m.yield = 1.5e9;
    m.hardening = 0.18e9;
    m.hardeningExp = 0.12;
    m.softeningExp = 1.0;
    m.meltK = 1723;
    m.heat = 134;
    m.failureStrain = 1.0;
    m.spall = 3.5e9;
    return m;
}

MpmMaterial mpmDepletedUranium() {
    // U-0.75Ti.
    MpmMaterial m;
    m.density = 18600;
    m.shear = 71e9;
    m.bulk = 99e9;
    m.yield = 1.08e9;
    m.hardening = 1.12e9;
    m.hardeningExp = 0.25;
    m.softeningExp = 1.0;
    m.meltK = 1473;
    m.heat = 117;
    m.failureStrain = 0.5;
    m.spall = 3.0e9;
    return m;
}

MpmMaterial mpmPenetratorSteel() {
    // Hardened steel core.
    MpmMaterial m;
    m.density = 7850;
    m.shear = 80e9;
    m.bulk = 160e9;
    m.yield = 1.5e9;
    m.hardening = 0.5e9;
    m.hardeningExp = 0.26;
    m.softeningExp = 1.03;
    m.meltK = 1793;
    m.heat = 477;
    m.failureStrain = 1.0;
    m.spall = 3.5e9;
    return m;
}

MpmMaterial mpmLightAlloy() {
    // High-strength aluminium alloy.
    MpmMaterial m;
    m.density = 2800;
    m.shear = 27e9;
    m.bulk = 70e9;
    m.yield = 0.45e9;
    m.hardening = 0.4e9;
    m.hardeningExp = 0.4;
    m.softeningExp = 1.0;
    m.meltK = 900;
    m.heat = 900;
    m.failureStrain = 0.3;
    m.spall = 1.0e9;
    return m;
}

MpmMaterial mpmLinerCopper() {
    // OFHC-like copper of a shaped-charge jet (Johnson-Cook after Johnson & Cook 1983). The
    // failure strain is high and the spall stress out of reach: the jet's stretching and the
    // plane-strain sheet's lateral rebound alone must not fail it (its breakup is programmed,
    // see the MPM scene).
    MpmMaterial m;
    m.density = 8960;
    m.shear = 46e9;
    m.bulk = 137e9;
    m.yield = 90e6;
    m.hardening = 292e6;
    m.hardeningExp = 0.31;
    m.softeningExp = 1.09;
    m.meltK = 1356;
    m.heat = 383;
    m.failureStrain = 5.0;
    m.spall = 1.0e12;
    return m;
}

namespace {

// A metal part (rho kg/m3; G, K, A, B GPa; JC n, m; melt K; c_p J/(kg K); failure strain; spall GPa).
MpmMaterial partMetal(double rho, double g, double k, double a, double b, double n, double m, double melt, double cp,
                      double failure, double spall) {
    MpmMaterial p;
    p.density = rho;
    p.shear = g * 1e9;
    p.bulk = k * 1e9;
    p.yield = a * 1e9;
    p.hardening = b * 1e9;
    p.hardeningExp = n;
    p.softeningExp = m;
    p.meltK = melt;
    p.heat = cp;
    p.failureStrain = failure;
    p.spall = spall * 1e9;
    return p;
}

// `m` made brittle: crack strength in GPa and Weibull modulus.
MpmMaterial brittle(MpmMaterial m, double strengthGpa, double modulus) {
    m.brittleStress = strengthGpa * 1e9;
    m.flawModulus = modulus;
    return m;
}

}  // namespace

std::optional<MpmMaterial> mpmPartMaterial(std::string_view id) {
    if (id == "gilding_metal") return partMetal(8860, 44, 140, 0.10, 0.29, 0.31, 1.09, 1300, 385, 1.0, 1.5);
    if (id == "lead") return partMetal(11340, 5.6, 46, 0.02, 0.03, 0.5, 1.0, 600, 129, 1.5, 0.5);
    // Hard parts crack instead of mushrooming: brittle(...) sets the crack strength (GPa) and
    // the Weibull modulus of the flaws it starts from (carbide the widest spread).
    if (id == "cap_steel") return brittle(partMetal(7850, 80, 160, 0.6, 0.5, 0.26, 1.03, 1793, 477, 0.6, 2.5), 2.0, 12);
    if (id == "tungsten_carbide")
        return brittle(partMetal(14900, 250, 380, 3.0, 0, 1, 1, 3000, 200, 0.02, 1.5), 1.2, 6);
    if (id == "mild_steel") return partMetal(7850, 80, 160, 0.35, 0.275, 0.36, 1.0, 1793, 477, 1.0, 1.5);
    if (id == "case_steel") return brittle(partMetal(7850, 80, 160, 0.79, 0.51, 0.26, 1.03, 1793, 477, 0.35, 2.5), 2.2, 14);
    if (id == "incendiary") return partMetal(1900, 1, 5, 0.02, 0, 1, 1, 600, 1000, 2, 0.05);
    if (id == "core_steel") return brittle(mpmPenetratorSteel(), 2.6, 16);
    if (id == "tungsten_alloy") return mpmTungstenAlloy();
    if (id == "du") return mpmDepletedUranium();
    if (id == "light_alloy") return mpmLightAlloy();
    // A composite propellant grain (AP/aluminium in a rubber binder): dense but weak, so it is a
    // crumbly block that cracks at tens of MPa and flows plastically under the impact's load.
    if (id == "propellant") return brittle(partMetal(1700, 0.5, 4, 0.01, 0, 1, 1, 600, 1500, 0.15, 0.02), 0.045, 6);
    return std::nullopt;
}

MpmMaterial mpmFiller(double density, double detonationSpeed, double energy) {
    MpmMaterial m = partMetal(density, 0.5, 8, 0.03, 0, 1, 1, 450, 1300, 2, 0.05);
    m.detonationSpeed = detonationSpeed;
    m.energy = energy;
    m.productsGamma = 3;
    m.initiation = 1e30;
    return m;
}

namespace {

// 32-bit integer hash of two ids (shaders/mpm/g2p.comp flawFactor does the same arithmetic).
uint32_t flawHash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u + b * 0x85EBCA6Bu + 0x68E31DA4u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

}  // namespace

double mpmFlawFactor(uint32_t a, uint32_t b, double modulus) {
    const double u = ((flawHash(a, b) >> 9) + 0.5) / 8388608.0;  // (0, 1), 23 bits so a float holds it exactly
    const double invM = 1.0 / std::max(modulus, 1.0);
    // Weibull draw, scaled so that its mean is one.
    return std::max(std::pow(-std::log(1.0 - u), invM) / std::tgamma(1.0 + invM), kMinFlawFactor);
}

double mpmSoundSpeed(const MpmMaterial& m) { return std::sqrt((m.bulk + 4.0 / 3.0 * m.shear) / m.density); }

double mpmSignalSpeed(const MpmMaterial& m) {
    double c = mpmSoundSpeed(m);
    if (m.model == MpmModel::Composite)
        c = std::sqrt((m.bulk + 4.0 / 3.0 * m.shear + m.fibreModulus) / m.density);
    if (mpmReactive(m)) {
        const double g = m.productsGamma;
        c = std::max({c, m.detonationSpeed, 1.5 * std::sqrt(g * (g - 1) * m.energy)});
    }
    return c;
}

double mpmViscousStep(const MpmMaterial& m, double dx) {
    if (m.model != MpmModel::Elastomer || !(m.viscosity > 0)) return std::numeric_limits<double>::infinity();
    return 0.25 * m.density * dx * dx / m.viscosity;
}

MpmHelState mpmHelState(const MpmMaterial& m) {
    MpmHelState h;
    h.strain = m.hel / (m.bulk + 4.0 / 3.0 * m.shear);
    h.pressure = m.bulk * h.strain;
    h.strength = 2 * m.shear * h.strain;
    return h;
}

double mpmProductsPressure(const MpmMaterial& m, double burn, double J) {
    return std::clamp(burn, 0.0, 1.0) * (m.productsGamma - 1) * m.density * m.energy *
           std::pow(std::max(J, 1e-3), -m.productsGamma);
}

double mpmProductsTemperature(const MpmMaterial& m, double burn, double J) {
    return kRoomTemperature +
           std::clamp(burn, 0.0, 1.0) * m.energy / m.heat * std::pow(std::max(J, 1e-3), -0.25);
}

double mpmYieldStress(const MpmMaterial& m, double plasticStrain, double temperature) {
    const double t = std::clamp((temperature - kRoomTemperature) / (m.meltK - kRoomTemperature), 0.0, 1.0);
    const double hardening = m.hardening * std::pow(std::max(plasticStrain, 0.0), m.hardeningExp);
    return (m.yield + hardening) * (1.0 - std::pow(t, m.softeningExp));
}

void mpmKirchhoff(const MpmMaterial& m, const double e[3], double s[3]) {
    const double tr = e[0] + e[1] + e[2];
    for (int i = 0; i < 3; ++i) s[i] = 2 * m.shear * (e[i] - tr / 3) + m.bulk * tr;
}

double mpmVonMises(const double s[3]) {
    const double a = s[0] - s[1], b = s[1] - s[2], c = s[2] - s[0];
    return std::sqrt(0.5 * (a * a + b * b + c * c));
}

double mpmCeramicStrength(const MpmMaterial& m, double pressure, double fracture) {
    const MpmHelState h = mpmHelState(m);
    const double intact =
        pressure + m.tensile > 0 ? h.strength * m.intactA * std::pow((pressure + m.tensile) / h.pressure, m.intactN) : 0.0;
    const double fractured = pressure > 0 ? std::min(h.strength * m.fracturedB * std::pow(pressure / h.pressure, m.fracturedM),
                                                     m.fracturedMax * h.strength)
                                          : 0.0;
    const double d = std::clamp(fracture, 0.0, 1.0);
    return intact - d * (intact - fractured);
}

double mpmCeramicFractureStrain(const MpmMaterial& m, double pressure) {
    const MpmHelState h = mpmHelState(m);
    const double s = std::max(pressure + m.tensile, 0.0) / h.pressure;
    return std::max(m.fractureD1 * std::pow(s, m.fractureD2), kMinCeramicFracture);
}

void mpmElastomerKirchhoff(const MpmMaterial& m, const double e[3], double s[3]) {
    const double tr = e[0] + e[1] + e[2];
    double stretch2[3], mean = 0;
    for (int i = 0; i < 3; ++i) {
        stretch2[i] = std::exp(2 * e[i]);
        mean += stretch2[i] / 3;
    }
    const double scale = m.shear * std::exp(-2.0 / 3.0 * tr);
    for (int i = 0; i < 3; ++i) s[i] = scale * (stretch2[i] - mean) + m.bulk * tr;
}

double mpmViscousStress(const MpmMaterial& m, const double L[2][2], double J, double tau[4]) {
    const double third = (L[0][0] + L[1][1]) / 3;
    const double d[4] = {L[0][0] - third, L[1][1] - third, 0.5 * (L[0][1] + L[1][0]), -third};
    for (int i = 0; i < 4; ++i) tau[i] = 2 * m.viscosity * J * d[i];
    const double vonMises =
        std::sqrt(1.5 * (tau[0] * tau[0] + tau[1] * tau[1] + tau[3] * tau[3] + 2 * tau[2] * tau[2]));
    if (m.viscousLimit > 0 && vonMises > m.viscousLimit)
        for (int i = 0; i < 4; ++i) tau[i] *= m.viscousLimit / vonMises;
    return tau[0] * d[0] + tau[1] * d[1] + 2 * tau[2] * d[2] + tau[3] * d[3];
}

namespace {

// Radial return of the deviatoric strains onto von Mises `strength`; returns the plastic strain.
double radialReturn(const MpmMaterial& m, double dev[3], double strength) {
    const double norm = std::sqrt(dev[0] * dev[0] + dev[1] * dev[1] + dev[2] * dev[2]);
    // von Mises stress of the trial state: sqrt(3/2) * 2G * |dev|.
    const double trial = std::sqrt(1.5) * 2 * m.shear * norm;
    if (trial <= strength || norm <= 0) return 0;
    const double target = strength / (std::sqrt(1.5) * 2 * m.shear);
    for (int i = 0; i < 3; ++i) dev[i] *= target / norm;
    return std::sqrt(2.0 / 3.0) * (norm - target);
}

// The ceramic branch of mpmReturnMap (JH-2).
double ceramicReturnMap(const MpmMaterial& m, MpmPoint& p, double dev[3], double& tr) {
    double deltaPlastic = 0;
    if (!p.failed) {
        if (p.comminuted) tr = std::min(tr, 0.0);
        const double pressure = -m.bulk * tr;
        if (pressure < -m.tensile * (1 - p.fracture)) p.fracture = 1;
        if (p.fracture >= 1) {
            p.comminuted = true;
            tr = std::min(tr, 0.0);
        }
        const double pc = -m.bulk * tr;
        const double strength = mpmCeramicStrength(m, pc, p.fracture);
        deltaPlastic = radialReturn(m, dev, strength);
        if (deltaPlastic > 0) {
            p.plasticStrain += deltaPlastic;
            p.temperature += kTaylorQuinney * strength * deltaPlastic / (m.density * m.heat);
            if (!p.comminuted) {
                p.fracture = std::min(p.fracture + deltaPlastic / mpmCeramicFractureStrain(m, pc), 1.0);
                if (p.fracture >= 1) p.comminuted = true;
            }
        }
        if (p.temperature >= m.meltK) p.failed = true;
    }
    if (p.failed) {
        dev[0] = dev[1] = dev[2] = 0;
        tr = std::min(tr, 0.0);
    }
    return deltaPlastic;
}

// The elastomer branch of mpmReturnMap: no plasticity, only the failure rule.
void elastomerFailure(const MpmMaterial& m, MpmPoint& p, double dev[3], double& tr) {
    if (!p.failed) {
        const double largest = std::max({p.strain[0], p.strain[1], p.strain[2]});
        const double mean = m.bulk * tr;  // Kirchhoff mean stress, tension positive
        if (std::exp(largest) - 1 > m.failureStrain || mean > m.spall || p.temperature >= m.meltK) p.failed = true;
    }
    if (p.failed) {
        dev[0] = dev[1] = dev[2] = 0;
        tr = std::min(tr, 0.0);
    }
}

// The composite branch of mpmReturnMap: the metal return map for the matrix, whose failure
// strain and spall stress delaminate it rather than fail it. Delaminated, the strains are
// along the fibres, across the plies and out of plane (g2p projects them into that frame and
// drops the ply shear): each sheet still yields, and tension across the plies is relaxed.
double compositeReturnMap(const MpmMaterial& m, MpmPoint& p, double dev[3], double& tr) {
    double deltaPlastic = 0;
    auto yieldMatrix = [&] {
        const double yield = mpmYieldStress(m, p.plasticStrain, p.temperature);
        const double dp = radialReturn(m, dev, yield);
        if (dp > 0) {
            p.plasticStrain += dp;
            p.temperature += kTaylorQuinney * yield * dp / (m.density * m.heat);
        }
        deltaPlastic += dp;
    };
    if (!p.failed && !p.delaminated) {
        yieldMatrix();
        const double mean = m.bulk * tr;  // Kirchhoff mean stress, tension positive
        if (p.plasticStrain > m.failureStrain || mean > m.spall) p.delaminated = true;
    }
    if (p.temperature >= m.meltK || (p.delaminated && p.fibreBroken)) p.failed = true;
    if (!p.failed && p.delaminated) {
        yieldMatrix();
        // Open: no tension across the plies (2 G e_nn + (K - 2 G / 3) tr = 0).
        if (2 * m.shear * dev[1] + m.bulk * tr > 0) {
            const double along = dev[0] + tr / 3, out = dev[2] + tr / 3;
            const double across = -(m.bulk - 2 * m.shear / 3) * (along + out) / (m.bulk + 4 * m.shear / 3);
            tr = along + across + out;
            dev[0] = along - tr / 3;
            dev[1] = across - tr / 3;
            dev[2] = out - tr / 3;
        }
    }
    if (p.failed) {
        dev[0] = dev[1] = dev[2] = 0;
        tr = std::min(tr, 0.0);
    }
    return deltaPlastic;
}

}  // namespace

void mpmFibreStress(const MpmMaterial& m, const double f[2], bool broken, double tau[4]) {
    tau[0] = tau[1] = tau[2] = tau[3] = 0;
    const double stretch = std::hypot(f[0], f[1]);
    if (broken || stretch <= 1) return;
    const double s = m.fibreModulus * std::log(stretch) / (stretch * stretch);
    tau[0] = s * f[0] * f[0];
    tau[1] = s * f[1] * f[1];
    tau[2] = s * f[0] * f[1];
}

bool mpmFibresBreak(const MpmMaterial& m, const double f[2]) {
    return std::log(std::max(std::hypot(f[0], f[1]), 1e-12)) > m.fibreFailureStrain;
}

bool mpmDelaminates(const MpmMaterial& m, const double tau[4], double J, const double f[2]) {
    const double len = std::max(std::hypot(f[0], f[1]), 1e-12);
    const double fx = f[0] / len, fy = f[1] / len, nx = -fy, ny = fx;
    const double across = (tau[0] * nx * nx + tau[1] * ny * ny + 2 * tau[2] * nx * ny) / J;
    const double shear = (tau[0] * fx * nx + tau[1] * fy * ny + tau[2] * (fx * ny + fy * nx)) / J;
    return across > m.delamTension || std::abs(shear) > m.delamShear;
}

double mpmReturnMap(const MpmMaterial& m, MpmPoint& p) {
    double* e = p.strain;
    double tr = e[0] + e[1] + e[2];
    double dev[3] = {e[0] - tr / 3, e[1] - tr / 3, e[2] - tr / 3};
    if (m.model == MpmModel::Ceramic) {
        const double deltaPlastic = ceramicReturnMap(m, p, dev, tr);
        for (int i = 0; i < 3; ++i) e[i] = dev[i] + tr / 3;
        return deltaPlastic;
    }
    if (m.model == MpmModel::Elastomer) {
        elastomerFailure(m, p, dev, tr);
        for (int i = 0; i < 3; ++i) e[i] = dev[i] + tr / 3;
        return 0;
    }
    if (m.model == MpmModel::Composite) {
        const double deltaPlastic = compositeReturnMap(m, p, dev, tr);
        for (int i = 0; i < 3; ++i) e[i] = dev[i] + tr / 3;
        return deltaPlastic;
    }
    double deltaPlastic = 0;
    if (!p.failed) {
        const double yield = mpmYieldStress(m, p.plasticStrain, p.temperature);
        deltaPlastic = radialReturn(m, dev, yield);
        if (deltaPlastic > 0) {
            p.plasticStrain += deltaPlastic;
            p.temperature += kTaylorQuinney * yield * deltaPlastic / (m.density * m.heat);
        }
        const double mean = m.bulk * tr;  // Kirchhoff mean stress, tension positive
        if (p.plasticStrain > m.failureStrain || mean > m.spall || p.temperature >= m.meltK) p.failed = true;
        // Brittle: a crack opens across the largest principal tension once it passes this point's strength.
        if (m.brittleStress > 0 && 2 * m.shear * std::max({dev[0], dev[1], dev[2]}) + mean > m.brittleStress * p.flaw)
            p.failed = true;
    }
    if (p.failed) {
        dev[0] = dev[1] = dev[2] = 0;
        tr = std::min(tr, 0.0);
    }
    for (int i = 0; i < 3; ++i) e[i] = dev[i] + tr / 3;
    return deltaPlastic;
}

}  // namespace phys
