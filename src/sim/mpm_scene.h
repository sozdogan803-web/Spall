// Initial state of the deformation simulation (MPM, phase 1) for one armour case, built on the
// CPU: plane-strain slab of the cross-section, x along the shot line with the first plate face at
// x = 0 at first contact (t = 0), y lateral, the same world frame as the Cross-section window.
// Plates are seeded on lattices aligned with their faces; the projectile on one aligned with x.
// The GPU solver (mpm_solver.h) takes it from here.
#pragma once

#include "data/projectile_db.h"
#include "physics/mpm_material.h"
#include "physics/projectile_shape.h"
#include "sim/armour_run.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Ultra, Insane and Ludicrous are for slow motion and pre-rendering: they run far below real time.
enum class MpmQuality { Low, Medium, High, Ultra, Insane, Ludicrous, Count };
const char* mpmQualityLabel(MpmQuality q);  // "Low", "Medium", "High", "Ultra", "Insane", "Ludicrous"
int mpmParticleBudget(MpmQuality q);         // 150 k, 600 k, 1.6 M, 4 M, 12 M, 100 M
int mpmCellsPerDiameter(MpmQuality q);       // 8, 14, 22, 128, 256, 512 (before coarsening to fit)
int mpmParticlesPerSide(MpmQuality q);       // 2, 2, 2, 3, 4, 5: particles to a cell side, so 4 to 25 a cell
int mpmMaxParticlesPerSide(MpmQuality q);    // 2, 2, 2, 6, 9, 16: the finest seed more where the grid binds
bool mpmBestPrerendered(MpmQuality q);       // Insane and Ludicrous: far too slow to watch as it computes

constexpr int kMpmMaxCells = 2048;            // grid nodes per axis (Low to High)
int mpmMaxCells(MpmQuality q);                // 1.5 times that for Ultra, twice for Insane, three times for Ludicrous

// Video memory a loaded scene takes (particles, grid, readout partials), bytes.
constexpr size_t kMpmParticleBytes = 144;     // MpmParticle in mpm_solver.cpp
size_t mpmVideoBytes(size_t particles, int nx, int ny);

// Why a scene of this size cannot be loaded, or empty if it can. `availBytes` is the video memory
// free (plus what the scene being replaced gives back; 0: unknown), of which at most 85 % may be
// taken; `maxBlockBytes` is the most the card allows in one storage buffer (0: unknown): the
// particles are one buffer (kMpmParticleBytes each) and the grid another (16 bytes a node).
std::string mpmFitProblem(size_t particles, int nx, int ny, size_t availBytes, size_t maxBlockBytes);
constexpr double kMpmCfl = 0.25;              // dt = kMpmCfl dx / (c_max + v)
constexpr int kMpmPrerollCells = 3;           // the nose starts this many cells before the first face
constexpr int kMpmMountCells = 6;             // at least this many cells of each plate's mount band

// Soft mounts: each plate's edge strip, clamped rigidly by default, is instead tethered to where it
// started by a damped spring (angular frequency mountOmega, damping ratio kMpmMountDamping), so
// the mounting gives and rings like a real one. The mount is as compliant as a bar of the plate's
// own steel (wave speed kMpmMountWave) kMpmMountLength long, whatever the cell: the strip is 2 dx
// wide, so omega = c / sqrt(2 dx L).
constexpr double kMpmMountWave = 5000;        // m/s
constexpr double kMpmMountLength = 1.0;       // m
constexpr double kMpmMountDamping = 0.5;      // damping ratio of the tether

// Acceleration of a tethered particle displaced d (m) from its rest position and moving at v (m/s)
// along that axis: the mirror of the tether force in shaders/mpm/p2g.comp.
inline double mpmTetherAcceleration(double omega, double zeta, double d, double v) {
    return -omega * omega * d - 2 * zeta * omega * v;
}

enum MpmParticleFlag : uint32_t {
    kMpmFailed = 1u,
    kMpmInactive = 2u,      // left the domain
    kMpmProjectile = 4u,
    kMpmAnchored = 8u,      // plate edge held by its (unmodelled) mounting
    kMpmBurstKicked = 16u,  // the case has taken its burst tilt (solver)
    kMpmTethered = 32u,     // soft mount: held to its rest position by a damped spring, not clamped
    kMpmLayerShift = 8,     // bits 8..15: plate layer index (projectile: 0)
    kMpmBad = 1u << 16,         // went non-finite and was retired (solver)
    kMpmFailStrain = 1u << 17,  // failure cause (solver): plastic strain, spall, melting
    kMpmFailSpall = 1u << 18,
    kMpmFailMelt = 1u << 19,
    kMpmReacted = 1u << 20,     // reactive material lit: burning or burnt to products (solver)
    kMpmFitting = 1u << 21,     // a part other than the penetrator (cap, fins, jacket, ...): not in the rod readouts
    kMpmTracer = 1u << 22,      // a tracer (a dull pellet)
    kMpmComminuted = 1u << 23,  // ceramic fully fractured: granular, no tension (solver)
    kMpmDelaminated = 1u << 24, // laminate's matrix split: no shear or tension (solver)
    kMpmFibreBroken = 1u << 25, // laminate's fibres broken (solver)
    kMpmJet = 1u << 26,         // shaped-charge jet material
    kMpmJetNeck = 1u << 27,     // the jet's first row of a segment: fails at breakup
    kMpmJetBroken = 1u << 28,   // the jet has broken up (solver, or seeded after breakup)
    kMpmMount = 1u << 29,       // plate next to its held edge: yields and stretches but never fails
    kMpmFiller = 1u << 30,      // the round's explosive filler: lit only by its fuze (solver)
    kMpmDormant = 1u << 31,     // not yet (or no longer) in play: a HEAT round's jet before release, its liner after
};

// Generic fin-stabilised dart (kDart* in physics/projectile_shape.h): phys::isDartRod.
bool mpmIsDart(const phys::Penetrator& p);

struct MpmSeed {
    float x, y;      // world position at tStart, m
    float volume;    // m2 (per unit depth)
    uint16_t material;  // index into MpmScene::materials
    uint16_t layer;     // plate layer (projectile: 0)
    uint32_t flags;     // MpmParticleFlag
    float vx = 0, vy = 0;  // initial velocity, m/s
    float kick = 0;        // jet: lateral velocity added at breakup, m/s
    float temp = 0;        // initial temperature, K (0: room temperature)
};

struct MpmScene {
    bool supported = false;
    std::string message;              // why not supported, or a note for the UI
    MpmQuality quality = MpmQuality::Medium;
    double dx = 0;                    // grid spacing, m
    double spacing = 0;               // nominal particle spacing dx / particlesPerSide, m
    int particlesPerSide = 2;         // particles to a cell side as seeded (a cell holds its square)
    double originX = 0, originY = 0;  // world position of grid node (0, 0), m
    int nx = 0, ny = 0;               // grid nodes
    double dt = 0;                    // s
    double tStart = 0, tEnd = 0;      // s, on the armour timeline (0 = first contact)
    double velocity = 0;              // impact velocity along +x, m/s
    double plateHalfExtent = 0;       // m, along each plate face from the shot line
    // m, of each layer as simulated: its thickness, but a kinetic round that stops in the stack
    // sees it cut a little past its reach (0 for layers beyond the cut), the cut face held.
    std::vector<double> simThickness;
    double cutDepth = 0;              // m along the shot line, where such a cut is made (0: none)
    double maxSoundSpeed = 0;         // m/s, fastest signal of any material (mpmSignalSpeed)
    // 0: the penetrator; 1 + i: plate layer i; then the other parts' materials (partMaterials).
    std::vector<phys::MpmMaterial> materials;
    std::vector<std::string> partMaterials;    // part material id of each material after the plates
    std::vector<MpmSeed> particles;            // projectile first: the penetrator part, then the other parts
    int projectileCount = 0;          // all the projectile's parts
    int bodyCount = 0;                // the penetrator part itself
    bool dart = false;                // a fin-stabilised dart (mpmIsDart)
    double bodyNose = 0;              // m, x of the penetrator's front at tStart (a cap may lead it)
    double bodyLength = 0;            // m
    int reactiveCount = 0;            // reactive plate particles free to react (anchored ones are held)
    int ceramicCount = 0;             // ceramic plate particles free to move (not anchored)
    int compositeCount = 0;           // laminate plate particles free to move (not anchored)
    double faceTangent[2] = {0, 1};   // unit vector along the plate faces; seeds the fibre direction
    double projectileArea = 0;        // m2 of the body's section
    double diameter = 0;              // m, projectile width in the plane (jets: reference width)
    bool jet = false;                 // shaped-charge jet (times since formation)
    double jetBreakup = 1e30;         // s, the jet's breakup time (never for other projectiles)
    double jetOrigin = 0;             // m, virtual origin S behind the first face
    // A HEAT round flown in whole (a catalogue or default heat_rocket body): the jet waits dormant
    // until jetRelease (the model's start), when the liner goes dormant in its place.
    bool heatBody = false;
    double jetRelease = 1e30;         // s
    bool fuzeProgrammed = false;      // the filler lights at fuzeLightTime from fuzeOrigin, no trigger
    double fuzeLightTime = 0;         // s
    // Fuzed rounds with a filler (else fuze.type None). The GPU sums each projectile particle's
    // round(m / fuzeMassUnit) x round(10 x speed lost) and triggers when the mean loss reaches
    // fuze.triggerDv; the filler then lights from fuzeOrigin (cells, X0) at its detonation speed
    // once the delay has run.
    phys::FuzeSpec fuze;
    double fuzeOrigin[2] = {0, 0};    // cells: base delay, the filler's rear on the axis; nose contact, its front
    double fuzeMassUnit = 0;          // kg per m of depth: the heaviest projectile particle / 4096
    double fuzeTotalUnits = 0;        // sum of round(m / fuzeMassUnit) over the projectile particles
    int fillerCount = 0;              // filler particles
    // The burst's forward lean. The products' pressure already pushes the case outwards; what a
    // sweeping detonation adds is Taylor's tilt of the metal's flight towards the way it travels.
    // The case alongside the charge (X0 in burstSpan, cells) is driven at Gurney's cylinder speed
    // burstGurney (m/s), tilted burstTaylor (rad) towards +x for a base-lit round (burstSweep +1) or
    // -x for a nose-lit one (-1); burstSpeed is the detonation speed D. All zero without a fuzed filler.
    double burstGurney = 0;
    double burstTaylor = 0;
    int burstSweep = 0;
    double burstSpeed = 0;
    double burstSpan[2] = {0, 0};
    // Soft mounts (see kMpmMountWave): the lateral edge strips are tethered, not clamped.
    bool softMounts = false;
    double mountOmega = 0;            // rad/s
    double mountDamping = 0;          // damping ratio
};

// The GPU materials table: kMpmMaterialFloats floats per material (see Materials in
// shaders/mpm/mpm_common.glsl for the layout).
constexpr int kMpmMaterialFloats = 32;
void mpmPackMaterial(const phys::MpmMaterial& m, float* out);

// Jets: the stream from phys::jetParameters at the model's start time, cut into segments of
// kMpmJetSegment jet radii (at least 3 cells) at breakup, the jet's reference width (for the
// grid's cells per diameter) kMpmJetReference cone diameters, its temperature kMpmJetTemperature.
constexpr double kMpmJetSegment = 4;
constexpr double kMpmJetReference = 0.1;
constexpr double kMpmJetTemperature = 700;

// Builds the scene, a kinetic round seeded part by part from `shape` (phys::shapeForCase; no
// parts: a generic shape; ignored for jets). Empty stacks come back unsupported with a message.
// `softMounts` tethers each plate's lateral edge strips instead of clamping them.
MpmScene buildMpmScene(const ProjectileDb& db, const ArmourCase& c, RodMaterial rod, const phys::ProjectileShape& shape,
                       const ArmourOutcome& out, MpmQuality quality, bool softMounts = false);
