#include "sim/mpm_scene.h"

#include "data/armour_blocks.h"
#include "physics/gurney.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct QualityInfo {
    const char* label;
    int budget;
    int cellsPerDiameter;
    int maxCells;
    int perSide;     // particles to a cell side: perSide^2 a cell
    int maxPerSide;  // seeded up to this where the grid, not the budget, sets the cell
};
// Ultra to Ludicrous ask for more cells across a rod than their grid can hold, so the grid and the
// budget set the cell; a case whose grid is mostly empty then seeds more particles to a cell, up to
// maxPerSide, until the budget is spent.
constexpr QualityInfo kQualities[] = {{"Low", 150000, 8, kMpmMaxCells, 2, 2},
                                      {"Medium", 600000, 14, kMpmMaxCells, 2, 2},
                                      {"High", 1600000, 22, kMpmMaxCells, 2, 2},
                                      {"Ultra", 4000000, 128, 3 * kMpmMaxCells / 2, 3, 6},
                                      {"Insane", 12000000, 256, 2 * kMpmMaxCells, 4, 9},
                                      {"Ludicrous", 100000000, 512, 3 * kMpmMaxCells, 5, 16}};
static_assert(std::size(kQualities) == static_cast<size_t>(MpmQuality::Count));

const QualityInfo& info(MpmQuality q) {
    return kQualities[std::clamp(static_cast<int>(q), 0, static_cast<int>(MpmQuality::Count) - 1)];
}

// A part's own material: a filler from the explosives table (a generic TNT-like one if unknown),
// else the part material, else the rod's; the part's density override applied.
phys::MpmMaterial partMaterial(const ProjectileDb& db, const phys::ShapePart& part, RodMaterial rod) {
    constexpr std::string_view kExplosive = "explosive:";
    phys::MpmMaterial m;
    if (part.material.rfind(kExplosive, 0) == 0) {
        const std::string_view id = std::string_view(part.material).substr(kExplosive.size());
        double density = 1650, speed = 6900, energy = 4e6;
        for (const Explosive& e : db.explosives())
            if (e.id == id) {
                density = e.density;
                speed = e.detonationVelocity;
                energy = 4e6 * e.reFactor;
            }
        m = phys::mpmFiller(density, speed, energy);
    } else {
        m = phys::mpmPartMaterial(part.material).value_or(rodMaterialMpm(rod));
    }
    if (part.density > 0) m.density = part.density;
    return m;
}

}  // namespace

bool mpmIsDart(const phys::Penetrator& p) {
    return phys::isDartRod(p);
}

const char* mpmQualityLabel(MpmQuality q) { return info(q).label; }
int mpmParticleBudget(MpmQuality q) { return info(q).budget; }
int mpmCellsPerDiameter(MpmQuality q) { return info(q).cellsPerDiameter; }
int mpmMaxCells(MpmQuality q) { return info(q).maxCells; }
int mpmParticlesPerSide(MpmQuality q) { return info(q).perSide; }
int mpmMaxParticlesPerSide(MpmQuality q) { return info(q).maxPerSide; }
bool mpmBestPrerendered(MpmQuality q) { return q >= MpmQuality::Insane && q < MpmQuality::Count; }

size_t mpmVideoBytes(size_t particles, int nx, int ny) {
    constexpr size_t kGridNodeBytes = 4 * sizeof(int32_t) + 2 * sizeof(float);
    constexpr size_t kPartialsBytes = 6 * 4 * sizeof(float);  // per 256 particles
    const size_t nodes = static_cast<size_t>(std::max(nx, 0)) * static_cast<size_t>(std::max(ny, 0));
    return particles * kMpmParticleBytes + nodes * kGridNodeBytes + (particles + 255) / 256 * kPartialsBytes;
}

std::string mpmFitProblem(size_t particles, int nx, int ny, size_t availBytes, size_t maxBlockBytes) {
    constexpr double kGB = 1073741824.0;
    constexpr size_t kGridBufferBytesPerNode = 4 * sizeof(int32_t);
    char buf[160];
    const size_t need = mpmVideoBytes(particles, nx, ny);
    if (availBytes > 0 && static_cast<double>(need) > 0.85 * static_cast<double>(availBytes)) {
        std::snprintf(buf, sizeof(buf), "needs about %.1f GB of video memory and %.1f GB is free", need / kGB, availBytes / kGB);
        return buf;
    }
    const size_t nodes = static_cast<size_t>(std::max(nx, 0)) * static_cast<size_t>(std::max(ny, 0));
    const size_t biggest = std::max(particles * kMpmParticleBytes, nodes * kGridBufferBytesPerNode);
    if (maxBlockBytes > 0 && biggest > maxBlockBytes) {
        std::snprintf(buf, sizeof(buf), "needs a %.1f GB buffer and the card allows %.1f GB in one", biggest / kGB,
                      maxBlockBytes / kGB);
        return buf;
    }
    return {};
}

void mpmPackMaterial(const phys::MpmMaterial& m, float* out) {
    double v[kMpmMaterialFloats] = {m.density,         m.shear,        m.bulk,          m.yield,
                                    m.hardening,       m.hardeningExp, m.softeningExp,  m.meltK,
                                    m.heat,            m.failureStrain, m.spall,        phys::mpmSoundSpeed(m),
                                    m.detonationSpeed, m.energy,       m.productsGamma, m.initiation};
    v[16] = static_cast<double>(static_cast<int>(m.model));
    switch (m.model) {
    case phys::MpmModel::Ceramic: {
        const phys::MpmHelState h = phys::mpmHelState(m);
        v[17] = h.strength;
        v[18] = h.pressure;
        v[19] = m.tensile;
        v[20] = m.intactA;
        v[21] = m.intactN;
        v[22] = m.fracturedB;
        v[23] = m.fracturedM;
        v[24] = m.fracturedMax * h.strength;
        v[25] = m.fractureD1;
        v[26] = m.fractureD2;
        break;
    }
    case phys::MpmModel::Elastomer:
        v[20] = m.viscosity;
        v[21] = m.viscousLimit;
        break;
    case phys::MpmModel::Composite:
        v[20] = m.fibreModulus;
        v[21] = m.fibreFailureStrain;
        v[22] = m.delamTension;
        v[23] = m.delamShear;
        break;
    default:
        // Metal: the brittle crack strength, 1 / Weibull modulus and 1 / Gamma(1 + 1 / modulus).
        if (m.brittleStress > 0) {
            const double invM = 1.0 / std::max(m.flawModulus, 1.0);
            v[20] = m.brittleStress;
            v[21] = invM;
            v[22] = 1.0 / std::tgamma(1.0 + invM);
        }
        break;
    }
    for (int i = 0; i < kMpmMaterialFloats; ++i) out[i] = static_cast<float>(v[i]);
}

MpmScene buildMpmScene(const ProjectileDb& db, const ArmourCase& c, RodMaterial rod, const phys::ProjectileShape& shapeIn,
                       const ArmourOutcome& out, MpmQuality quality, bool softMounts) {
    MpmScene s;
    s.quality = quality;
    const phys::Penetrator& p = c.penetrator;
    const std::vector<phys::LayerResult>& layers = out.result.layers;
    const bool jet = p.kind == phys::PenetratorKind::ShapedCharge;
    if (layers.empty() || layers.size() != c.layers.size()) {
        s.message = "No plates to simulate.";
        return s;
    }
    if (jet ? !(p.coneDiameter > 0) : !(c.velocity > 0) || !(p.diameter > 0)) {
        s.message = jet ? "The charge needs a cone diameter." : "The projectile needs a speed and a diameter.";
        return s;
    }

    // A negative angle is the same event mirrored across the shot line; the view mirrors it.
    const double theta = std::clamp(std::abs(c.obliquityDeg), 0.0, phys::kMaxObliquityDeg) * kPi / 180.0;
    const double sinT = std::sin(theta), cosT = std::cos(theta), tanT = std::tan(theta);
    // A jet starts at the engineering model's own start ts (times since formation): elements of
    // velocity v at x = v ts - S, its half-width at the tail jetHalf (see the seeding below).
    const phys::JetParameters jp = phys::jetParameters(p);
    const double S = jp.virtualOrigin, t0 = S / jp.tipVelocity;
    const double ts = out.result.startTime > 0 ? out.result.startTime : 0.25 * t0;
    const double jetHalf = 2.4 * jp.jetRadius * t0 / ts;
    const double D = jet ? kMpmJetReference * p.coneDiameter : p.diameter;
    // A kinetic round's parts (a generic shape when the caller has none), x from its tip backwards;
    // the penetrator part is the engineering model's, from `front` to penetratorBack.
    const phys::ProjectileShape shape = jet                    ? phys::ProjectileShape{}
                                        : !shapeIn.parts.empty() ? shapeIn
                                                                 : phys::shapeForCase(nullptr, p);
    const double front = shape.penetratorFront;
    const double len = jet ? (jp.tipVelocity - jp.minVelocity) * ts : shape.penetratorBack - front;
    const double R = 0.5 * D;
    s.diameter = D;
    s.velocity = jet ? jp.tipVelocity : c.velocity;
    s.dart = mpmIsDart(p);

    // A kinetic round that stops in the stack never feels the block much past its reach: the stack
    // is cut there along the shot line (layers beyond it dropped) and the cut face held, as if the
    // block went on. A rifle core in 600 mm of steel then gets the grid of a thin plate.
    const phys::PenetrationResult& r = out.result;
    double cut = 1e30;  // m along the shot line from the first face
    if (!jet && !r.perforated) {
        cut = r.depth + std::max(r.depth, 8 * D);
        if (out.fuze.triggered) cut = std::max(cut, out.fuze.burstDepth + 3 * D);
    }

    // Plates: rectangles in their own frame (u along the normal n from the front face, sigma
    // along the face t), centred on where the shot line crosses each plate's mid-plane.
    double los = 0, area = jet ? 2.4 * jp.jetRadius * t0 * (jp.tipVelocity - jp.minVelocity) : 0;
    for (const phys::ShapePart& part : shape.parts)
        if (part.simulated) area += phys::partArea(part);
    for (const phys::LayerResult& l : layers) los += std::max(0.0, std::min(l.back, cut) - l.front);
    const double H = std::min(0.5, std::max(8 * D, 0.6 * los));
    s.plateHalfExtent = H;
    std::vector<double> thick(layers.size());
    std::vector<char> dropped(layers.size(), 0), held(layers.size(), 0);
    for (size_t i = 0; i < layers.size(); ++i) {
        // The preset's own thickness: a tile layer the shot line passes between is air along the
        // line (no thickness in the engineering model) but its tiles either side are still there.
        thick[i] = std::max(0.0, c.layers[i].thickness);
        if (layers[i].front >= cut) {
            dropped[i] = 1;
            thick[i] = 0;
        } else if (!isBlockLayer(c.layers[i]) && (cut - layers[i].front) * cosT < thick[i]) {
            held[i] = 1;
            thick[i] = (cut - layers[i].front) * cosT;
        }
        area += thick[i] * 2 * H;
    }
    s.simThickness = thick;
    for (size_t i = 0; i < layers.size(); ++i)
        if (dropped[i] || held[i]) s.cutDepth = cut;
    const double nX = cosT, nY = sinT, tX = -sinT, tY = cosT;
    // How far the projectile reaches ahead of the penetrator's front towards the first face,
    // measured as x + y tan(theta) (the face is x + y tan(theta) = 0): the furthest corner of any
    // simulated part (a cap's tip; at obliquity the upper corner of a nose or of a fin).
    double lead = 0;
    for (const phys::ShapePart& part : shape.parts)
        if (part.simulated)
            for (const phys::ShapeStation& st : part.stations) lead = std::max(lead, front - st.x + st.rOuter * tanT);
    auto plateCorner = [&](size_t i, double u, double sigma) {
        const double mid = -0.5 * thick[i] * tanT;
        return std::array<double, 2>{layers[i].front + u * nX + (mid + sigma) * tX, u * nY + (mid + sigma) * tY};
    };

    // Residual travel behind the stack, for the domain: the animation can run on past the model's
    // end, so room for 2.5 times as long (at most three penetrator lengths, at least 3 D).
    const double tEnd = std::max(1.25 * r.endTime, 1e-6);
    const double travel = !r.perforated ? 3 * D
                          : jet       ? std::max(3.0 * p.coneDiameter, 3 * D)
                                      : std::max(3 * D, std::min(r.residualVelocity * std::max(2.5 * tEnd - r.exitTime, 0.0), 3 * len));

    // Grid spacing: the quality's cells per diameter, coarsened for the particle budget (the base
    // particles per cell) and the grid size.
    const int baseSide = mpmParticlesPerSide(quality);
    double dx = std::max(D / mpmCellsPerDiameter(quality), std::sqrt(static_cast<double>(baseSide * baseSide) * area / mpmParticleBudget(quality)));
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    for (int iter = 0; iter < 4; ++iter) {
        const double pre = kMpmPrerollCells * dx;
        const double margin = 4 * dx;
        xmin = jet ? jp.minVelocity * ts - S : -pre - lead - (shape.length - front);
        xmax = std::min(layers.back().back, cut) + travel + D;
        // A fuzed round's burst stays inside the grid.
        if (!jet && out.fuze.triggered) xmax = std::max(xmax, out.fuze.burstDepth + 3 * D);
        ymin = -std::max(3 * D, (jet ? jetHalf : std::max(R, 0.5 * shape.diameter)) + margin);
        ymax = -ymin;
        for (size_t i = 0; i < layers.size(); ++i)
            for (double u : {0.0, thick[i]})
                for (double sg : {-H, H}) {
                    if (dropped[i]) continue;
                    const auto q = plateCorner(i, u, sg);
                    xmin = std::min(xmin, q[0]);
                    xmax = std::max(xmax, q[0]);
                    ymin = std::min(ymin, q[1]);
                    ymax = std::max(ymax, q[1]);
                }
        // Room for the plates to bulge, petal and throw debris sideways and back before it leaves
        // the grid (what leaves it still flies on, see g2p.comp).
        xmin -= margin + 2 * D;
        xmax += margin;
        ymin -= margin + H;
        ymax += margin + H;
        const double need = std::max(xmax - xmin, ymax - ymin) / (mpmMaxCells(quality) - 2);
        if (need <= dx) break;
        dx = need * 1.001;
    }
    s.dx = dx;
    // The finest levels spend what the grid left of the budget on more particles to a cell.
    int perSide = baseSide;
    if (area > 0 && mpmMaxParticlesPerSide(quality) > baseSide)
        perSide = std::clamp(static_cast<int>(std::sqrt(0.95 * mpmParticleBudget(quality) * dx * dx / area)), baseSide,
                             mpmMaxParticlesPerSide(quality));
    s.particlesPerSide = perSide;
    s.spacing = dx / perSide;
    s.originX = xmin;
    s.originY = ymin;
    s.nx = static_cast<int>(std::ceil((xmax - xmin) / dx)) + 1;
    s.ny = static_cast<int>(std::ceil((ymax - ymin) / dx)) + 1;

    // Materials: the penetrator (a dart's rod, else its part's own material, at the case's
    // density), the plates, then one per other part material and density override.
    const phys::ShapePart* penPart = nullptr;
    for (const phys::ShapePart& part : shape.parts)
        if (part.penetrator && !penPart) penPart = &part;
    phys::MpmMaterial penetrator = jet ? phys::mpmLinerCopper() : rodMaterialMpm(rod);
    if (!jet && !s.dart && penPart && !penPart->material.empty()) penetrator = partMaterial(db, *penPart, rod);
    penetrator.density = jet ? p.linerDensity : p.density;
    s.materials.push_back(penetrator);
    for (const PresetLayer& l : c.layers) {
        const bool valid = l.material >= 0 && l.material < static_cast<int>(db.materials().size());
        s.materials.push_back(valid ? db.materials()[static_cast<size_t>(l.material)].mpm : phys::MpmMaterial{});
    }
    // Parts in the penetrator's own material ("") share material 0.
    std::vector<uint16_t> partIndex(shape.parts.size(), 0);
    std::vector<double> partDensity;  // each appended material's override, beside partMaterials
    for (size_t k = 0; k < shape.parts.size(); ++k) {
        const phys::ShapePart& part = shape.parts[k];
        if (!part.simulated || part.penetrator || part.material.empty()) continue;
        size_t j = 0;
        while (j < s.partMaterials.size() && !(s.partMaterials[j] == part.material && partDensity[j] == part.density)) ++j;
        if (j == s.partMaterials.size()) {
            s.partMaterials.push_back(part.material);
            partDensity.push_back(part.density);
            s.materials.push_back(partMaterial(db, part, rod));
        }
        partIndex[k] = static_cast<uint16_t>(1 + layers.size() + j);
    }
    for (const phys::MpmMaterial& m : s.materials) s.maxSoundSpeed = std::max(s.maxSoundSpeed, phys::mpmSignalSpeed(m));
    s.dt = kMpmCfl * dx / (s.maxSoundSpeed + s.velocity);
    for (const phys::MpmMaterial& m : s.materials) s.dt = std::min(s.dt, phys::mpmViscousStep(m, dx));
    s.faceTangent[0] = tX;
    s.faceTangent[1] = tY;
    // The projectile's leading point starts kMpmPrerollCells cells before the first face; the
    // penetrator's front reaches the face on the shot line at t = 0.
    const double pre = kMpmPrerollCells * dx;
    s.tStart = jet ? ts : -(pre + lead) / c.velocity;
    s.tEnd = std::max(tEnd, s.tStart + 10 * s.dt);

    // Projectile: one lattice over the whole shape, rows hx and columns hy of about the spacing,
    // fitted to the penetrator (rows from its back, columns across its diameter). Each point goes
    // to the part it falls in. Where a part is thinner than a column at some row it gets one point
    // at mid-radius on each side carrying its thickness instead, so thin jackets and walls survive.
    const double h = s.spacing;
    const int nxr = std::max(2, static_cast<int>(std::lround(len / h)));
    const int nyr = std::max(2, static_cast<int>(std::lround(D / h)));
    const double hx = len / nxr, hy = D / nyr;
    // World x of the penetrator's front at tStart.
    const double nose = -pre - lead;
    s.bodyNose = nose;
    s.bodyLength = len;
    if (jet) {
        // The stream at ts: rows across it spaced about 1.5 h at breakup, each with the model's
        // velocity (x + S) / ts. Its half-width is the engineering view's radius at t0, thinned
        // as 1 / t (a plane-strain sheet stretching as t), with the matching inflow -y / ts. At
        // breakup it parts into segments: the tip row of each fails (the neck) and each, neck included, takes a
        // lateral kick, drift x its mid velocity x a fixed pseudo-random offset. A jet that is
        // already past breakup at ts is seeded broken.
        const double tb = jp.breakupTime, spread = jp.tipVelocity - jp.minVelocity;
        const double tail = jp.minVelocity * ts - S;
        const int rows = std::max(2, static_cast<int>(std::lround(len / std::clamp(1.5 * h * ts / tb, h / 16, h))));
        const double hj = len / rows;
        const double segSpread = std::max(kMpmJetSegment * jp.jetRadius, 3 * dx) / tb;
        const bool broken = ts >= tb;
        auto segment = [&](int i) {
            const double v = (tail + (i + 0.5) * hj + S) / ts;
            return static_cast<int>(std::floor((jp.tipVelocity - v) / segSpread));
        };
        for (int i = 0; i < rows; ++i) {
            const double x = tail + (i + 0.5) * hj;
            const double v = (x + S) / ts;
            const double f = std::clamp((jp.tipVelocity - v) / spread, 0.0, 1.0);
            const double half = jp.jetRadius * (0.6 + 1.8 * f * f) * t0 / ts;
            const int ny = std::max(2, static_cast<int>(std::lround(2 * half / h)));
            const double hw = 2 * half / ny;
            const int k = segment(i);
            const bool neck = i + 1 < rows && segment(i + 1) != k;
            const double kick = jp.drift * (jp.tipVelocity - (k + 0.5) * segSpread) * phys::jetParticleOffset(k);
            uint32_t flags = kMpmProjectile | kMpmJet | (neck ? kMpmJetNeck : 0u);
            if (broken) flags |= kMpmJetBroken | (neck ? kMpmFailed : 0u);
            for (int j = 0; j < ny; ++j) {
                double y = -half + (j + 0.5) * hw, vy = -y / ts;
                if (broken) {
                    y += kick * (ts - tb);
                    vy += kick;
                }
                s.particles.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(hj * hw), 0, 0, flags,
                                       static_cast<float>(v), static_cast<float>(vy), static_cast<float>(kick),
                                       static_cast<float>(kMpmJetTemperature)});
                s.projectileArea += hj * hw;
            }
        }
        s.bodyNose = jp.tipVelocity * ts - S;
        s.bodyLength = len;
        s.jet = true;
        s.jetBreakup = tb;
        s.jetOrigin = S;
    }
    std::vector<MpmSeed> fittings;
    if (!jet) {
        const double back = shape.penetratorBack, Rs = std::max(R, 0.5 * shape.diameter);
        const int rowLo = -static_cast<int>(std::ceil((shape.length - back) / hx)) - 1;
        const int rowHi = static_cast<int>(std::ceil(back / hx)) + 1;
        const int colLo = static_cast<int>(std::floor((R - Rs) / hy)) - 1;
        const int colHi = static_cast<int>(std::ceil((R + Rs) / hy)) + 1;
        for (int i = rowLo; i <= rowHi; ++i) {
            const double xs = back - (i + 0.5) * hx;  // shape x
            if (xs <= 0 || xs >= shape.length) continue;
            const float x = static_cast<float>(nose - (xs - front));
            for (size_t k = 0; k < shape.parts.size(); ++k) {
                const phys::ShapePart& part = shape.parts[k];
                double rIn = 0, rOut = 0;
                if (!part.simulated || !phys::partRadiiAt(part, xs, rIn, rOut) || !(rOut > rIn)) continue;
                uint32_t flags = kMpmProjectile;
                if (!part.penetrator) flags |= kMpmFitting;
                if (part.role == phys::PartRole::Tracer) flags |= kMpmTracer;
                if (part.role == phys::PartRole::Filler) flags |= kMpmFiller;
                std::vector<MpmSeed>& dst = part.penetrator ? s.particles : fittings;
                auto seed = [&](double y, double vol) {
                    dst.push_back({x, static_cast<float>(y), static_cast<float>(vol), partIndex[k], 0, flags});
                    if (part.penetrator) s.projectileArea += vol;
                };
                if (rOut - rIn < hy) {
                    const double mid = 0.5 * (rIn + rOut);
                    seed(mid, hx * (rOut - rIn));
                    seed(-mid, hx * (rOut - rIn));
                    continue;
                }
                // The row's points share its exact section, 2 (rOut - rIn) hx, so a part one or
                // two columns thick keeps its mass.
                int n = 0;
                for (int j = colLo; j <= colHi; ++j) {
                    const double y = -R + (j + 0.5) * hy;
                    if (std::abs(y) >= rIn && std::abs(y) < rOut) ++n;
                }
                if (n == 0) continue;
                for (int j = colLo; j <= colHi; ++j) {
                    const double y = -R + (j + 0.5) * hy;
                    if (std::abs(y) >= rIn && std::abs(y) < rOut) seed(y, 2 * (rOut - rIn) * hx / n);
                }
            }
        }
    }
    s.bodyCount = static_cast<int>(s.particles.size());
    s.particles.insert(s.particles.end(), fittings.begin(), fittings.end());
    s.projectileCount = static_cast<int>(s.particles.size());

    // The fuze: the filler lights from its rear (base delay) or front (nose contact) on the axis.
    for (int i = 0; i < s.projectileCount; ++i)
        if (s.particles[static_cast<size_t>(i)].flags & kMpmFiller) ++s.fillerCount;
    if (s.fillerCount > 0 && p.fuze.type != phys::FuzeType::None) {
        double fillerFront = 1e30, fillerBack = -1e30;
        for (const phys::ShapePart& part : shape.parts)
            if (part.role == phys::PartRole::Filler)
                for (const phys::ShapeStation& st : part.stations) {
                    fillerFront = std::min(fillerFront, st.x);
                    fillerBack = std::max(fillerBack, st.x);
                }
        const double xs = p.fuze.type == phys::FuzeType::BaseDelay ? fillerBack : fillerFront;
        s.fuze = p.fuze;
        s.fuzeOrigin[0] = (nose - (xs - front) - s.originX) / dx;
        s.fuzeOrigin[1] = (0 - s.originY) / dx;
        double heaviest = 0;
        for (int i = 0; i < s.projectileCount; ++i) {
            const MpmSeed& q = s.particles[static_cast<size_t>(i)];
            heaviest = std::max(heaviest, static_cast<double>(static_cast<float>(s.materials[q.material].density * q.volume)));
        }
        s.fuzeMassUnit = heaviest / 4096;
        for (int i = 0; i < s.projectileCount; ++i) {
            const MpmSeed& q = s.particles[static_cast<size_t>(i)];
            s.fuzeTotalUnits += std::round(static_cast<float>(s.materials[q.material].density * q.volume) / s.fuzeMassUnit);
        }

        // The burst: Gurney's cylinder speed from the case and charge mass alongside each other,
        // tilted by Taylor's angle towards the way the detonation sweeps.
        double charge = 0, lo = 1e30, hi = -1e30;
        const phys::MpmMaterial* fillerMat = nullptr;
        for (int i = 0; i < s.projectileCount; ++i) {
            const MpmSeed& q = s.particles[static_cast<size_t>(i)];
            if (!(q.flags & kMpmFiller)) continue;
            const phys::MpmMaterial& m = s.materials[q.material];
            fillerMat = &m;
            charge += m.density * q.volume;
            const double cx = (q.x - s.originX) / dx;
            lo = std::min(lo, cx);
            hi = std::max(hi, cx);
        }
        double wall = 0;
        for (int i = 0; i < s.projectileCount; ++i) {
            const MpmSeed& q = s.particles[static_cast<size_t>(i)];
            const double cx = (q.x - s.originX) / dx;
            if ((q.flags & (kMpmFiller | kMpmFitting)) || cx < lo || cx > hi) continue;
            wall += s.materials[q.material].density * q.volume;
        }
        if (fillerMat && fillerMat->detonationSpeed > 0 && fillerMat->energy > 0 && charge > 0) {
            s.burstGurney = phys::gurneyCylinderVelocity(std::sqrt(2 * fillerMat->energy), wall, charge);
            s.burstSpeed = fillerMat->detonationSpeed;
            s.burstTaylor = phys::taylorAngle(s.burstGurney, s.burstSpeed);
            s.burstSweep = p.fuze.type == phys::FuzeType::BaseDelay ? 1 : -1;
            s.burstSpan[0] = lo;
            s.burstSpan[1] = hi;
        }
    }

    // Plates, edges anchored. The band next to each held strip stands for the mounting and the
    // plate beyond the modelled width: it yields and stretches but never fails, so the plate
    // bulges and flexes between its mounts instead of tearing free and flying off whole.
    const int ns = std::max(2, static_cast<int>(std::lround(2 * H / h)));
    const double hs = 2 * H / ns;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (dropped[i]) continue;
        const bool reactive = phys::mpmReactive(s.materials[1 + i]);
        const phys::MpmModel model = s.materials[1 + i].model;
        const int nu = std::max(2, static_cast<int>(std::lround(thick[i] / h)));
        const double hu = thick[i] / nu;
        if (isBlockLayer(c.layers[i])) {
            // Separate tiles (ERA): each its own free piece, held by nothing, so a tile the
            // round hits is thrown off on its own and its neighbours stay put.
            for (const LayerSpan& tile : layerTiles(c.layers[i], -H, H)) {
                const int nt = std::max(2, static_cast<int>(std::lround((tile.hi - tile.lo) / h)));
                const double ht = (tile.hi - tile.lo) / nt;
                for (int a = 0; a < nu; ++a)
                    for (int b = 0; b < nt; ++b) {
                        const auto q = plateCorner(i, (a + 0.5) * hu, tile.lo + (b + 0.5) * ht);
                        if (reactive) ++s.reactiveCount;
                        if (model == phys::MpmModel::Ceramic) ++s.ceramicCount;
                        if (model == phys::MpmModel::Composite) ++s.compositeCount;
                        s.particles.push_back({static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(hu * ht),
                                               static_cast<uint16_t>(1 + i), static_cast<uint16_t>(i),
                                               static_cast<uint32_t>(i) << kMpmLayerShift});
                    }
            }
            continue;
        }
        const double mount = std::max(kMpmMountCells * dx, std::min(thick[i], 0.2 * H));
        for (int a = 0; a < nu; ++a)
            for (int b = 0; b < ns; ++b) {
                const double sigma = -H + (b + 0.5) * hs;
                const auto q = plateCorner(i, (a + 0.5) * hu, sigma);
                uint32_t flags = static_cast<uint32_t>(i) << kMpmLayerShift;
                if (softMounts && std::abs(sigma) > H - 2 * dx) flags |= kMpmTethered | kMpmMount;
                else if (std::abs(sigma) > H - 2 * dx || (held[i] && (a + 0.5) * hu > thick[i] - 2 * dx)) flags |= kMpmAnchored;
                else {
                    if (std::abs(sigma) > H - 2 * dx - mount) flags |= kMpmMount;
                    if (reactive) ++s.reactiveCount;
                    if (model == phys::MpmModel::Ceramic) ++s.ceramicCount;
                    if (model == phys::MpmModel::Composite) ++s.compositeCount;
                }
                s.particles.push_back({static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(hu * hs),
                                       static_cast<uint16_t>(1 + i), static_cast<uint16_t>(i), flags});
            }
    }
    if (!jet)
        for (int i = 0; i < s.projectileCount; ++i) s.particles[static_cast<size_t>(i)].vx = static_cast<float>(c.velocity);
    if (softMounts) {
        s.softMounts = true;
        s.mountOmega = kMpmMountWave / std::sqrt(2 * dx * kMpmMountLength);
        s.mountDamping = kMpmMountDamping;
    }
    s.supported = true;
    s.message = jet ? "Plane-strain sheet: the jet is a copper sheet of unit depth with a programmed breakup, so depths "
                      "are indicative."
                    : "Plane-strain slab: the projectile is a blade of unit depth, so depths are indicative.";
    return s;
}
