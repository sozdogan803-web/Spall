#include "render/blackbody.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPlanckH = 6.62607015e-34;   // J s
constexpr double kLightC = 2.99792458e8;      // m/s
constexpr double kBoltzmann = 1.380649e-23;   // J/K
constexpr double kLumensPerWatt = 683.0;      // luminous efficacy at 555 nm

// Piecewise Gaussian lobe of the Wyman-Sloan-Shirley fit.
double lobe(double nm, double mu, double sigmaBelow, double sigmaAbove) {
    const double s = (nm < mu ? sigmaBelow : sigmaAbove);
    const double t = (nm - mu) / s;
    return std::exp(-0.5 * t * t);
}

} // namespace

void cieColourMatch(double nm, double& x, double& y, double& z) {
    x = 1.056 * lobe(nm, 599.8, 37.9, 31.0) + 0.362 * lobe(nm, 442.0, 16.0, 26.7) - 0.065 * lobe(nm, 501.1, 20.4, 26.2);
    y = 0.821 * lobe(nm, 568.8, 46.9, 40.5) + 0.286 * lobe(nm, 530.9, 16.3, 31.1);
    z = 1.217 * lobe(nm, 437.0, 11.8, 36.0) + 0.681 * lobe(nm, 459.0, 26.0, 13.8);
}

double planckRadiance(double metres, double kelvin) {
    if (kelvin <= 0) return 0;
    const double exponent = kPlanckH * kLightC / (metres * kBoltzmann * kelvin);
    if (exponent > 700) return 0;
    const double l5 = metres * metres * metres * metres * metres;
    return 2.0 * kPlanckH * kLightC * kLightC / (l5 * std::expm1(exponent));
}

Xyz blackbodyXYZ(double kelvin) {
    Xyz c;
    for (int nm = 380; nm <= 780; ++nm) {
        double x, y, z;
        cieColourMatch(nm, x, y, z);
        const double L = planckRadiance(nm * 1e-9, kelvin) * 1e-9;  // per nm
        c.X += L * x;
        c.Y += L * y;
        c.Z += L * z;
    }
    c.X *= kLumensPerWatt;
    c.Y *= kLumensPerWatt;
    c.Z *= kLumensPerWatt;
    return c;
}

void xyzToLinearSrgb(const Xyz& c, float rgb[3]) {
    const double r = 3.2404542 * c.X - 1.5371385 * c.Y - 0.4985314 * c.Z;
    const double g = -0.9692660 * c.X + 1.8760108 * c.Y + 0.0415560 * c.Z;
    const double b = 0.0556434 * c.X - 0.2040259 * c.Y + 1.0572252 * c.Z;
    rgb[0] = static_cast<float>(std::max(r, 0.0));
    rgb[1] = static_cast<float>(std::max(g, 0.0));
    rgb[2] = static_cast<float>(std::max(b, 0.0));
}

std::vector<float> makeBlackbodyLut(int n, float Tmin, float Tmax) {
    std::vector<float> lut(static_cast<size_t>(std::max(n, 0)) * 4);
    for (int i = 0; i < n; ++i) {
        const double T = n > 1 ? Tmin + (Tmax - Tmin) * i / (n - 1.0) : Tmin;
        const Xyz c = blackbodyXYZ(T);
        float* e = &lut[static_cast<size_t>(i) * 4];
        xyzToLinearSrgb(c, e);
        e[3] = static_cast<float>(c.Y);
    }
    return lut;
}

double glowBrightness(double kelvin) {
    const double Y = blackbodyXYZ(kelvin).Y;
    if (!(Y > 0)) return 0;
    const double t = std::clamp((std::log10(Y) + 2.5) / 8.0, 0.0, 1.0);
    return t * std::sqrt(t);
}

std::vector<float> makeGlowLut(int n, float Tmin, float Tmax) {
    std::vector<float> lut(static_cast<size_t>(std::max(n, 0)) * 4);
    for (int i = 0; i < n; ++i) {
        const double T = n > 1 ? Tmin + (Tmax - Tmin) * i / (n - 1.0) : Tmin;
        float* e = &lut[static_cast<size_t>(i) * 4];
        xyzToLinearSrgb(blackbodyXYZ(T), e);
        const float top = std::max({e[0], e[1], e[2]});
        const float b = static_cast<float>(glowBrightness(T));
        for (int k = 0; k < 3; ++k) e[k] = top > 0 ? e[k] / top * b : 0.0f;
        e[3] = b;
    }
    return lut;
}
