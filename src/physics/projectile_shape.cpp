#include "physics/projectile_shape.h"

#include "physics/mpm_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace phys {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct StyleName {
    ShapeStyle style;
    const char* id;
};
constexpr StyleName kStyles[] = {
    {ShapeStyle::Dart, "dart"},   {ShapeStyle::Ap, "ap"},         {ShapeStyle::Apc, "apc"},
    {ShapeStyle::Apbc, "apbc"},   {ShapeStyle::Apcbc, "apcbc"},   {ShapeStyle::Apcr, "apcr"},
    {ShapeStyle::Apds, "apds"},   {ShapeStyle::Bullet, "bullet"}, {ShapeStyle::HeMissile, "he_missile"},
    {ShapeStyle::HeatRocket, "heat_rocket"},
};

// A nose profile from its tip at `start`: tangent ogive (rho > 0), elliptical (rho = 0, len > 0) or
// flat (len = 0); full radius R behind it.
struct Nose {
    double start = 0, len = 0, R = 0, rho = 0;
    double r(double x) const {
        const double u = x - start;
        if (u < 0) return 0;
        if (u >= len) return R;
        if (rho > 0) return std::max(0.0, std::sqrt(std::max(0.0, rho * rho - (len - u) * (len - u))) - (rho - R));
        const double f = (len - u) / len;
        return R * std::sqrt(std::max(0.0, 1 - f * f));
    }
};

// none: elliptical over min(1.3 R, 0.5 L) (the legacy compact nose); 0: flat; > 0: tangent ogive of
// crh calibres (at least a hemisphere).
Nose makeNose(const std::optional<double>& crh, double R, double L, double start = 0) {
    Nose n;
    n.start = start;
    n.R = R;
    if (!crh) {
        n.len = std::min(1.3 * R, 0.5 * L);
    } else if (*crh > 0) {
        n.rho = std::max(*crh, 0.5) * 2 * R;
        n.len = std::sqrt(n.rho * n.rho - (n.rho - R) * (n.rho - R));
    }
    return n;
}

// A tangent ogive of the given length (elliptical when shorter than R, where no ogive fits).
Nose noseOfLength(double len, double R) {
    Nose n;
    n.R = R;
    n.len = len;
    if (len >= R) n.rho = (len * len + R * R) / (2 * R);
    return n;
}

using Profile = std::function<double(double)>;

struct Def {
    PartRole role;
    std::string material;
    double x0, x1;
    Profile out, in;
    bool penetrator = false;
    bool simulated = true;
    double density = 0;
};

// Collects parts as radius functions, then samples them all on one grid of stations, so parts that
// share a boundary interpolate it identically; a radius that jumps at a grid point gets two stations.
struct Builder {
    double R, eps, tol;
    std::vector<double> grid;
    std::vector<Def> defs;

    Builder(double length, double radius)
        : R(radius), eps(1e-12 * std::max(length, 2 * radius)), tol(1e-5 * radius) {}

    void point(double x) { grid.push_back(x); }
    // Nose stations at start + len t^2: dense at the tip, every <= R/8 behind it.
    void nose(const Nose& n) {
        if (n.len <= 0) return;
        const int k = std::max(8, static_cast<int>(std::ceil(16 * n.len / n.R)));
        for (int i = 0; i <= k; ++i) {
            const double t = static_cast<double>(i) / k;
            point(n.start + n.len * t * t);
        }
    }
    Def& add(PartRole role, std::string material, double x0, double x1, Profile out, Profile in) {
        point(x0);
        point(x1);
        defs.push_back({role, std::move(material), x0, x1, std::move(out), std::move(in)});
        return defs.back();
    }

    double eval(const Profile& f, double x, int side) const {
        const double v = f(x), w = f(x + side * eps);
        return std::abs(w - v) > tol ? w : v;
    }
    ShapeStation station(const Def& d, double x, int side) const {
        const double o = std::max(0.0, eval(d.out, x, side));
        const double i = std::clamp(eval(d.in, x, side), 0.0, o);
        return {x, o, i};
    }

    ProjectileShape finish(double front, double back, double penDiameter) {
        std::sort(grid.begin(), grid.end());
        std::vector<double> xs;
        for (double g : grid)
            if (xs.empty() || g - xs.back() > 4 * eps) xs.push_back(g);
        ProjectileShape s;
        std::stable_sort(defs.begin(), defs.end(), [](const Def& a, const Def& b) { return a.x0 < b.x0; });
        for (const Def& d : defs) {
            if (d.x1 - d.x0 <= 4 * eps) continue;
            ShapePart part;
            part.role = d.role;
            part.material = d.material;
            part.penetrator = d.penetrator;
            part.simulated = d.simulated;
            part.density = d.density;
            part.stations.push_back(station(d, d.x0, +1));
            for (double g : xs) {
                if (g <= d.x0 + 4 * eps || g >= d.x1 - 4 * eps) continue;
                const ShapeStation lo = station(d, g, -1), hi = station(d, g, +1);
                part.stations.push_back(lo);
                if (std::abs(lo.rOuter - hi.rOuter) > tol || std::abs(lo.rInner - hi.rInner) > tol)
                    part.stations.push_back(hi);
            }
            part.stations.push_back(station(d, d.x1, -1));
            for (const ShapeStation& st : part.stations) s.diameter = std::max(s.diameter, 2 * st.rOuter);
            s.length = std::max(s.length, d.x1);
            s.parts.push_back(std::move(part));
        }
        s.penetratorFront = front;
        s.penetratorBack = back;
        s.penetratorDiameter = penDiameter;
        return s;
    }
};

double orDefault(double v, double fallback) { return v > 0 ? v : fallback; }

std::string orMaterial(const std::string& v, const char* fallback) { return v.empty() ? fallback : v; }

// Boat tail: the radius tapers linearly from R to the base's over the last `boatTail`.
Profile tailProfile(const ShapeSpec& spec, double R, double L, double from) {
    if (spec.boatTail <= 0) return [R](double) { return R; };
    const double bt = std::min(spec.boatTail, 0.5 * (L - from));
    const double rb = std::min(R, spec.boatTailDiameter > 0 ? 0.5 * spec.boatTailDiameter : 0.85 * R);
    const double x0 = L - bt;
    return [=](double x) { return x > x0 && bt > 0 ? R + (rb - R) * (x - x0) / bt : R; };
}

ProjectileShape buildDart(const ShapeSpec& spec, const Penetrator& p) {
    const double D = orDefault(spec.diameter, p.diameter), R = 0.5 * D;
    const double rod = orDefault(spec.length, p.length);
    const double cap = orDefault(spec.dartCap.length, kDartCapLength * D);
    const double wall = std::min(R, orDefault(spec.dartCap.wall, kDartCapWall * D) * std::hypot(1.0, R / cap));
    const double finTip = std::max(R, 0.5 * orDefault(spec.fins.span, kDartFinSpan * D));
    const double root = std::min(orDefault(spec.fins.rootChord, kDartFinRootChord * D), rod);
    const double tip = std::min(orDefault(spec.fins.tipChord, kDartFinTipChord * D), root);
    const double sweep = spec.fins.sweep < 0 ? root - tip : std::clamp(spec.fins.sweep, 0.0, root - tip);
    const double trL = orDefault(spec.tracer.length, kDartTracerLength * D);
    const double trR = std::min(R, 0.5 * orDefault(spec.tracer.diameter, kDartTracerWidth * D));
    const double end = cap + rod;

    Builder b(end + trL, R);
    // Hollow cone: outer R x / cap, inner the outer less the wall (measured across).
    b.add(PartRole::Cap, "light_alloy", 0, cap, [=](double x) { return R * x / cap; },
          [=](double x) { return std::max(0.0, R * x / cap - wall); });
    b.point(wall * cap / R);
    b.add(PartRole::Body, spec.bodyMaterial, cap, end, [=](double) { return R; }, [](double) { return 0.0; })
        .penetrator = true;
    // Trapezoid fins, trailing edge at the tail: leading edge swept over `sweep`, tip chord, then
    // back down to the rod over whatever of the root chord is left.
    const double f0 = end - root, f1 = f0 + sweep, f2 = f1 + tip;
    b.add(PartRole::Fins, "light_alloy", f0, end,
          [=](double x) {
              if (x < f1) return sweep > 0 ? R + (finTip - R) * (x - f0) / sweep : finTip;
              if (x <= f2) return finTip;
              return finTip - (finTip - R) * (x - f2) / std::max(end - f2, 1e-300);
          },
          [=](double) { return R; });
    b.point(f1);
    b.point(f2);
    b.add(PartRole::Tracer, "light_alloy", end, end + trL, [=](double) { return trR; }, [](double) { return 0.0; });
    return b.finish(cap, end, D);
}

// ap / apc / apbc / apcbc (and he_missile until its own builder): the whole round is the body,
// optionally under a cap and a windshield, with a filler and its base fuze, a band and a tracer.
ProjectileShape buildShell(const ShapeSpec& spec, const Penetrator& p) {
    const double D = orDefault(spec.diameter, p.diameter), R = 0.5 * D;
    const double L = orDefault(spec.length, p.length);
    const bool hasWs = spec.style == ShapeStyle::Apbc || spec.style == ShapeStyle::Apcbc;
    const bool hasCap = spec.style == ShapeStyle::Apc || spec.style == ShapeStyle::Apcbc;
    Builder b(L, R);

    Nose inner = makeNose(spec.noseCrh, R, L);
    Nose ws;
    double wsLen = 0, x0 = 0;
    if (hasWs) {
        wsLen = spec.windshield.length > 0 ? spec.windshield.length
                                           : makeNose(orDefault(spec.windshield.crh, 3.0), R, L).len;
        wsLen = std::min(wsLen, 0.6 * L);
        ws = noseOfLength(wsLen, R);
        x0 = std::max(0.0, wsLen - inner.len);
        b.nose(ws);
    }
    inner.start = x0;
    b.nose(inner);
    const auto clampWs = [=](double x, double r) { return hasWs && x < wsLen ? std::min(r, ws.r(x)) : r; };

    const double capLen = hasCap ? std::min(orDefault(spec.cap.length, 0.6 * D), 0.8 * (L - x0)) : 0;
    const double bodyTip = x0 + 0.4 * capLen;
    Nose bodyNose = inner;
    bodyNose.start = bodyTip;
    if (hasCap) b.nose(bodyNose);
    const Profile tail = tailProfile(spec, R, L, bodyTip);
    if (spec.boatTail > 0) b.point(L - std::min(spec.boatTail, 0.5 * (L - bodyTip)));
    const auto bodyOut = [=](double x) { return std::min(clampWs(x, bodyNose.r(x)), tail(x)); };

    // Tracer cavity at the base.
    double trL = 0, trR = 0;
    if (spec.tracer.length > 0) {
        trL = std::min(spec.tracer.length, 0.3 * (L - bodyTip));
        trR = std::min(0.5 * orDefault(spec.tracer.diameter, 0.3 * D), 0.8 * bodyOut(L - b.eps));
    }
    // Filler cavity and its base fuze directly behind it, inside the body ahead of the tracer.
    double ff = 0, fl = 0, fuzeLen = 0, fr = 0;
    if (!spec.filler.explosive.empty()) {
        const double fd = orDefault(spec.filler.diameter, 0.3 * D);
        fl = orDefault(spec.filler.length, D);
        fuzeLen = 0.4 * fd;
        const double front = bodyTip, back = L - trL - 0.1 * D;
        fl = std::min(fl, back - front - fuzeLen);
        if (fl > 0) {
            ff = spec.filler.fromNose >= 0 ? spec.filler.fromNose : 0.5 * (front + back - fl - fuzeLen);
            ff = std::clamp(ff, front, back - fl - fuzeLen);
            const double end = ff + fl + fuzeLen;
            fr = std::min(0.5 * fd, 0.8 * std::min(bodyOut(ff + b.eps), bodyOut(end - b.eps)));
        }
        if (fl <= 0 || fr <= 0) fl = fuzeLen = fr = 0;
    }
    const double cavityEnd = ff + fl + fuzeLen;

    Def& body = b.add(PartRole::Body, spec.bodyMaterial, bodyTip, L, bodyOut, [=](double x) {
        if (fr > 0 && x > ff && x < cavityEnd) return fr;
        if (trL > 0 && x > L - trL) return trR;
        return 0.0;
    });
    body.penetrator = true;
    if (hasCap) {
        b.add(PartRole::Cap, spec.cap.material, x0, x0 + capLen, [=](double x) { return clampWs(x, inner.r(x)); },
              [=](double x) { return x > bodyTip ? bodyOut(x) : 0.0; });
    }
    if (hasWs) {
        const double wall = orDefault(spec.windshield.wall, 0.03 * D);
        const auto innerOut = [=](double x) {
            double r = x > bodyTip ? bodyOut(x) : 0.0;
            if (hasCap && x > x0 && x < x0 + capLen) r = std::max(r, clampWs(x, inner.r(x)));
            return r;
        };
        b.add(PartRole::Windshield, spec.windshield.material, 0, wsLen, [=](double x) { return ws.r(x); },
              [=](double x) { return std::min(ws.r(x), std::max(ws.r(x) - wall, innerOut(x))); });
    }
    if (fr > 0) {
        Def& filler = b.add(PartRole::Filler, "explosive:" + spec.filler.explosive, ff, ff + fl,
                            [=](double) { return fr; }, [](double) { return 0.0; });
        filler.density = spec.filler.density;
        b.add(PartRole::Fuze, "gilding_metal", ff + fl, cavityEnd, [=](double) { return fr; }, [](double) { return 0.0; });
    }
    if (spec.band.width > 0) {
        const double w = std::min(spec.band.width, L - bodyTip);
        const double bf = std::clamp(L - spec.band.fromBase - w, bodyTip, L - w);
        b.add(PartRole::Band, "gilding_metal", bf, bf + w, [=](double) { return 1.02 * R; }, bodyOut);
    }
    if (trL > 0)
        b.add(PartRole::Tracer, "incendiary", L - trL, L, [=](double) { return trR; }, [](double) { return 0.0; });
    return b.finish(0, L, D);
}

ProjectileShape buildApcr(const ShapeSpec& spec, const Penetrator& p) {
    const double cD = orDefault(spec.core.diameter, p.diameter), rc = 0.5 * cD;
    const double cl = orDefault(spec.core.length, p.length);
    const double D = std::max(orDefault(spec.diameter, 2 * cD), cD), R = 0.5 * D;
    const double L = std::max(orDefault(spec.length, cl + D), cl);
    const double cf = std::clamp(spec.core.fromNose >= 0 ? spec.core.fromNose : 0.25 * L, 0.0, L - cl);
    Builder b(L, R);
    const Nose outer = makeNose(spec.noseCrh, R, L);
    const Nose coreNose = makeNose(spec.core.noseCrh, rc, cl, cf);
    b.nose(outer);
    b.nose(coreNose);
    const Profile tail = tailProfile(spec, R, L, 0);
    if (spec.boatTail > 0) b.point(L - std::min(spec.boatTail, 0.5 * L));
    const auto out = [=](double x) { return std::min(outer.r(x), tail(x)); };
    b.add(PartRole::Body, orMaterial(spec.bodyMaterial, "light_alloy"), 0, L, out,
          [=](double x) { return x > cf && x < cf + cl ? std::min(coreNose.r(x), out(x)) : 0.0; });
    b.add(PartRole::Core, spec.core.material, cf, cf + cl, [=](double x) { return coreNose.r(x); },
          [](double) { return 0.0; })
        .penetrator = true;
    return b.finish(cf, cf + cl, cD);
}

ProjectileShape buildApds(const ShapeSpec& spec, const Penetrator& p) {
    const double cD = orDefault(spec.core.diameter, p.diameter), rc = 0.5 * cD;
    const double cl = orDefault(spec.core.length, p.length);
    Builder b(cl, rc);
    const Nose nose = makeNose(spec.core.noseCrh ? spec.core.noseCrh : spec.noseCrh, rc, cl);
    b.nose(nose);
    b.add(PartRole::Core, spec.core.material, 0, cl, [=](double x) { return nose.r(x); }, [](double) { return 0.0; })
        .penetrator = true;
    return b.finish(0, cl, cD);
}

ProjectileShape buildBullet(const ShapeSpec& spec, const Penetrator& p) {
    const double cD = orDefault(spec.core.diameter, p.diameter), rc = 0.5 * cD;
    const double cl = orDefault(spec.core.length, p.length);
    const double D = std::max(orDefault(spec.diameter, 1.3 * cD), cD), R = 0.5 * D;
    const double L = std::max(orDefault(spec.length, cl + D), cl);
    const double t = std::min(orDefault(spec.jacket.thickness, 0.07 * D), 0.5 * R);
    const double cf = std::clamp(spec.core.fromNose >= 0 ? spec.core.fromNose : L - cl - 0.05 * D, 0.0, L - cl);
    const double incL = std::min(spec.incendiary.length, cf), inc0 = cf - incL;
    Builder b(L, R);
    const Nose outer = makeNose(spec.noseCrh, R, L);
    const Nose coreNose = makeNose(spec.core.noseCrh, rc, cl, cf);
    b.nose(outer);
    b.nose(coreNose);
    const Profile tail = tailProfile(spec, R, L, 0);
    if (spec.boatTail > 0) b.point(L - std::min(spec.boatTail, 0.5 * L));
    const auto out = [=](double x) { return std::min(outer.r(x), tail(x)); };
    const auto jIn = [=](double x) { return std::max(out(x) - t, 0.0); };
    const auto coreOut = [=](double x) { return std::min(coreNose.r(x), jIn(x)); };
    b.add(PartRole::Jacket, spec.jacket.material, 0, L, out, jIn);
    b.add(PartRole::Sleeve, orMaterial(spec.bodyMaterial, "lead"), 0, L, jIn, [=](double x) {
        if (x > cf && x < cf + cl) return coreOut(x);
        if (incL > 0 && x > inc0 && x < cf) return jIn(x);
        return 0.0;
    });
    if (incL > 0) b.add(PartRole::Incendiary, "incendiary", inc0, cf, jIn, [](double) { return 0.0; });
    b.add(PartRole::Core, spec.core.material, cf, cf + cl, coreOut, [](double) { return 0.0; }).penetrator = true;
    return b.finish(cf, cf + cl, cD);
}

} // namespace

bool isDartRod(const Penetrator& p) {
    return p.kind == PenetratorKind::LongRod && p.diameter > 0 && p.length >= kDartMinLd * p.diameter;
}

const char* shapeStyleId(ShapeStyle s) {
    for (const StyleName& n : kStyles)
        if (n.style == s) return n.id;
    return "ap";
}

std::optional<ShapeStyle> parseShapeStyle(std::string_view id) {
    for (const StyleName& n : kStyles)
        if (id == n.id) return n.style;
    return std::nullopt;
}

const char* partRoleId(PartRole r) {
    switch (r) {
    case PartRole::Body: return "body";
    case PartRole::Core: return "core";
    case PartRole::Cap: return "cap";
    case PartRole::Windshield: return "windshield";
    case PartRole::Jacket: return "jacket";
    case PartRole::Sleeve: return "sleeve";
    case PartRole::Band: return "band";
    case PartRole::Fins: return "fins";
    case PartRole::Tracer: return "tracer";
    case PartRole::Filler: return "filler";
    case PartRole::Fuze: return "fuze";
    case PartRole::Case: return "case";
    case PartRole::Nose: return "nose";
    case PartRole::Motor: return "motor";
    case PartRole::Incendiary: return "incendiary";
    case PartRole::Liner: return "liner";
    case PartRole::Propellant: return "propellant";
    }
    return "body";
}

// he_missile: the nose shell with the fuze in its tip, the warhead case round its filler, then the
// motor tube closed by a nozzle plate round a bored grain of propellant, and the fins at the tail.
// All of it is seeded, so a missile that hits crumples from the nose to the tail. The grain carries
// whatever of the round's mass the other parts do not (the propellant left after burnout is a
// modelling choice; `warnings` says when the other parts and a full grain cannot make the mass).
ProjectileShape buildMissile(const ShapeSpec& spec, const Penetrator& p) {
    const double D = orDefault(spec.diameter, p.diameter), R = 0.5 * D;
    const double L = std::max(orDefault(spec.length, p.length), 3 * D);
    const double noseLen = std::min(orDefault(spec.noseLength, 1.5 * D), 0.3 * L);
    const double whLen = std::max(std::min(orDefault(spec.warheadLength, 3 * D), L - noseLen - D), 0.5 * D);
    const double wall = std::min(orDefault(spec.caseWall, 0.06 * D), 0.4 * R);
    const double whEnd = noseLen + whLen;
    const double nw = std::min(std::max(0.03 * D, 0.002), 0.4 * R);    // nose shell
    const double tm = std::min(std::max(0.04 * R, 0.0008), 0.4 * R);   // motor tube
    const double plate = std::min(0.3 * R, 0.25 * (L - whEnd)), throat = std::min(0.45 * R, R - tm);
    const double gOut = R - tm, g1 = L - plate;                        // the grain: whEnd .. g1
    const Nose nose = noseOfLength(noseLen, R);

    double f0 = noseLen + wall, f1 = whEnd - wall, fr = 0;
    if (!spec.filler.explosive.empty() && f1 > f0) {
        fr = std::min(spec.filler.diameter > 0 ? 0.5 * spec.filler.diameter : R - wall, R - wall);
        if (spec.filler.length > 0) f1 = std::min(f1, f0 + spec.filler.length);
    }
    const double finTip = std::max(R, 0.5 * orDefault(spec.fins.span, 2.5 * D));
    const double root = std::min(orDefault(spec.fins.rootChord, 1.2 * D), L - whEnd);
    const double tip = std::min(orDefault(spec.fins.tipChord, 0.8 * D), root);
    const double sweep = spec.fins.sweep < 0 ? root - tip : std::clamp(spec.fins.sweep, 0.0, root - tip);

    // The round with a grain bored to `bore` (< 0: no grain).
    const auto make = [&](double bore) {
        Builder b(L, R);
        b.nose(nose);
        const double fz0 = 0.4 * noseLen, fzR = 0.25 * R;
        const auto hollow = [=](double x) { return std::max(0.0, nose.r(x) - nw); };
        b.add(PartRole::Nose, spec.noseMaterial, 0, noseLen, [=](double x) { return nose.r(x); }, hollow);
        b.add(PartRole::Fuze, "gilding_metal", fz0, noseLen, [=](double x) { return std::min(fzR, hollow(x)); },
              [](double) { return 0.0; });
        b.add(PartRole::Case, spec.bodyMaterial, noseLen, whEnd, [=](double) { return R; },
              [=](double x) { return fr > 0 && x > f0 && x < f1 ? fr : 0.0; })
            .penetrator = true;
        if (fr > 0) {
            Def& filler = b.add(PartRole::Filler, "explosive:" + spec.filler.explosive, f0, f1,
                                [=](double) { return fr; }, [](double) { return 0.0; });
            filler.density = spec.filler.density;
        }
        b.add(PartRole::Motor, "light_alloy", whEnd, L, [=](double) { return R; },
              [=](double x) { return x > g1 ? throat : gOut; });
        b.point(g1);
        if (bore >= 0 && g1 > whEnd)
            b.add(PartRole::Propellant, "propellant", whEnd, g1, [=](double) { return gOut; },
                  [=](double) { return bore; });
        if (root > 0) {
            const double a = L - root, a1 = a + sweep, a2 = a1 + tip;
            b.add(PartRole::Fins, "light_alloy", a, L,
                  [=](double x) {
                      if (x < a1) return sweep > 0 ? R + (finTip - R) * (x - a) / sweep : finTip;
                      if (x <= a2) return finTip;
                      return finTip - (finTip - R) * (x - a2) / std::max(L - a2, 1e-300);
                  },
                  [=](double) { return R; });
            b.point(a1);
            b.point(a2);
        }
        return b.finish(0, L, D);
    };

    double bore = 0.5 * gOut;  // no mass given: a typical bored grain
    std::vector<std::string> warnings;
    if (spec.massKg > 0 && g1 > whEnd) {
        double rest = 0;
        for (const ShapePart& part : make(-1).parts) {
            double rho = part.density;
            if (!(rho > 0)) {
                if (part.material.empty()) rho = p.density;
                else if (const std::optional<MpmMaterial> m = mpmPartMaterial(part.material)) rho = m->density;
            }
            rest += partMass(part, rho);
        }
        const double rhoGrain = mpmPartMaterial("propellant")->density, ell = g1 - whEnd;
        const double ring = (spec.massKg - rest) / (rhoGrain * kPi * ell);  // gOut^2 - bore^2 the rest needs, m2
        char buf[200];
        if (ring <= 0) {
            bore = -1;
            std::snprintf(buf, sizeof(buf),
                          "The %.2f kg round is lighter than its other parts (%.2f kg), so it has no propellant.",
                          spec.massKg, rest);
            warnings.push_back(buf);
        } else if (ring >= gOut * gOut) {
            bore = 0;
            if (ring > 1.0001 * gOut * gOut) {
                std::snprintf(buf, sizeof(buf),
                              "The %.2f kg round is heavier than its parts (%.2f kg) and a full grain; the "
                              "simulated mass is off.",
                              spec.massKg, rest);
                warnings.push_back(buf);
            }
        } else {
            bore = std::sqrt(gOut * gOut - ring);
        }
    }
    ProjectileShape s = make(bore);
    s.warnings = std::move(warnings);
    return s;
}

// heat_rocket: an RPG round or a HEAT missile. The hollow nose gives the built-in standoff, so it
// runs from the tip to the cone's mouth; the copper cone (the penetrator part: what the jet model
// describes) sits in the explosive, which carries on behind its apex to the case's base plug; then a
// taper down to the burned-out motor tube with its nozzle, and the fins at the tail. All seeded.
ProjectileShape buildHeatRocket(const ShapeSpec& spec, const Penetrator& p) {
    const double cd = p.coneDiameter, Rl = 0.5 * cd, h = cd;
    const double Lp = std::max(p.standoff, 0.0) * cd;
    const double D = std::max(orDefault(spec.diameter, 1.15 * cd), 1.04 * cd), R = 0.5 * D;
    const double wall = std::min(orDefault(spec.caseWall, 0.03 * cd), 0.1 * R), Rc = R - wall;
    const double tw = 0.03 * cd;                 // liner wall
    const double fx1 = Lp + h + 0.4 * cd;        // the explosive's back
    const double plugEnd = fx1 + 0.12 * cd;
    const double rm = std::min(0.5 * orDefault(spec.motorDiameter, 0.5 * D), R);
    const double taperEnd = plugEnd + std::max(0.3 * D, 1.2 * (R - rm));
    const double L = std::max(orDefault(spec.length, Lp + 5 * cd), taperEnd + 2 * D);
    const double tm = std::min(std::max(0.06 * rm, 0.012 * D), 0.5 * rm);  // motor wall
    Builder b(L, R);
    if (Lp > 1e-9 * cd) {
        const Nose nose = noseOfLength(Lp, R);
        const double nw = 0.03 * cd;
        b.nose(nose);
        b.add(PartRole::Nose, spec.noseMaterial, 0, Lp, [=](double x) { return nose.r(x); },
              [=](double x) { return std::max(0.0, nose.r(x) - nw); });
    }
    const auto linerIn = [=](double x) { return std::max(0.0, Rl * (1 - (x - Lp) / h)); };
    const auto linerOut = [=](double x) { return std::min(Rc, linerIn(x) + tw); };
    b.add(PartRole::Liner, "", Lp, Lp + h, linerOut, linerIn).penetrator = true;
    Def& filler = b.add(PartRole::Filler, "explosive:" + orMaterial(spec.filler.explosive, "comp_b"), Lp, fx1,
                        [=](double) { return Rc; }, [=](double x) { return x < Lp + h ? linerOut(x) : 0.0; });
    filler.density = spec.filler.density;
    b.point(Lp + h);
    const auto caseOut = [=](double x) { return x <= plugEnd ? R : R + (rm - R) * (x - plugEnd) / (taperEnd - plugEnd); };
    b.add(PartRole::Case, orMaterial(spec.bodyMaterial, "case_steel"), Lp, taperEnd, caseOut, [=](double x) {
        if (x < fx1) return Rc;
        if (x < plugEnd) return 0.0;
        return std::max(0.0, caseOut(x) - wall);
    });
    b.point(plugEnd);
    const double plate = std::min(0.3 * rm, 0.25 * (L - taperEnd)), throat = std::min(0.45 * rm, rm - tm);
    b.add(PartRole::Motor, "case_steel", taperEnd, L, [=](double) { return rm; },
          [=](double x) { return x > L - plate ? throat : rm - tm; });
    b.point(L - plate);
    const double finTip = std::max(rm, 0.5 * orDefault(spec.fins.span, 1.3 * D));
    const double root = std::min(orDefault(spec.fins.rootChord, 0.6 * D), L - taperEnd);
    const double tip = std::min(orDefault(spec.fins.tipChord, 0.35 * D), root);
    const double sweep = spec.fins.sweep < 0 ? root - tip : std::clamp(spec.fins.sweep, 0.0, root - tip);
    if (root > 0 && finTip > rm) {
        const double a = L - root, a1 = a + sweep, a2 = a1 + tip;
        b.add(PartRole::Fins, "light_alloy", a, L,
              [=](double x) {
                  if (x < a1) return sweep > 0 ? rm + (finTip - rm) * (x - a) / sweep : finTip;
                  if (x <= a2) return finTip;
                  return finTip - (finTip - rm) * (x - a2) / std::max(L - a2, 1e-300);
              },
              [=](double) { return rm; });
        b.point(a1);
        b.point(a2);
    }
    return b.finish(Lp, Lp + h, cd);
}

ProjectileShape buildShape(const ShapeSpec& spec, const Penetrator& p) {
    switch (spec.style) {
    case ShapeStyle::HeMissile: return buildMissile(spec, p);
    case ShapeStyle::HeatRocket: return buildHeatRocket(spec, p);
    case ShapeStyle::Dart: return buildDart(spec, p);
    case ShapeStyle::Apcr: return buildApcr(spec, p);
    case ShapeStyle::Apds: return buildApds(spec, p);
    case ShapeStyle::Bullet: return buildBullet(spec, p);
    default: return buildShell(spec, p);
    }
}

ProjectileShape shapeForCase(const ShapeSpec* catalogue, const Penetrator& p) {
    if (catalogue) {
        ShapeSpec s = *catalogue;
        const double sx = s.refLength > 0 && p.length > 0 ? p.length / s.refLength : 1;
        const double sr = s.refDiameter > 0 && p.diameter > 0 ? p.diameter / s.refDiameter : 1;
        if (sx != 1) {
            for (double* v : {&s.length, &s.boatTail, &s.band.fromBase, &s.band.width, &s.cap.length,
                              &s.windshield.length, &s.core.length, &s.incendiary.length, &s.filler.length,
                              &s.fins.rootChord, &s.fins.tipChord, &s.tracer.length, &s.dartCap.length,
                              &s.warheadLength, &s.noseLength})
                *v *= sx;
            for (double* v : {&s.core.fromNose, &s.filler.fromNose, &s.fins.sweep})
                if (*v >= 0) *v *= sx;
        }
        if (sr != 1) {
            for (double* v : {&s.diameter, &s.boatTailDiameter, &s.windshield.wall, &s.core.diameter,
                              &s.jacket.thickness, &s.filler.diameter, &s.fins.span, &s.tracer.diameter,
                              &s.dartCap.wall, &s.caseWall, &s.motorDiameter})
                *v *= sr;
        }
        s.massKg *= sx * sr * sr;
        s.refLength = p.length;
        s.refDiameter = p.diameter;
        return buildShape(s, p);
    }
    ShapeSpec s;
    if (p.kind == PenetratorKind::ShapedCharge) {
        // The default rocket around the cone (its diameter is the cone's business, not p.diameter).
        s.style = ShapeStyle::HeatRocket;
        s.filler.explosive = "comp_b";
        return buildShape(s, p);
    }
    s.diameter = p.diameter;
    if (isDartRod(p)) {
        s.style = ShapeStyle::Dart;
        s.length = p.length;
    } else if (p.kind == PenetratorKind::LongRod) {
        s.noseCrh = 0.0;
        s.length = p.length;
    } else {
        s.length = std::max(p.length, p.diameter);
    }
    return buildShape(s, p);
}

double partArea(const ShapePart& part) {
    double a = 0;
    for (size_t k = 1; k < part.stations.size(); ++k) {
        const ShapeStation &s0 = part.stations[k - 1], &s1 = part.stations[k];
        a += (s1.x - s0.x) * ((s0.rOuter - s0.rInner) + (s1.rOuter - s1.rInner));
    }
    return a;
}

double partVolume(const ShapePart& part) {
    if (part.role == PartRole::Fins) {
        const double D = part.stations.empty() ? 0 : 2 * part.stations.front().rInner;
        return 0.5 * partArea(part) * 4 * 0.1 * D;
    }
    double v = 0;
    for (size_t k = 1; k < part.stations.size(); ++k) {
        const ShapeStation &s0 = part.stations[k - 1], &s1 = part.stations[k];
        const double a = s0.rOuter, b = s1.rOuter, c = s0.rInner, d = s1.rInner;
        v += kPi * (s1.x - s0.x) * ((a * a + a * b + b * b) - (c * c + c * d + d * d)) / 3;
    }
    return v;
}

double partMass(const ShapePart& part, double materialDensity) {
    return partVolume(part) * (part.density > 0 ? part.density : materialDensity);
}

bool partRadiiAt(const ShapePart& part, double x, double& rInner, double& rOuter) {
    const std::vector<ShapeStation>& st = part.stations;
    if (st.empty() || x < st.front().x || x > st.back().x) return false;
    for (size_t k = 1; k < st.size(); ++k) {
        const ShapeStation &a = st[k - 1], &b = st[k];
        if (x > b.x) continue;
        const double dx = b.x - a.x, t = dx > 0 ? std::clamp((x - a.x) / dx, 0.0, 1.0) : 0.0;
        rOuter = a.rOuter + t * (b.rOuter - a.rOuter);
        rInner = a.rInner + t * (b.rInner - a.rInner);
        return true;
    }
    rOuter = st.back().rOuter;
    rInner = st.back().rInner;
    return true;
}

} // namespace phys
