// Readouts of a thin witness sheet in the deformation simulation: how wide the hole in it is, how
// far it bulged and how fast the penetrator is moving past it. Material state only.
#pragma once

#include "sim/armour_run.h"
#include "sim/mpm_scene.h"
#include "sim/mpm_solver.h"

#include <vector>

// A layer past the first that is at most 5 mm thick, one tile across and set at least 20 mm
// behind the layer before it.
bool isWitnessSheet(const ArmourCase& c, int layer);

struct WitnessReadout {
    bool valid = false;          // the particles match the scene and the layer has material
    int layer = 0;
    double holeWidth = 0;        // m: across the failed sheet material around the shot line (0: no hole)
    double bulge = 0;            // m: furthest forward move of intact sheet material
    double peakSpeed = 0;        // m/s: fastest forward speed of the sheet
    double residualVelocity = 0; // m/s: the penetrator's, once it is past the sheet (0: not past)
};

// `particles` is MpmSolver::readParticles() (the same order as scene.particles).
WitnessReadout measureWitness(const MpmScene& scene, const std::vector<MpmParticleState>& particles, int layer,
                              const MpmMeasure& measure);
