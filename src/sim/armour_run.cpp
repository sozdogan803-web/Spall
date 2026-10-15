#include "sim/armour_run.h"

#include "data/armour_blocks.h"

#include <algorithm>
#include <cmath>

std::vector<phys::ArmourLayer> physicsLayers(const ProjectileDb& db, const std::vector<PresetLayer>& layers) {
    std::vector<phys::ArmourLayer> out;
    out.reserve(layers.size());
    double carried = 0;  // thickness of tiles the shot line passed between
    for (const PresetLayer& l : layers) {
        phys::ArmourLayer p;
        if (l.material >= 0 && l.material < static_cast<int>(db.materials().size()))
            p.material = db.materials()[static_cast<size_t>(l.material)].material;
        p.thickness = l.thickness;
        p.gap = l.gap + carried;
        carried = 0;
        // The shot runs down the gap between two tiles: the layer is air along the shot line,
        // and the layers behind stay where they are.
        if (!layerCoversShotLine(l)) {
            carried = std::max(0.0, l.thickness);
            p.thickness = 0;
        }
        out.push_back(p);
    }
    return out;
}

ArmourOutcome evaluateArmour(const ProjectileDb& db, const ArmourCase& c) {
    ArmourOutcome o;
    const phys::Penetrator& p = c.penetrator;
    // The sign only says which way the plates lean (the drawing is mirrored); the event is the same.
    const double obliquity = std::clamp(std::abs(c.obliquityDeg), 0.0, phys::kMaxObliquityDeg);
    o.layers = physicsLayers(db, c.layers);
    o.result = phys::simulatePenetration(p, c.velocity, obliquity, o.layers);
    o.rhaSemiInfinite = phys::rhaPenetration(p, c.velocity);
    o.ballisticLimit = phys::ballisticLimit(p, obliquity, o.layers);
    o.witnessDepth = phys::residualPenetration(p, c.velocity, obliquity, o.layers);
    o.witnessReference = phys::residualPenetration(p, c.velocity, obliquity, {});

    const double cosT = std::cos(obliquity * 3.14159265358979323846 / 180.0);
    for (const phys::ArmourLayer& l : o.layers) {
        o.lineOfSight += l.thickness / cosT;
        o.arealDensity += l.material.density * l.thickness / cosT;
    }
    o.rhaEquivalentIsBound = !o.result.perforated;
    o.rhaEquivalent = std::max(0.0, o.witnessReference - o.witnessDepth);
    if (o.lineOfSight > 0) {
        o.thicknessEfficiency = o.rhaEquivalent / o.lineOfSight;
        o.massEfficiency = o.rhaEquivalent * phys::kRhaDensity / o.arealDensity;
    }
    if (p.kind != phys::PenetratorKind::ShapedCharge && p.kind != phys::PenetratorKind::HeWarhead) {
        o.fuze = phys::fuzeEvent(p.fuze, c.velocity, o.result);
        if (o.fuze.triggered) {
            // Leave room for the filler to detonate end to end and the products to start moving.
            double end = o.fuze.burstTime + 20e-6;
            if (p.fuze.detonationSpeed > 0) end += 10 * p.fuze.fillerLength / p.fuze.detonationSpeed;
            o.result.endTime = std::max(o.result.endTime, end);
        }
    }
    return o;
}
