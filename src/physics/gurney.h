// Gurney and Taylor: how a detonating charge drives its case. Pure functions of the explosive's
// Gurney speed sqrt(2E), the metal and charge masses per unit length, and the detonation speed.
#pragma once

#include <algorithm>
#include <cmath>

namespace phys {

// Gurney's open-ended cylinder: the case, of mass `caseMass`, leaves at
// V = sqrt(2E) / sqrt(M/C + 1/2) for a charge of mass `chargeMass` (any matching mass units).
// No charge, no push.
inline double gurneyCylinderVelocity(double sqrt2E, double caseMass, double chargeMass) {
    if (!(chargeMass > 0) || !(sqrt2E > 0)) return 0;
    return sqrt2E / std::sqrt(std::max(caseMass, 0.0) / chargeMass + 0.5);
}

// Taylor: a detonation sweeping along the case turns the metal's flight towards the direction it
// travels, sin(theta) = V / (2 D), theta measured from the surface normal. Capped at a right angle.
inline double taylorAngle(double velocity, double detonationSpeed) {
    if (!(detonationSpeed > 0) || !(velocity > 0)) return 0;
    return std::asin(std::min(velocity / (2 * detonationSpeed), 1.0));
}

}  // namespace phys
