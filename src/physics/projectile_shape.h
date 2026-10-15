// The shape of a kinetic round or missile, as parts (body, cap, windshield, core, jacket, band,
// fins, tracer, filler, fuze, ...), each a solid of revolution given by stations along the axis.
#pragma once

#include "physics/penetration.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Generic fin-stabilised dart: long rods with L >= kDartMinLd D get light-alloy fittings around
// the unchanged body, in multiples of D: a hollow ballistic cap ahead of the nose, swept fins at
// the tail (trailing edges at the tail) and a tracer pellet behind it.
constexpr double kDartMinLd = 5;
constexpr double kDartCapLength = 2.5;
constexpr double kDartCapWall = 0.15;      // at least one particle spacing
constexpr double kDartFinSpan = 3.6;       // tip to tip
constexpr double kDartFinRootChord = 3.0;
constexpr double kDartFinTipChord = 1.2;
constexpr double kDartTracerLength = 0.6;
constexpr double kDartTracerWidth = 0.5;

namespace phys {

// A long rod of L >= kDartMinLd D: a fin-stabilised dart.
bool isDartRod(const Penetrator& p);

enum class ShapeStyle { Dart, Ap, Apc, Apbc, Apcbc, Apcr, Apds, Bullet, HeMissile, HeatRocket };
const char* shapeStyleId(ShapeStyle s);
std::optional<ShapeStyle> parseShapeStyle(std::string_view id);

enum class PartRole { Body, Core, Cap, Windshield, Jacket, Sleeve, Band, Fins, Tracer, Filler, Fuze, Case, Nose, Motor, Incendiary, Liner, Propellant };
const char* partRoleId(PartRole r);

// m; x from the round's nose tip, backwards positive. Two stations at the same x are a step.
struct ShapeStation {
    double x = 0, rOuter = 0, rInner = 0;
};

struct ShapePart {
    PartRole role = PartRole::Body;
    // mpmPartMaterial id; "" = the penetrator's own material; "explosive:<id>" = a filler.
    std::string material;
    std::vector<ShapeStation> stations;
    bool penetrator = false;  // the part the engineering models describe
    bool simulated = true;    // seeded in the MPM (false: drawn only)
    double density = 0;       // kg/m3; > 0 overrides the material's
};

// A catalogue shape: m, 0 / empty = the style's default.
struct ShapeSpec {
    ShapeStyle style = ShapeStyle::Ap;
    double length = 0, diameter = 0;
    std::optional<double> noseCrh;  // none: elliptical (legacy); 0: flat; > 0: tangent ogive
    double boatTail = 0, boatTailDiameter = 0;
    struct Band { double fromBase = 0, width = 0; } band;
    struct Cap { double length = 0; std::string material = "cap_steel"; } cap;
    struct Windshield { double length = 0, crh = 0, wall = 0; std::string material = "light_alloy"; } windshield;
    struct Core {
        double diameter = 0, length = 0;
        std::optional<double> noseCrh;
        double fromNose = -1;   // < 0: default
        std::string material;   // "" = the penetrator's own
    } core;
    std::string bodyMaterial;
    struct Jacket { double thickness = 0; std::string material = "gilding_metal"; } jacket;
    struct Incendiary { double length = 0; } incendiary;
    struct Filler {
        std::string explosive;  // empty: no filler
        double diameter = 0, length = 0, fromNose = -1, density = 0, detonationSpeed = 0, energy = 0;
    } filler;
    FuzeSpec fuze;
    struct Fins { double span = 0, rootChord = 0, tipChord = 0, sweep = -1; } fins;
    struct Tracer { double length = 0, diameter = 0; } tracer;
    struct DartCap { double length = 0, wall = 0; } dartCap;
    double caseWall = 0, warheadLength = 0, noseLength = 0;
    double motorDiameter = 0;  // heat_rocket: the motor tube behind the warhead
    std::string noseMaterial = "light_alloy";
    double massKg = 0;
    double refLength = 0, refDiameter = 0;  // the catalogue penetrator's sizes the spec was drawn for
    bool present = false;
};

struct ProjectileShape {
    std::vector<ShapePart> parts;  // nose to base by first station
    double length = 0, diameter = 0;
    // The engineering extent: the whole round for shells and missiles, the rod or core otherwise.
    double penetratorFront = 0, penetratorBack = 0, penetratorDiameter = 0;
    std::vector<std::string> warnings;  // e.g. a missile's mass that its parts and a full grain cannot make
};

ProjectileShape buildShape(const ShapeSpec& spec, const Penetrator& p);
// The catalogue's shape scaled to the case's length and diameter; nullptr = a generic shape.
ProjectileShape shapeForCase(const ShapeSpec* catalogue, const Penetrator& p);

// The part's section in the plane, both sides of the axis, m2.
double partArea(const ShapePart& part);
// Solid of revolution, m3 (fins: 4 fins, 0.1 D thick).
double partVolume(const ShapePart& part);
double partMass(const ShapePart& part, double materialDensity);
// Radii at x by linear interpolation; false outside the part.
bool partRadiiAt(const ShapePart& part, double x, double& rInner, double& rOuter);

} // namespace phys
