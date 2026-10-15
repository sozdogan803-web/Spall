#include "sim/witness_readout.h"

#include "data/armour_blocks.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kSheetMaxThickness = 0.005;  // m
constexpr double kSheetMinGap = 0.02;         // m, behind the layer in front

}  // namespace

bool isWitnessSheet(const ArmourCase& c, int layer) {
    if (layer < 1 || layer >= static_cast<int>(c.layers.size())) return false;
    const PresetLayer& l = c.layers[static_cast<size_t>(layer)];
    return l.thickness <= kSheetMaxThickness && l.gap >= kSheetMinGap && !isBlockLayer(l);
}

WitnessReadout measureWitness(const MpmScene& scene, const std::vector<MpmParticleState>& particles, int layer,
                              const MpmMeasure& measure) {
    WitnessReadout w;
    w.layer = layer;
    if (layer < 0 || particles.size() != scene.particles.size()) return w;

    const double reach = 2 * scene.dx;  // how close to the shot line, and how far apart, one hole's edges lie
    std::vector<double> failedY;        // where the failed sheet material started, across the shot line
    double sheetBack = 0, nearest = 0;
    bool found = false;
    for (size_t i = 0; i < particles.size(); ++i) {
        const MpmSeed& seed = scene.particles[i];
        if ((seed.flags & kMpmProjectile) || seed.layer != layer) continue;
        const MpmParticleState& p = particles[i];
        if (p.flags & (kMpmInactive | kMpmBad)) continue;
        sheetBack = found ? std::max<double>(sheetBack, seed.x) : seed.x;
        found = true;
        if (p.flags & kMpmFailed) {
            failedY.push_back(seed.y);
            continue;
        }
        w.bulge = std::max(w.bulge, p.x - seed.x);
        w.peakSpeed = std::max(w.peakSpeed, p.vx);
    }
    if (!found) return w;
    w.valid = true;

    // The hole is the run of failed material that touches the shot line: sorted by where it
    // started, it runs on while the gaps between neighbours stay within a couple of cells.
    std::sort(failedY.begin(), failedY.end());
    size_t seedAt = failedY.size();
    for (size_t i = 0; i < failedY.size(); ++i)
        if (std::abs(failedY[i]) <= reach && (seedAt == failedY.size() || std::abs(failedY[i]) < nearest)) {
            seedAt = i;
            nearest = std::abs(failedY[i]);
        }
    if (seedAt < failedY.size()) {
        size_t lo = seedAt, hi = seedAt;
        while (lo > 0 && failedY[lo] - failedY[lo - 1] <= reach) --lo;
        while (hi + 1 < failedY.size() && failedY[hi + 1] - failedY[hi] <= reach) ++hi;
        w.holeWidth = failedY[hi] - failedY[lo] + (scene.spacing > 0 ? scene.spacing : 0.5 * scene.dx);
    }

    if (measure.intact > 0 && measure.nose > sheetBack) w.residualVelocity = measure.velocity;
    return w;
}
