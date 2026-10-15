// Draws a projectile's parts (physics/projectile_shape.h) in the armour cross-section.
#pragma once

#include "physics/projectile_shape.h"

#include "imgui.h"

#include <functional>
#include <limits>

// Where the round is in the section (world m: x along the shot line, y across it).
struct ShapeDrawFrame {
    std::function<ImVec2(double, double)> toScreen;  // world -> screen (mirroring included)
    double noseX = 0;         // the penetrator's front; parts behind it are cut off here (eroded)
    double erodedLength = 0;  // the penetrator's current length; parts behind it hang off its tail
    // Parts ahead of the penetrator's front are crushed against the first face, x + slope y <= clip.
    double firstFaceClip = std::numeric_limits<double>::infinity();
    double firstFaceSlope = 0;
    double mirrorY = 1;       // -1: the view is mirrored, the lit side flips with it
    ImU32 ownColour = 0;      // parts in the penetrator's own material ("" ); 0 = partColour
};

void drawProjectileShape(ImDrawList* draw, const phys::ProjectileShape& shape, const ShapeDrawFrame& frame);
ImU32 partColour(const phys::ShapePart& part);
unsigned partRgb(const phys::ShapePart& part);  // the same, 0xRRGGBB
