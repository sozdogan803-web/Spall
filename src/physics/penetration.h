// Terminal ballistics of one projectile striking a stack of generic, parallel armour plates
// (CPU, SI units). Long rods: Tate-Alekseevskii integrated in time, calibrated to Lanz-Odermatt.
// Compact AP: de Marre limit velocity with Lambert-Jonas residuals. Shaped charges: discrete
// jet elements from a virtual origin. Engineering estimates for education only: the outputs
// are penetration physics of plates, not vulnerability of any vehicle or structure.
#pragma once

#include <string>
#include <vector>

namespace phys {

enum class PenetratorKind { LongRod, CompactAp, ShapedCharge, HeWarhead, Count };
const char* penetratorKindId(PenetratorKind k);     // "long_rod", "compact_ap", "shaped_charge"
const char* penetratorKindLabel(PenetratorKind k);  // "Long rod (APFSDS)", ...

constexpr double kRhaDensity = 7850.0;       // kg/m3
constexpr double kRodStrength = 1.5e9;       // Pa, tungsten-alloy and DU long-rod flow stress Yp
constexpr double kDeMarreK = 2300.0;         // m/s, de Marre constant for RHA (t, D in dm; m in kg)
constexpr double kJetMinVelocity = 2000.0;   // m/s, slowest jet element that is tracked
constexpr double kMaxObliquityDeg = 80.0;

struct ArmourMaterial {
    double density = kRhaDensity;  // kg/m3
    double resistance = 5.6e9;     // Tate target resistance Rt, Pa
    double hardnessBhn = 300;      // Brinell hardness; 0 for non-metals (validity checks only)
    double keEfficiency = 1;       // compact-AP mass efficiency relative to RHA (areal density)
    bool ceramic = false;          // assumed laterally confined
    bool reactive = false;         // explosive layer, treated here as an inert soft plate
};

struct ArmourLayer {
    ArmourMaterial material;
    double thickness = 0.05;  // normal thickness, m
    double gap = 0;           // air gap in front of the plate along its normal, m (ignored for the first)
};

// The fuze of a round with an explosive filler. It triggers once the round has lost triggerDv of
// its impact velocity; a base-delay fuze (APHE) then lights the filler at its rear after delay, a
// nose-contact fuze (missiles) at its front after functionTime. s, m/s, m.
enum class FuzeType { None, BaseDelay, NoseContact };
struct FuzeSpec {
    FuzeType type = FuzeType::None;
    double triggerDv = 0;       // m/s
    double delay = 0;           // s (base delay)
    double functionTime = 0;    // s (nose contact)
    double fillerLength = 0;    // m, for the run's end time
    double detonationSpeed = 0; // m/s, of the filler
};

struct Penetrator {
    PenetratorKind kind = PenetratorKind::LongRod;
    // Kinetic rounds (rod, core or shot).
    double diameter = 0.025;       // m
    double length = 0.6;           // m
    double density = 17600;        // kg/m3
    double mass = 0;               // kg; 0 = solid cylinder of diameter, length and density
    double strength = kRodStrength;// long-rod flow stress Yp, Pa
    bool uranium = false;          // DU (Lanz-Odermatt constants for validity and reference)
    // Shaped charges.
    double coneDiameter = 0.1;     // m
    double standoff = 2;           // built-in standoff, cone diameters
    double jetQuality = 0.5;       // q: tip velocity 7000 + 2000 q m/s; breakup and drift follow
    double linerDensity = 8930;    // kg/m3 (copper)
    // Rounds with an explosive filler (APHE, missiles); type None for everything else.
    FuzeSpec fuze;
};

// Mass of the kinetic penetrator, kg.
double penetratorMass(const Penetrator& p);

struct JetParameters {
    double tipVelocity;   // m/s
    double minVelocity;   // m/s
    double breakupTime;   // s after formation at the virtual origin
    double drift;         // lateral particle velocity / axial velocity after breakup
    double virtualOrigin; // m behind the first plate face at the built-in standoff
    double craterRadius;  // m, radius a drifting particle must stay within to hit the crater bottom
    double jetRadius;     // m, representative jet radius (crater width only)
};
JetParameters jetParameters(const Penetrator& p);
// Deterministic standard-normal-ish number for jet particle k (sum of four uniforms): the
// lateral drift direction of particle k after breakup, in units of drift * velocity.
double jetParticleOffset(int k);

struct PenetrationSample {
    double time;          // s (kinetic: since first contact; jets: since formation)
    double depth;         // nose or crater-bottom position along the shot line from the first face, m
    double velocity;      // m/s: rod or shot velocity; jets: velocity of the element at the bottom
    double length;        // m: remaining rod or shot length; jets: remaining effective jet length
    double rate;          // penetration rate along the shot line, m/s
    double craterRadius;  // m (illustrative)
    int layer;            // layer at the nose; -1 in a gap, layers.size() once through
};

struct LayerResult {
    double front = 0, back = 0;  // entry and exit along the shot line, m
    double depth = 0;            // depth reached inside this layer along the shot line, m
    bool perforated = false;
    double exitVelocity = 0;     // m/s (rod or shot; jets: first element through)
    double exitTime = 0;         // s
};

enum class JetElementFate { Consumed, Stopped, Residual };

struct JetElement {
    double velocity;   // m/s
    double length;     // effective length arriving at the crater, m
    double fullLength; // length before drift losses, m
    double arrival;    // s, reaches the crater bottom (or the rear face for residual elements)
    double finish;     // s, fully consumed
    JetElementFate fate;
};

struct PenetrationResult {
    std::vector<PenetrationSample> history;
    std::vector<LayerResult> layers;
    std::vector<JetElement> jet;   // shaped charges only, tip first
    bool perforated = false;
    double depth = 0;              // deepest point along the shot line from the first face, m
    double residualVelocity = 0;   // m/s behind the stack (jets: tip of the residual jet)
    double residualLength = 0;     // m (rods: remaining length; jets: residual effective length)
    double residualMass = 0;       // kg (kinetic rounds)
    double exitTime = 0;           // s, nose leaves the last plate
    double startTime = 0;          // s, suggested animation start
    double endTime = 0;            // s, suggested animation end
    std::vector<std::string> warnings;
};

// Simulates the full event. obliquityDeg is measured from the plate normal and applies to every
// plate; velocity is ignored for shaped charges.
PenetrationResult simulatePenetration(const Penetrator& p, double velocity, double obliquityDeg,
                                      const std::vector<ArmourLayer>& layers);

// Shaped-charge jet with explicit parameters (used by simulatePenetration and the tests).
PenetrationResult simulateJet(const JetParameters& jet, double linerDensity, double coneDiameter,
                              double obliquityDeg, const std::vector<ArmourLayer>& layers);

// Semi-infinite RHA penetration at normal incidence, m.
double rhaPenetration(const Penetrator& p, double velocity);

// Depth of penetration into a semi-infinite RHA witness block directly behind the stack, m.
double residualPenetration(const Penetrator& p, double velocity, double obliquityDeg,
                           const std::vector<ArmourLayer>& layers);

// Lowest impact velocity (m/s) that perforates the stack; kinetic rounds only. Returns 0 when
// not applicable and a negative value when even maxVelocity does not perforate.
double ballisticLimit(const Penetrator& p, double obliquityDeg, const std::vector<ArmourLayer>& layers,
                      double maxVelocity = 3000);

// Jet quality q that makes rhaPenetration match the given published depth at the built-in
// standoff (clamped to [-0.8, 1.5]).
double fitJetQuality(Penetrator p, double publishedRhaDepth);

// Published relations used by the models, the calibration and the tests.
double lanzOdermattRatio(bool uranium, double lengthOverDiameter, double velocity, double rodDensity,
                         double targetDensity, double bhn, double obliquityDeg);  // P / L
double steelResistanceFromBhn(double bhn);  // Tate Rt for armour steel, Pa
double tatePenetrationRate(double v, double rodDensity, double rodStrength, double targetDensity,
                           double targetResistance);  // u, m/s (0 when the rod cannot penetrate)
double lengthFactor(double lengthOverDiameter);        // Lanz-Odermatt 1/tanh(b0 + b1 L/D)
double deMarreLimitVelocity(double rhaThickness, double diameter, double mass);  // m/s, normal
double deMarreThickness(double velocity, double diameter, double mass);        // m: its inverse
double lambertJonasResidual(double v, double limitVelocity, double exponent, double plugFactor);

}  // namespace phys
