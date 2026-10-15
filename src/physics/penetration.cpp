#include "physics/penetration.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace phys {

namespace {

constexpr double kPiPen = 3.14159265358979323846;
// Lanz & Odermatt (2000) constants shared by tungsten and DU.
constexpr double kLoB0 = 0.283, kLoB1 = 0.0656, kLoM = -0.224;
constexpr int kJetElements = 600;
constexpr double kWitnessThickness = 50.0;  // m, "semi-infinite" block

struct Span {
    double front, back;  // along the shot line from the first face, m
};

std::vector<Span> spans(const std::vector<ArmourLayer>& layers, double cosT) {
    std::vector<Span> s;
    s.reserve(layers.size());
    double x = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i > 0) x += std::max(0.0, layers[i].gap) / cosT;
        const double los = std::max(0.0, layers[i].thickness) / cosT;
        s.push_back({x, x + los});
        x += los;
    }
    return s;
}

std::string format(const char* fmt, double a, double b = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    return buf;
}

void initLayers(PenetrationResult& r, const std::vector<Span>& sp) {
    r.layers.assign(sp.size(), LayerResult{});
    for (size_t i = 0; i < sp.size(); ++i) {
        r.layers[i].front = sp[i].front;
        r.layers[i].back = sp[i].back;
    }
}

// Marks layer depths for a stop at position x along the shot line.
void markStop(PenetrationResult& r, double x) {
    r.depth = x;
    for (LayerResult& l : r.layers)
        if (!l.perforated) l.depth = std::clamp(x - l.front, 0.0, l.back - l.front);
}

double rodCraterRadius(double diameter, double rodDensity, double v, double u, double resistance) {
    const double q = rodDensity * (v - u) * (v - u) / std::max(resistance, 1e6);
    return 0.5 * diameter * (1.0 + std::sqrt(q));
}

// ---------------------------------------------------------------------------------- long rods

void simulateRod(const Penetrator& p, double v0, double theta, const std::vector<ArmourLayer>& layers,
                 PenetrationResult& r) {
    const double cosT = std::cos(theta);
    const std::vector<Span> sp = spans(layers, cosT);
    initLayers(r, sp);
    const double D = std::max(p.diameter, 1e-4);
    const double area = 0.25 * kPiPen * D * D;
    const double rhoP = p.density, Yp = p.strength;
    const double f = lengthFactor(p.length / D) * std::pow(cosT, kLoM);
    double v = v0, L = p.length, x = 0, t = 0;
    const double dt = 0.005 * D / std::max(v0, 100.0);
    const int sampleEvery = std::max(1, static_cast<int>(p.length / D / 0.005 / 700.0));
    auto push = [&](double rate, double rc, int layer) { r.history.push_back({t, x, v, L, rate, rc, layer}); };
    push(0, 0, 0);

    size_t li = 0;
    int step = 0;
    bool stopped = false;
    while (!stopped) {
        if (li >= layers.size()) {
            r.perforated = true;
            r.residualVelocity = v;
            r.residualLength = L;
            r.residualMass = rhoP * area * L;
            r.exitTime = t;
            r.depth = x;
            break;
        }
        if (x < sp[li].front) {  // free flight across a gap
            t += (sp[li].front - x) / v;
            x = sp[li].front;
            push(v, 0, -1);
            continue;
        }
        const ArmourMaterial& m = layers[li].material;
        // Derivatives: returns false when the rod can no longer penetrate this material.
        auto deriv = [&](double vv, double LL, double& rate, double& dv, double& dL, double& u) {
            if (vv <= 1.0 || LL <= 0.01 * D) return false;
            if (0.5 * m.density * vv * vv + m.resistance <= Yp) {  // rigid: the rod does not erode
                u = vv;
                rate = vv;
                dv = -(0.5 * m.density * vv * vv + m.resistance) / (rhoP * LL);
                dL = 0;
                return true;
            }
            u = tatePenetrationRate(vv, rhoP, Yp, m.density, m.resistance);
            if (u <= 0) return false;
            rate = f * u;
            dv = -Yp / (rhoP * LL);
            dL = -(vv - u);
            return true;
        };
        double rate1 = 0, dv1 = 0, dL1 = 0, u1 = 0, rate2 = 0, dv2 = 0, dL2 = 0, u2 = 0;
        if (!deriv(v, L, rate1, dv1, dL1, u1)) {
            stopped = true;
            break;
        }
        const double vm = v + 0.5 * dt * dv1, Lm = L + 0.5 * dt * dL1;
        if (!deriv(vm, Lm, rate2, dv2, dL2, u2)) {
            stopped = true;
            break;
        }
        double h = dt;
        bool crossed = false;
        if (x + h * rate2 >= sp[li].back) {
            h = std::max(0.0, (sp[li].back - x) / rate2);
            crossed = true;
        }
        x = crossed ? sp[li].back : x + h * rate2;
        v += h * dv2;
        L = std::max(0.0, L + h * dL2);
        t += h;
        if (crossed) {
            r.layers[li].perforated = true;
            r.layers[li].depth = sp[li].back - sp[li].front;
            r.layers[li].exitVelocity = v;
            r.layers[li].exitTime = t;
            push(rate2, rodCraterRadius(D, rhoP, v, u2, m.resistance), static_cast<int>(li));
            ++li;
            continue;
        }
        if (++step % sampleEvery == 0)
            push(rate2, rodCraterRadius(D, rhoP, v, u2, m.resistance), static_cast<int>(li));
        if (step > 4000000) stopped = true;  // safety net; never reached for sane inputs
    }
    if (stopped) {
        markStop(r, x);
        r.history.push_back({t, x, v, L, 0, r.history.empty() ? 0 : r.history.back().craterRadius,
                             static_cast<int>(li)});
        r.residualLength = L;
    }
    const double v1 = std::max(v0, 1.0);
    r.startTime = -1.2 * p.length / v1;
    r.endTime = r.perforated ? t + 2.0 * p.length / std::max(r.residualVelocity, 1.0) : t * 1.08 + 1e-5;
}

// --------------------------------------------------------------------------------- compact AP

void simulateCompact(const Penetrator& p, double v0, double theta, const std::vector<ArmourLayer>& layers,
                     PenetrationResult& r) {
    const double cosT = std::cos(theta);
    const std::vector<Span> sp = spans(layers, cosT);
    initLayers(r, sp);
    const double D = std::max(p.diameter, 1e-4);
    const double area = 0.25 * kPiPen * D * D;
    const double m = penetratorMass(p);
    const double rc = 0.55 * D;
    double v = v0, x = 0, t = 0;
    r.history.push_back({t, x, v, p.length, 0, 0, 0});

    size_t i = 0;
    while (i < layers.size()) {
        // Touching layers act as one block.
        const size_t first = i;
        double tRha = 0, plug = 0;
        do {
            const ArmourLayer& l = layers[i];
            tRha += std::max(0.0, l.thickness) * l.material.keEfficiency * l.material.density / kRhaDensity;
            plug += l.material.density * area * std::max(0.0, l.thickness) / cosT;
            ++i;
        } while (i < layers.size() && layers[i].gap <= 1e-9);
        const size_t last = i - 1;
        const double front = sp[first].front, back = sp[last].back, los = back - front;

        if (x < front) {
            t += (front - x) / v;
            x = front;
            r.history.push_back({t, x, v, p.length, v, 0, -1});
        }
        if (los <= 0) continue;
        const double tEff = tRha / cosT;
        const double vL = deMarreLimitVelocity(tEff, D, m);
        const bool through = v > vL;
        // A shot that does not get through stops where a plate would just have been perforated
        // at its speed: a block too thick to perforate is semi-infinite to it, so the depth does
        // not depend on how thick it is.
        const double stop = through ? los : los * std::min(1.0, deMarreThickness(v, D, m) / tEff);
        const double decel = through ? vL * vL / (2.0 * los) : v * v / (2.0 * std::max(stop, 1e-9));  // force / m
        const double tStop = through ? (v - std::sqrt(v * v - vL * vL)) / decel : v / decel;
        const double v0b = v, t0b = t;
        constexpr int kSamples = 60;
        for (int k = 1; k <= kSamples; ++k) {
            const double tk = tStop * k / kSamples;
            const double xk = front + v0b * tk - 0.5 * decel * tk * tk;
            const double vk = std::max(0.0, v0b - decel * tk);
            int layer = static_cast<int>(last);
            for (size_t j = first; j <= last; ++j)
                if (xk < sp[j].back) {
                    layer = static_cast<int>(j);
                    break;
                }
            r.history.push_back({t0b + tk, std::min(xk, back), vk, p.length, vk, rc, layer});
        }
        if (!through) {
            t = t0b + tStop;
            markStop(r, front + stop);
            r.history.back().depth = r.depth;
            break;
        }
        for (size_t j = first; j <= last; ++j) {
            const double dx = sp[j].back - front;
            const double tj = (v0b - std::sqrt(std::max(0.0, v0b * v0b - 2 * decel * dx))) / decel;
            r.layers[j].perforated = true;
            r.layers[j].depth = sp[j].back - sp[j].front;
            r.layers[j].exitVelocity = std::sqrt(std::max(0.0, v0b * v0b - 2 * decel * dx));
            r.layers[j].exitTime = t0b + tj;
        }
        const double z = tEff / D;
        const double vr = lambertJonasResidual(v0b, vL, 2.0 + z / 3.0, m / (m + plug / 3.0));
        t = t0b + tStop;
        x = back;
        v = vr;
        r.layers[last].exitVelocity = vr;
        r.history.push_back({t, x, v, p.length, v, rc, static_cast<int>(last)});
    }
    if (i >= layers.size() && (layers.empty() || r.layers.back().perforated)) {
        r.perforated = true;
        r.residualVelocity = v;
        r.residualLength = p.length;
        r.residualMass = m;
        r.exitTime = t;
        r.depth = x;
    }
    const double v1 = std::max(v0, 1.0);
    r.startTime = -1.5 * std::max(p.length, 2 * D) / v1;
    r.endTime = r.perforated ? t + 4.0 * std::max(p.length, 2 * D) / std::max(r.residualVelocity, 1.0)
                             : t * 1.15 + 2e-5;
}

// ----------------------------------------------------------------------------- shaped charges

void jetImpl(const JetParameters& jp, double rhoJ, double coneDiameter, double theta,
             const std::vector<ArmourLayer>& layers, PenetrationResult& r) {
    const double cosT = std::cos(theta);
    const std::vector<Span> sp = spans(layers, cosT);
    initLayers(r, sp);
    if (layers.empty()) return;
    const double S = jp.virtualOrigin;
    const double dv = (jp.tipVelocity - jp.minVelocity) / kJetElements;
    size_t li = 0;
    double xc = 0;       // crater bottom along the shot line from the first face
    double tfree = 0;    // time the crater bottom is free for the next element
    const double t0 = S / jp.tipVelocity;
    r.history.push_back({t0, 0, jp.tipVelocity, 0, 0, 0, 0});
    r.jet.reserve(kJetElements);

    for (int e = 0; e < kJetElements; ++e) {
        const double v = jp.tipVelocity - (e + 0.5) * dv;
        JetElement el{v, 0, 0, 0, 0, JetElementFate::Consumed};
        if (li >= layers.size()) {  // the stack is already perforated: free flight through the hole
            const double tExit = (S + sp.back().back) / v;
            el.fullLength = dv * std::min(tExit, jp.breakupTime);
            el.length = el.fullLength;
            if (tExit > jp.breakupTime) {
                const double sigma = jp.drift * v * (tExit - jp.breakupTime);
                el.length *= 1.0 - std::exp(-0.5 * (jp.craterRadius / std::max(sigma, 1e-12)) *
                                            (jp.craterRadius / std::max(sigma, 1e-12)));
            }
            el.arrival = el.finish = tExit;
            el.fate = JetElementFate::Residual;
            r.jet.push_back(el);
            continue;
        }
        const double tarr = std::max((S + xc) / v, tfree);
        double ell = dv * std::min(tarr, jp.breakupTime);
        el.fullLength = ell;
        if (tarr > jp.breakupTime) {
            const double sigma = jp.drift * v * (tarr - jp.breakupTime);
            const double s = jp.craterRadius / std::max(sigma, 1e-12);
            ell *= 1.0 - std::exp(-0.5 * s * s);
        }
        el.length = ell;
        el.arrival = tarr;
        double t = tarr;
        while (ell > 0 && li < layers.size()) {
            const ArmourMaterial& m = layers[li].material;
            const double u = tatePenetrationRate(v, rhoJ, 0.0, m.density, m.resistance);
            if (u <= 0) {  // too slow: the element piles up at the bottom
                t += ell / v;
                ell = 0;
                el.fate = JetElementFate::Stopped;
                break;
            }
            const double tau = ell / (v - u);
            const double rc = jp.jetRadius * std::sqrt(rhoJ * (v - u) * (v - u) / (2.0 * m.resistance) + 1.0);
            if (xc + u * tau < sp[li].back) {
                xc += u * tau;
                t += tau;
                ell = 0;
                r.history.push_back({t, xc, v, 0, u, rc, static_cast<int>(li)});
            } else {
                const double dtl = (sp[li].back - xc) / u;
                ell -= (v - u) * dtl;
                t += dtl;
                xc = sp[li].back;
                r.layers[li].perforated = true;
                r.layers[li].depth = sp[li].back - sp[li].front;
                r.layers[li].exitVelocity = v;
                r.layers[li].exitTime = t;
                r.history.push_back({t, xc, v, 0, u, rc, static_cast<int>(li)});
                ++li;
                if (li < layers.size()) {
                    t += (sp[li].front - xc) / v;
                    xc = sp[li].front;
                    r.history.push_back({t, xc, v, 0, v, 0, -1});
                }
            }
        }
        if (li >= layers.size() && !r.perforated) {
            r.perforated = true;
            r.residualVelocity = v;
            r.exitTime = t;
            el.fate = JetElementFate::Residual;
            r.residualLength += ell;
        }
        el.finish = t;
        tfree = t;
        r.jet.push_back(el);
    }
    for (const JetElement& el : r.jet)
        if (el.fate == JetElementFate::Residual && el.velocity < r.residualVelocity) r.residualLength += el.length;
    if (r.perforated) {
        r.depth = sp.empty() ? 0 : sp.back().back;
    } else {
        markStop(r, xc);
    }
    // Remaining effective jet length behind the crater bottom, for the time-history plot.
    double tail = 0;
    std::vector<double> remaining(r.jet.size() + 1, 0.0);
    for (size_t k = r.jet.size(); k-- > 0;) {
        if (r.jet[k].fate != JetElementFate::Residual) tail += r.jet[k].length;
        remaining[k] = tail;
    }
    for (PenetrationSample& s : r.history) {
        const int k = std::clamp(static_cast<int>((jp.tipVelocity - s.velocity) / dv), 0, kJetElements);
        s.length = remaining[static_cast<size_t>(k)];
    }
    r.history.front().length = remaining[0];
    r.startTime = 0.25 * t0;
    double end = t0;
    for (const JetElement& el : r.jet) end = std::max(end, el.finish);
    r.endTime = r.perforated ? std::max(end, (S + r.depth + 3.0 * coneDiameter) / jp.minVelocity) : end * 1.05;
}

void addWarnings(const Penetrator& p, double velocity, double obliquityDeg, const std::vector<ArmourLayer>& layers,
                 PenetrationResult& r) {
    if (obliquityDeg > 70.0)
        r.warnings.push_back(format("Obliquity %.0f deg: ricochet and nose deflection are not modelled above ~70 deg.",
                                    obliquityDeg));
    bool ceramic = false, soft = false, spaced = false, reactive = false;
    for (size_t i = 0; i < layers.size(); ++i) {
        const ArmourMaterial& m = layers[i].material;
        ceramic |= m.ceramic;
        reactive |= m.reactive;
        soft |= !m.ceramic && (m.hardnessBhn < 150 || m.hardnessBhn > 550 || m.density < 7500 || m.density > 8100);
        spaced |= i > 0 && layers[i].gap > 1e-9;
    }
    if (ceramic)
        r.warnings.push_back("Ceramic layers are assumed laterally confined; dwell, cracking and comminution are not modelled.");
    if (reactive)
        r.warnings.push_back("Explosive layers are treated here as inert soft plates; the Deformation (MPM) display "
                             "simulates their reaction.");
    if (p.kind == PenetratorKind::LongRod) {
        const double ld = p.length / std::max(p.diameter, 1e-6);
        if (ld < 10 || ld > 40)
            r.warnings.push_back(format("Rod L/D %.1f is outside the Lanz-Odermatt calibration range (10-40).", ld));
        if (velocity < 1000 || velocity > 2200)
            r.warnings.push_back(format("Impact velocity %.0f m/s is outside the long-rod calibration range (1000-2200 m/s).",
                                        velocity));
        if (soft)
            r.warnings.push_back("Non-steel or very soft/hard layers use estimated Tate resistances (low confidence).");
        if (spaced && obliquityDeg > 20)
            r.warnings.push_back("Yaw and bending of the rod on leaving an oblique plate are not modelled.");
    } else if (p.kind == PenetratorKind::CompactAp) {
        if (velocity > 1300)
            r.warnings.push_back(format("Impact velocity %.0f m/s is above the de Marre fitting range (~1300 m/s).", velocity));
        if (obliquityDeg > 55)
            r.warnings.push_back("Compact shot may shatter or ricochet at high obliquity; not modelled.");
        r.warnings.push_back("The engineering estimate does not model shot shatter, caps or face-hardened plates; the "
                             "Deformation (MPM) view cracks brittle cores and shells itself.");
    } else if (p.kind == PenetratorKind::HeWarhead) {
        r.warnings.push_back("HE warhead: the engineering model only times the nose reaching each plate; the "
                             "Deformation (MPM) display simulates the fuze, the detonation and the plates.");
    } else {
        if (p.standoff > 10)
            r.warnings.push_back(format("Standoff %.1f CD: particle drift of a long-standoff jet is approximate.", p.standoff));
        if (obliquityDeg > 60)
            r.warnings.push_back("Oblique plates can deflect a jet (asymmetric collapse); not modelled.");
    }
}

// HE warhead: no penetration model, only the nose reaching each plate's front face at the flight
// speed (the timeline), and an end time that leaves room for the detonation and the plates' response.
void simulateArrivals(const Penetrator& p, double velocity, double theta, const std::vector<ArmourLayer>& layers,
                      PenetrationResult& r) {
    const double cosT = std::cos(theta);
    double x = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i > 0) x += layers[i].gap / cosT;
        LayerResult l;
        l.front = x;
        x += layers[i].thickness / cosT;
        l.back = x;
        r.layers.push_back(l);
        PenetrationSample s{};
        s.time = l.front / velocity;
        s.depth = l.front;
        s.velocity = velocity;
        s.length = p.length;
        s.layer = static_cast<int>(i);
        r.history.push_back(s);
    }
    r.depth = r.layers.empty() ? 0 : r.layers.back().front;
    double after = 150e-6 * p.diameter / 0.1;
    if (p.fuze.detonationSpeed > 0) after += 10 * p.fuze.fillerLength / p.fuze.detonationSpeed;
    r.endTime = (r.history.empty() ? 0 : r.history.back().time) + std::clamp(after, 100e-6, 1e-3);
}

}  // namespace

const char* penetratorKindId(PenetratorKind k) {
    switch (k) {
    case PenetratorKind::LongRod: return "long_rod";
    case PenetratorKind::CompactAp: return "compact_ap";
    case PenetratorKind::ShapedCharge: return "shaped_charge";
    case PenetratorKind::HeWarhead: return "he_warhead";
    default: return "?";
    }
}

const char* penetratorKindLabel(PenetratorKind k) {
    switch (k) {
    case PenetratorKind::LongRod: return "Long rod (APFSDS)";
    case PenetratorKind::CompactAp: return "Compact AP (shot or core)";
    case PenetratorKind::ShapedCharge: return "Shaped charge (HEAT)";
    case PenetratorKind::HeWarhead: return "HE warhead (missile/rocket)";
    default: return "?";
    }
}

double penetratorMass(const Penetrator& p) {
    if (p.mass > 0) return p.mass;
    return p.density * 0.25 * kPiPen * p.diameter * p.diameter * p.length;
}

JetParameters jetParameters(const Penetrator& p) {
    const double q = std::clamp(p.jetQuality, -0.8, 1.5);
    const double cd = std::max(p.coneDiameter, 1e-3);
    JetParameters j{};
    j.tipVelocity = 7000.0 + 2000.0 * q;
    j.minVelocity = kJetMinVelocity;
    j.breakupTime = (0.8 + 0.8 * q) * cd * 1000.0 * 1e-6;
    j.drift = (8.0 - 4.0 * q) * 1e-3;
    j.virtualOrigin = (std::max(p.standoff, 0.0) + 1.0) * cd;
    j.craterRadius = 0.12 * cd;
    j.jetRadius = 0.025 * cd;
    return j;
}

double jetParticleOffset(int k) {
    uint32_t h = static_cast<uint32_t>(k) * 2654435761u + 0x9e3779b9u;
    double sum = 0;
    for (int i = 0; i < 4; ++i) {
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        sum += static_cast<double>(h & 0xffffu) / 65535.0;
    }
    return (sum - 2.0) * 1.7320508;  // variance of 4 uniforms is 1/3
}

PenetrationResult simulatePenetration(const Penetrator& p, double velocity, double obliquityDeg,
                                      const std::vector<ArmourLayer>& layers) {
    PenetrationResult r;
    const double theta = std::clamp(obliquityDeg, 0.0, kMaxObliquityDeg) * kPiPen / 180.0;
    switch (p.kind) {
    case PenetratorKind::LongRod: simulateRod(p, std::max(velocity, 1.0), theta, layers, r); break;
    case PenetratorKind::CompactAp: simulateCompact(p, std::max(velocity, 1.0), theta, layers, r); break;
    case PenetratorKind::HeWarhead: simulateArrivals(p, std::max(velocity, 1.0), theta, layers, r); break;
    default: jetImpl(jetParameters(p), p.linerDensity, p.coneDiameter, theta, layers, r); break;
    }
    addWarnings(p, velocity, obliquityDeg, layers, r);
    return r;
}

PenetrationResult simulateJet(const JetParameters& jet, double linerDensity, double coneDiameter,
                              double obliquityDeg, const std::vector<ArmourLayer>& layers) {
    PenetrationResult r;
    const double theta = std::clamp(obliquityDeg, 0.0, kMaxObliquityDeg) * kPiPen / 180.0;
    jetImpl(jet, linerDensity, coneDiameter, theta, layers, r);
    return r;
}

double rhaPenetration(const Penetrator& p, double velocity) {
    if (p.kind == PenetratorKind::HeWarhead) return 0;
    if (p.kind == PenetratorKind::CompactAp) return deMarreThickness(velocity, p.diameter, penetratorMass(p));
    ArmourLayer block;
    block.thickness = kWitnessThickness;
    return simulatePenetration(p, velocity, 0.0, {block}).depth;
}

double residualPenetration(const Penetrator& p, double velocity, double obliquityDeg,
                           const std::vector<ArmourLayer>& layers) {
    if (p.kind == PenetratorKind::HeWarhead) return 0;
    const double cosT = std::cos(std::clamp(obliquityDeg, 0.0, kMaxObliquityDeg) * kPiPen / 180.0);
    if (p.kind == PenetratorKind::CompactAp) {
        const PenetrationResult r = simulatePenetration(p, velocity, obliquityDeg, layers);
        return r.perforated ? rhaPenetration(p, r.residualVelocity) / cosT : 0.0;
    }
    std::vector<ArmourLayer> withWitness = layers;
    ArmourLayer witness;
    witness.thickness = kWitnessThickness;
    withWitness.push_back(witness);
    const PenetrationResult r = simulatePenetration(p, velocity, obliquityDeg, withWitness);
    return r.layers.empty() ? 0.0 : r.layers.back().depth;
}

double ballisticLimit(const Penetrator& p, double obliquityDeg, const std::vector<ArmourLayer>& layers,
                      double maxVelocity) {
    if (p.kind == PenetratorKind::ShapedCharge || p.kind == PenetratorKind::HeWarhead || layers.empty()) return 0;
    if (!simulatePenetration(p, maxVelocity, obliquityDeg, layers).perforated) return -1;
    double lo = 1.0, hi = maxVelocity;
    for (int k = 0; k < 40 && hi - lo > 0.25; ++k) {
        const double mid = 0.5 * (lo + hi);
        (simulatePenetration(p, mid, obliquityDeg, layers).perforated ? hi : lo) = mid;
    }
    return hi;
}

double fitJetQuality(Penetrator p, double publishedRhaDepth) {
    p.kind = PenetratorKind::ShapedCharge;
    double lo = -0.8, hi = 1.5;
    p.jetQuality = hi;
    if (rhaPenetration(p, 0) < publishedRhaDepth) return hi;
    p.jetQuality = lo;
    if (rhaPenetration(p, 0) > publishedRhaDepth) return lo;
    for (int k = 0; k < 40; ++k) {
        p.jetQuality = 0.5 * (lo + hi);
        (rhaPenetration(p, 0) < publishedRhaDepth ? lo : hi) = p.jetQuality;
    }
    return 0.5 * (lo + hi);
}

double lanzOdermattRatio(bool uranium, double lengthOverDiameter, double velocity, double rodDensity,
                         double targetDensity, double bhn, double obliquityDeg) {
    const double a = uranium ? 0.825 : 0.994;
    const double c0 = uranium ? 90.0 : 134.5;
    const double c1 = uranium ? -0.0849 : -0.148;
    const double vk = velocity / 1000.0;
    const double theta = obliquityDeg * kPiPen / 180.0;
    return a * lengthFactor(lengthOverDiameter) * std::pow(std::cos(theta), kLoM) *
           std::sqrt(rodDensity / targetDensity) * std::exp(-(c0 + c1 * bhn) * bhn / (rodDensity * vk * vk));
}

double steelResistanceFromBhn(double bhn) {
    // Fitted so that Tate x lengthFactor matches Lanz-Odermatt (Yp = 1.5 GPa).
    if (bhn <= 300) return 3.95e9 + (bhn - 150.0) * (5.6e9 - 3.95e9) / 150.0;
    if (bhn <= 500) return 5.6e9 + (bhn - 300.0) * (6.2e9 - 5.6e9) / 200.0;
    return 6.2e9 + (bhn - 500.0) * 0.6e9 / 200.0;
}

double tatePenetrationRate(double v, double rodDensity, double rodStrength, double targetDensity,
                           double targetResistance) {
    if (v <= 0) return 0;
    const double dR = targetResistance - rodStrength;
    if (0.5 * rodDensity * v * v <= dR) return 0;
    const double mu2 = targetDensity / rodDensity;
    double u;
    if (std::fabs(1.0 - mu2) < 1e-6) {
        u = (v * v - 2.0 * dR / rodDensity) / (2.0 * v);
    } else {
        const double A = 2.0 * dR * (1.0 - mu2) / targetDensity;
        u = (v - std::sqrt(mu2) * std::sqrt(std::max(0.0, v * v + A))) / (1.0 - mu2);
    }
    return std::clamp(u, 0.0, v);
}

double lengthFactor(double lengthOverDiameter) {
    return 1.0 / std::tanh(kLoB0 + kLoB1 * std::max(lengthOverDiameter, 0.0));
}

double deMarreLimitVelocity(double rhaThickness, double diameter, double mass) {
    return kDeMarreK * std::pow(10.0 * std::max(rhaThickness, 0.0), 0.7) * std::pow(10.0 * diameter, 0.75) /
           std::sqrt(std::max(mass, 1e-9));
}

double deMarreThickness(double velocity, double diameter, double mass) {
    const double v = std::max(velocity, 0.0);
    return std::pow(v * std::sqrt(std::max(mass, 1e-9)) / (kDeMarreK * std::pow(10.0 * diameter, 0.75)), 1.0 / 0.7) / 10.0;
}

double lambertJonasResidual(double v, double limitVelocity, double exponent, double plugFactor) {
    if (v <= limitVelocity) return 0;
    return plugFactor * std::pow(std::pow(v, exponent) - std::pow(limitVelocity, exponent), 1.0 / exponent);
}

}  // namespace phys
