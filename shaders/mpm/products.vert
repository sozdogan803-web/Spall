#version 460
// Explosive products (reacted particles) as soft, noisy puffs into the products buffer
// (src/render/mpm_view.cpp, additive). A puff covers twice the particle's expanded width (the
// products' particles spread unevenly): it grows as sqrt(J), at most 8 x, but is never smaller
// than uMinPuff pixels so a zoomed-out cloud stays continuous. Its deposit is scaled to the size, so the buffer sums to the relative density
// rho / rho0 whatever the zoom. Drawn per chunk of the store, uBase the chunk's first particle.
#include "mpm_common.glsl"
#include "frame.glsl"

uniform vec2 uOrigin;      // m, world position of grid node (0, 0)
uniform vec2 uCentre;      // m, view centre
uniform vec2 uHalfView;    // m, half the canvas
uniform float uSpacingPx;  // particle spacing on screen
uniform float uMinPuff;    // px
uniform float uMaxPuff;    // px, the largest point size the driver draws
uniform int uFrames;       // draw the pre-rendered frames at 7 and 8 instead of the particles
uniform float uLerp;       // weight of the frame at 8 in the positions
// Pre-rendered playback (src/render/mpm_snapshots.h): the frames either side of the time.
layout(std430, binding = 7) readonly buffer FrameA { Frame frameA[]; };
layout(std430, binding = 8) readonly buffer FrameB { Frame frameB[]; };

out float vDeposit;   // relative density at the puff's centre per unit footprint
out float vTemp;      // kK
out float vExpansion; // 0 dense .. 1 expanded twentyfold
flat out vec2 vSeed;

void main() {
    Particle p = uFrames != 0 ? unpackFrame(frameA[gl_VertexID]) : particles[gl_VertexID];
    if ((p.flags & FLAG_BAD) != 0u || (p.flags & FLAG_REACTED) == 0u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = 1.0;
        vDeposit = vTemp = vExpansion = 0.0;
        vSeed = vec2(0.0);
        return;
    }
    vec2 x = uFrames != 0 && uLerp > 0.0 ? mix(p.x, frameB[gl_VertexID].x, uLerp) : p.x;
    vec2 w = uOrigin + x * uDx;
    gl_Position = vec4((w - uCentre) / uHalfView, 0.0, 1.0);
    float J = max((p.F.x * p.F.w - p.F.y * p.F.z) * p.Fzz, 1.0);
    float size = clamp(2.5 * uSpacingPx * min(sqrt(J), 8.0), max(uMinPuff, 1.0), uMaxPuff);
    gl_PointSize = size;
    // The footprint (1 - r^2)^2 integrates to pi S^2 / 12; one particle's share of the dense
    // material is spacing^2. Burning explosive counts with its products.
    vDeposit = 12.0 * uSpacingPx * uSpacingPx / (3.14159265 * size * size);
    vTemp = p.temp * 1e-3;
    vExpansion = clamp(log(J) / 3.0, 0.0, 1.0);
    // Each puff its own patch of noise, fixed to the particle so the texture moves with it (the
    // index in the whole store, to the 24 bits a float keeps exactly).
    float id = float((uBase + uint(gl_VertexID)) & 0xFFFFFFu);
    vSeed = fract(vec2(id * 0.7548777, id * 0.5698403)) * 97.0;
}
