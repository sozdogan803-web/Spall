// Material models of the deformation simulation (MPM). Metal: Hencky elasticity with J2
// plasticity, Johnson-Cook flow stress without the rate term, adiabatic heating and a simple
// failure rule; reactive (explosive) materials burn to gamma-law products. Ceramic: the
// Johnson-Holmquist (JH-2) pressure-dependent strength with fracture and comminution.
// Elastomer: compressible neo-Hookean with a capped deviatoric viscosity. Composite: the metal
// model as matrix plus tension-only fibres along the plies, with delamination. SI units. The
// CPU functions here mirror shaders/mpm/g2p.comp and are what the unit tests check. Generic,
// approximate open-literature parameters.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace phys {

constexpr double kRoomTemperature = 293.0;  // K
constexpr double kTaylorQuinney = 0.9;      // fraction of plastic work that becomes heat
constexpr double kMinCeramicFracture = 1e-4;  // floor of the ceramic's fracture strain

enum class MpmModel { Metal, Ceramic, Elastomer, Composite, Count };
const char* mpmModelId(MpmModel m);  // "metal", "ceramic", "elastomer", "composite"

struct MpmMaterial {
    MpmModel model = MpmModel::Metal;
    double density = 7850;        // kg/m3
    double shear = 77.5e9;        // shear modulus G, Pa
    double bulk = 159e9;          // bulk modulus K, Pa
    double yield = 1.0e9;         // Johnson-Cook A, Pa
    double hardening = 0.5e9;     // Johnson-Cook B, Pa
    double hardeningExp = 0.26;   // n
    double softeningExp = 1.03;   // m (thermal softening)
    double meltK = 1793;          // melting temperature, K
    double heat = 477;            // specific heat c_p, J/(kg K)
    double failureStrain = 1.5;   // equivalent plastic strain at failure
    double spall = 3e9;           // tensile mean stress at failure, Pa
    // Brittle fracture (metals): the largest principal tensile stress at which a sound point
    // cracks, Pa; 0 = ductile (fails only by strain, spall or melting). Each point's strength is
    // this times its own flaw factor (mpmFlawFactor, Weibull modulus flawModulus: lower is a
    // wider spread), so cracks start at the weak points and run along the stress they raise.
    double brittleStress = 0;
    double flawModulus = 10;
    // Reactive (explosive) materials only; detonationSpeed 0 = inert.
    double detonationSpeed = 0;   // D, m/s
    double energy = 4e6;          // Q, J/kg, released into the products (acts as the Gurney energy)
    double productsGamma = 3;     // gamma of the products
    double initiation = 2e9;      // pressure that lights the explosive, Pa
    // Ceramic (JH-2 form; alumina-like defaults).
    double hel = 19e9;            // Hugoniot elastic limit, Pa
    double intactA = 0.93;        // intact strength coefficient A
    double intactN = 0.6;         // intact strength exponent N
    double fracturedB = 0.31;     // fractured strength coefficient B
    double fracturedM = 0.6;      // fractured strength exponent M
    double tensile = 0.2e9;       // tensile strength T (as a negative pressure), Pa
    double fracturedMax = 0.2;    // cap of the fractured strength, fraction of sigma_HEL
    double fractureD1 = 0.005;    // fracture strain D1 ((p + T) / P_HEL)^D2
    double fractureD2 = 1.0;
    // Elastomer.
    double viscosity = 0;         // deviatoric viscosity eta, Pa s
    double viscousLimit = 0;      // cap of the viscous stress (von Mises), Pa; 0 = no cap
    // Composite (fibres along the plies).
    double fibreModulus = 0;      // E_f of the laminate along its fibres, Pa
    double fibreFailureStrain = 0.03;  // log strain at which the fibres break
    double delamTension = 0.01e9; // through-thickness tension strength, Pa
    double delamShear = 0.01e9;   // interlaminar shear strength, Pa
};

inline bool mpmReactive(const MpmMaterial& m) { return m.detonationSpeed > 0; }

// Strength of one point of a brittle metal relative to the material's, a Weibull draw of
// modulus `modulus` normalised to mean one, never below kMinFlawFactor. `a` and `b` identify the
// point (the GPU passes the bits of its initial position), the same hash as g2p.comp.
constexpr double kMinFlawFactor = 0.25;
double mpmFlawFactor(uint32_t a, uint32_t b, double modulus);

// Built-in penetrator materials.
MpmMaterial mpmTungstenAlloy();
MpmMaterial mpmDepletedUranium();
MpmMaterial mpmPenetratorSteel();
// Light alloy (aluminium-type) of a dart's fittings: ballistic cap, fins, tracer housing.
MpmMaterial mpmLightAlloy();
MpmMaterial mpmLinerCopper();  // a shaped-charge jet

// Materials of a projectile's parts by id: gilding_metal, lead, cap_steel, tungsten_carbide,
// mild_steel, case_steel, incendiary, and the built-ins core_steel, tungsten_alloy, du and
// light_alloy. nullopt for an unknown id.
std::optional<MpmMaterial> mpmPartMaterial(std::string_view id);
// A shell's or warhead's explosive filler: sheet-explosive mechanics with the given density
// (kg/m3), detonation speed (m/s) and energy (J/kg). Its initiation pressure is out of reach,
// so impact never lights it: only the fuze does (see the MPM fuze).
MpmMaterial mpmFiller(double density, double detonationSpeed, double energy);

// Longitudinal elastic wave speed sqrt((K + 4G/3) / rho), m/s.
double mpmSoundSpeed(const MpmMaterial& m);
// Fastest signal for the time step: the elastic speed, with a composite's fibre modulus added
// to the stiffness; for reactive materials also the detonation speed and 1.5 x the products'
// sound speed at their initial state, m/s.
double mpmSignalSpeed(const MpmMaterial& m);
// Largest stable step of an elastomer's viscosity at grid spacing dx, 0.25 rho dx^2 / eta, s
// (infinite without viscosity).
double mpmViscousStep(const MpmMaterial& m, double dx);

// The state at a ceramic's Hugoniot elastic limit under uniaxial strain: strain
// HEL / (K + 4G/3), pressure P_HEL = K strain, von Mises sigma_HEL = 2G strain, Pa.
struct MpmHelState {
    double strain = 0, pressure = 0, strength = 0;
};
MpmHelState mpmHelState(const MpmMaterial& m);

// Products of a reactive material with reacted fraction `burn` at volume ratio J = V / V0:
// p = burn (gamma - 1) rho0 Q J^-gamma (a constant-volume release, then isentropic expansion,
// which does work rho0 Q per unit volume from J = 1 to infinity), Pa.
double mpmProductsPressure(const MpmMaterial& m, double burn, double J);
// Products temperature 293 K + burn (Q / c_p) J^(-1/4), K (display only). The blast view's
// fireball law (T ~ V^(-1/4)); the gamma-law isentrope J^(1 - gamma) would take them below red
// heat by J = 2, which real products, whose gamma falls towards 1.3 as they expand, do not.
double mpmProductsTemperature(const MpmMaterial& m, double burn, double J);

// Flow stress (A + B ep^n)(1 - T*^m), Pa; zero at and above the melting point.
double mpmYieldStress(const MpmMaterial& m, double plasticStrain, double temperature);

// A ceramic's JH-2 strength (von Mises, Pa) at pressure p with fracture D: intact
// sigma_HEL A ((p + T) / P_HEL)^N (0 for p + T <= 0), fractured
// min(sigma_HEL B (p / P_HEL)^M, fracturedMax sigma_HEL) (0 for p <= 0), blended by D.
double mpmCeramicStrength(const MpmMaterial& m, double pressure, double fracture);
// Plastic strain that fractures a ceramic at pressure p:
// max(D1 ((p + T) / P_HEL)^D2, kMinCeramicFracture).
double mpmCeramicFractureStrain(const MpmMaterial& m, double pressure);

// Principal Kirchhoff stresses of an elastomer (compressible neo-Hookean) from its principal
// log stretches e_i = ln lambda_i: G J^(-2/3) (lambda_i^2 - sum lambda^2 / 3) + K ln J, Pa
// (Hencky at small strain, zero at rest).
void mpmElastomerKirchhoff(const MpmMaterial& m, const double strain[3], double stress[3]);
// An elastomer's viscous Kirchhoff stress 2 eta J D_dev from the in-plane velocity gradient
// L[i][j] = dv_i / dx_j (plane strain: D_zz = 0), its von Mises value capped at the viscous
// limit. Writes tau = (xx, yy, xy, zz), Pa, and returns the power it dissipates per unit
// initial volume, tau : D (>= 0), W/m3, which heats the elastomer.
double mpmViscousStress(const MpmMaterial& m, const double L[2][2], double J, double tau[4]);

// A laminate's fibre stress from its fibre vector f (seeded as the unit face tangent and
// carried with the material, so |f| is the fibre stretch): Kirchhoff E_f ln|f| along f in
// tension, none in compression or once the fibres have broken. Writes tau = (xx, yy, xy, zz), Pa.
void mpmFibreStress(const MpmMaterial& m, const double f[2], bool broken, double tau[4]);
// The fibres break (for good) once ln|f| exceeds the fibre failure strain.
bool mpmFibresBreak(const MpmMaterial& m, const double f[2]);
// The matrix delaminates when its Cauchy stress (Kirchhoff tau / J, tau = (xx, yy, xy, zz))
// across the plies (normal to f) exceeds the through-thickness tension strength, or its shear
// on the plies exceeds the interlaminar shear strength.
bool mpmDelaminates(const MpmMaterial& m, const double tau[4], double J, const double f[2]);

// One particle's constitutive update in principal log-strain space.
struct MpmPoint {
    double strain[3];     // principal elastic log strains (in-plane 1, 2; out-of-plane 3)
    double plasticStrain = 0;
    double temperature = kRoomTemperature;
    bool failed = false;
    double flaw = 1;          // brittle metal: this point's strength relative to the material's (mpmFlawFactor)
    double fracture = 0;      // ceramic: fracture D, 0 to 1
    bool comminuted = false;  // ceramic: D reached 1 (granular: fractured strength, no tension)
    bool delaminated = false; // composite: the plies have split (they slide and open)
    bool fibreBroken = false; // composite: the fibres have broken (set by the caller)
};

// Radial return of the trial strains onto the yield surface, heating, then the failure rule;
// a failed point keeps only compressive volumetric strain. A ceramic returns onto its JH-2
// strength at the trial pressure; its plastic strain grows the fracture D, tension past
// T (1 - D) sets D = 1, and at D = 1 it is comminuted (no tension); melting fails it. An
// elastomer has no plasticity: it fails when its largest principal engineering stretch
// exceeds the failure strain, in tension past the spall stress, or when it reaches its melting
// (decomposition) temperature. A composite's matrix takes the metal return map, but reaching
// its failure strain or spall stress delaminates it instead (the solver also delaminates it on
// mpmDelaminates of the returned stress, in the same step). Delaminated it is a stack of
// sheets: its strains are then along the fibres, across the plies and out of plane (the solver
// projects them into that frame and drops the ply shear), each sheet still yields, and any
// tension across the plies is relaxed away; melted, or delaminated with broken fibres, it has
// failed.
// Returns the plastic strain increment.
double mpmReturnMap(const MpmMaterial& m, MpmPoint& p);

// Principal Kirchhoff stresses of the Hencky model, Pa (tension positive).
void mpmKirchhoff(const MpmMaterial& m, const double strain[3], double stress[3]);

// von Mises equivalent of principal stresses, Pa.
double mpmVonMises(const double stress[3]);

}  // namespace phys
