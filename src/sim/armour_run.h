// One case of the armour workspace: a projectile against a stack of catalogue plates (CPU only).
// Adds the summary figures shown beside the animation: semi-infinite RHA depth, ballistic limit,
// residual penetration into an RHA witness block and the depth-of-penetration (DoP) efficiencies
// of the stack. Terminal ballistics of generic plates only.
#pragma once

#include "data/projectile_db.h"
#include "physics/fuze.h"
#include "physics/penetration.h"

#include <vector>

struct ArmourCase {
    phys::Penetrator penetrator;
    double velocity = 1500;             // m/s (ignored for shaped charges)
    double obliquityDeg = 0;            // from the plate normal, applies to every plate; < 0 leans the other way
    std::vector<PresetLayer> layers;    // materials index ProjectileDb::materials()
};

struct ArmourOutcome {
    phys::PenetrationResult result;
    std::vector<phys::ArmourLayer> layers;
    double rhaSemiInfinite = 0;   // m, normal impact on a semi-infinite RHA block at the case velocity
    double ballisticLimit = 0;    // m/s; 0 = not applicable (jets), < 0 = not perforated at 3000 m/s
    double witnessDepth = 0;      // m along the shot line into an RHA block directly behind the stack
    double witnessReference = 0;  // m along the shot line into the same block with no stack in front
    double lineOfSight = 0;       // m of plate material along the shot line (gaps excluded)
    double arealDensity = 0;      // kg/m2 of plate material along the shot line
    double rhaEquivalent = 0;     // m along the shot line: witnessReference - witnessDepth
    bool rhaEquivalentIsBound = false;  // stack not perforated: rhaEquivalent is only a lower bound
    double thicknessEfficiency = 0;     // rhaEquivalent / lineOfSight
    double massEfficiency = 0;          // RHA areal density of rhaEquivalent / arealDensity
    phys::FuzeEvent fuze;               // fuzed rounds: trigger and burst point (result.endTime covers it)
};

// Physics layers for catalogue layers (invalid material indices fall back to RHA properties).
std::vector<phys::ArmourLayer> physicsLayers(const ProjectileDb& db, const std::vector<PresetLayer>& layers);

ArmourOutcome evaluateArmour(const ProjectileDb& db, const ArmourCase& c);
