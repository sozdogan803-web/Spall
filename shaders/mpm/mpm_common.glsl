// Shared definitions of the deformation simulation (2D plane-strain MLS-MPM, see
// docs/physics.md and src/sim/mpm_solver.h). Positions are in grid cells from node (0, 0);
// everything else is SI per unit depth. The particle layout mirrors MpmParticle in
// src/sim/mpm_solver.cpp.
#ifndef MPM_COMMON_GLSL
#define MPM_COMMON_GLSL

const uint FLAG_FAILED = 1u;
const uint FLAG_INACTIVE = 2u;
const uint FLAG_PROJECTILE = 4u;
const uint FLAG_ANCHORED = 8u;
const uint FLAG_BURST_KICKED = 16u;  // the case has taken its burst tilt (g2p)
const uint FLAG_TETHERED = 32u;      // soft mount: damped spring to its rest position (p2g)
const uint FLAG_BAD = 1u << 16;  // went non-finite and was retired
const uint FLAG_FAIL_STRAIN = 1u << 17;
const uint FLAG_FAIL_SPALL = 1u << 18;
const uint FLAG_FAIL_MELT = 1u << 19;
const uint FLAG_REACTED = 1u << 20;  // reactive material lit (burning or burnt to products)
const uint FLAG_FITTING = 1u << 21;  // dart fitting (cap, fins, tracer): not in the rod readouts
const uint FLAG_TRACER = 1u << 22;
const uint FLAG_COMMINUTED = 1u << 23;   // ceramic fully fractured: granular, no tension
const uint FLAG_DELAMINATED = 1u << 24;  // laminate's matrix split: no shear or tension
const uint FLAG_FIBRE_BROKEN = 1u << 25; // laminate's fibres broken
const uint FLAG_JET = 1u << 26;          // shaped-charge jet material
const uint FLAG_JET_NECK = 1u << 27;     // the jet's first row of a segment: fails at breakup
const uint FLAG_JET_BROKEN = 1u << 28;   // the jet has broken up
const uint FLAG_MOUNT = 1u << 29;        // plate next to its held edge: never fails
const uint FLAG_FILLER = 1u << 30;       // the round's explosive filler: lit only by its fuze
const uint LAYER_SHIFT = 8u;         // bits 8..15 of the flags: plate layer

struct Particle {
    vec2 x;      // cells
    vec2 v;      // m/s
    vec4 C;      // affine velocity (c00, c01, c10, c11), 1/s
    vec4 F;      // elastic deformation gradient (f00, f01, f10, f11)
    vec4 tau;    // Kirchhoff stress (xx, yy, xy, zz), Pa, bulk viscosity included
    float Fzz;   // out-of-plane elastic stretch
    float epsP;  // equivalent plastic strain
    float temp;  // K
    float vol;   // initial volume, m2
    float mass;  // kg per m of depth
    uint mat;
    uint flags;
    float stress;    // von Mises, Pa
    vec2 xlo;        // compensation of the position sum, cells
    float pressure;  // Pa, compression positive
    float burn;      // reacted fraction of a reactive material
    vec2 X0;         // initial position, cells
    float tLight;    // s, when a reactive material lit
    float fracture;  // ceramic fracture variable D, 0 (intact) to 1 (comminuted)
    vec2 fibre;      // fibre vector of a laminate (seeded as the unit face tangent)
    float kick;      // jet: lateral velocity added at breakup, m/s
    float pad;
};

layout(std430, binding = 0) buffer Particles { Particle particles[]; };
// Fixed-point grid: per node (mass, momentum x, momentum y, anchored).
layout(std430, binding = 1) buffer Grid { int grid[]; };
layout(std430, binding = 2) buffer GridVelocity { vec2 gridVelocity[]; };
// Per material MAT_STRIDE vec4, packed by mpmPackMaterial (src/sim/mpm_scene.h):
//   0 (density, shear, bulk, yield), 1 (hardening, hardeningExp, softeningExp, meltK),
//   2 (heat, failureStrain, spall, sound speed), 3 (detonationSpeed, energy, productsGamma,
//   initiation; detonationSpeed 0 = inert), 4 (model, sigma_HEL, P_HEL, tensile T), then per
//   model: metal 5 (brittle crack strength, 1 / Weibull modulus, 1 / Gamma(1 + 1 / modulus), -;
//   0 = ductile), ceramic 5 (A, N, B, M), 6 (fractured cap in Pa, D1, D2, -); elastomer 5 (viscosity,
//   viscous limit, -, -); composite 5 (fibre modulus, fibre failure strain, delamination
//   tension, delamination shear).
layout(std430, binding = 3) readonly buffer Materials { vec4 materials[]; };
const uint MAT_STRIDE = 8u;
const uint MODEL_METAL = 0u;
const uint MODEL_CERAMIC = 1u;
const uint MODEL_ELASTOMER = 2u;
const uint MODEL_COMPOSITE = 3u;
vec4 matParam(uint mat, uint i) { return materials[mat * MAT_STRIDE + i]; }
uint matModel(uint mat) { return uint(matParam(mat, 4u).x + 0.5); }

uniform ivec2 uN;        // grid nodes
uniform float uDx;       // m
uniform float uDt;       // s
uniform float uMassRef;  // kg/m, fixed-point mass unit / 2^22
uniform float uVelRef;   // m/s, fixed-point velocity unit
uniform uint uCount;     // particles of this dispatch (a chunk of the store, or all of it)
uniform uint uBase;      // index of the dispatch's first particle in the whole store (a multiple of 256)
uniform vec2 uTether;    // soft mounts: angular frequency (rad/s), damping ratio

const float kFixedScale = 4194304.0;  // 2^22
const float kFixedClamp = 33554432.0; // 2^25 per contribution

int nodeIndex(ivec2 n) { return n.y * uN.x + n.x; }

// Quadratic B-spline weights of the three nodes from base for a particle at fractional
// offset fx = x - base (in [0.5, 1.5)).
void weights(vec2 fx, out vec2 w[3]) {
    w[0] = 0.5 * (1.5 - fx) * (1.5 - fx);
    w[1] = 0.75 - (fx - 1.0) * (fx - 1.0);
    w[2] = 0.5 * (fx - 0.5) * (fx - 0.5);
}

bool finite2(vec2 v) { return !any(isnan(v)) && !any(isinf(v)); }
bool finite4(vec4 v) { return !any(isnan(v)) && !any(isinf(v)); }

#endif
