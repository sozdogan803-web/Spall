#version 460
// Point sprite per deformation-simulation particle (src/render/mpm_view.cpp), placed with the
// Cross-section window's centre and scale and coloured by the selected field. The view draws the
// points of a chunk of the store (or of a frame) in ranges, the plates' before the projectile's,
// which sets the order they overlap in. Passes: explosive
// products (round, blended, growing as they expand; in Material colours the products pass of
// shaders/mpm/products.* draws them instead), the solid material (plates first, the projectile
// last; debris, comminuted ceramic and delaminated laminate smaller, ceramic darkening as it
// fractures), and in Material colours an additive glow of hot metal. Retired particles are
// culled.
#include "mpm_common.glsl"
#include "frame.glsl"
#include "../common/colormap.glsl"

uniform vec2 uOrigin;       // m, world position of grid node (0, 0)
uniform vec2 uCentre;       // m, view centre
uniform vec2 uHalfView;     // m, half the canvas
uniform float uPointSize;   // px
uniform float uShift;       // m, projectile offset before the simulation starts
uniform int uMode;          // MpmColour
uniform vec2 uRange;        // field value at the ends of the colour bar
uniform vec3 uColours[32];  // display sRGB per material (0 = projectile)
uniform int uPass;          // 0 = products (reacted explosive), 1 = everything else, 2 = glow
uniform sampler2D uGlow;    // display glow over uGlowRange (src/render/blackbody.h)
uniform vec2 uGlowRange;    // K
uniform float uMaxPoint;    // px, the largest point size the driver draws
uniform vec4 uBody;         // cells: the projectile body's initial tail x, nose x, axis y, radius
uniform int uDart;          // the body has sabot grooves
uniform int uFrames;        // draw the pre-rendered frames at 7 and 8 instead of the particles
uniform float uLerp;        // weight of the frame at 8 in the positions
// Pre-rendered playback (src/render/mpm_snapshots.h): the frames either side of the time.
layout(std430, binding = 7) readonly buffer FrameA { Frame frameA[]; };
layout(std430, binding = 8) readonly buffer FrameB { Frame frameB[]; };

out vec3 vColour;
out float vAlpha;

vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// Glow colour times brightness (rgb) and brightness (a) of material at T.
vec4 glowAt(float T) {
    float n = float(textureSize(uGlow, 0).x);
    float u = clamp((T - uGlowRange.x) / (uGlowRange.y - uGlowRange.x), 0.0, 1.0);
    return textureLod(uGlow, vec2((u * (n - 1.0) + 0.5) / n, 0.5), 0.0);
}

void main() {
    uint id = uint(gl_VertexID);  // in the bound chunk, or the bound frame
    Particle p = uFrames != 0 ? unpackFrame(frameA[id]) : particles[id];
    bool reacted = (p.flags & FLAG_REACTED) != 0u;
    // Off the grid it is still drawn (it flies on); only retired (non-finite) particles hide.
    if ((p.flags & FLAG_BAD) != 0u || reacted != (uPass == 0)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = 1.0;
        vColour = vec3(0.0);
        vAlpha = 0.0;
        return;
    }
    vec2 x = uFrames != 0 && uLerp > 0.0 ? mix(p.x, frameB[id].x, uLerp) : p.x;
    vec2 w = uOrigin + x * uDx;
    if ((p.flags & FLAG_PROJECTILE) != 0u) w.x += uShift;
    gl_Position = vec4((w - uCentre) / uHalfView, 0.0, 1.0);
    // Products cover their expanded area: sprites grow as sqrt(J), at most 4 x. k runs from 0
    // (dense) to 1 (expanded twentyfold).
    float J = (p.F.x * p.F.w - p.F.y * p.F.z) * p.Fzz;
    float k = reacted ? clamp(log(max(J, 1.0)) / 3.0, 0.0, 1.0) : 0.0;
    bool failed = (p.flags & FLAG_FAILED) != 0u;
    bool loose = failed || (p.flags & (FLAG_COMMINUTED | FLAG_DELAMINATED)) != 0u;
    gl_PointSize = min(uPointSize * (reacted ? clamp(sqrt(max(J, 1.0)), 1.0, 4.0) : loose ? 0.8 : 1.0), uMaxPoint);
    vAlpha = reacted ? mix(0.95, 0.5, k) : 1.0;

    vec3 base = uColours[min(p.mat, 31u)];
    float value = 0.0;
    if (uMode == 1) value = p.stress;
    else if (uMode == 2) value = p.epsP;
    else if (uMode == 3) value = p.temp;
    else if (uMode == 4) value = length(p.v);
    float f = clamp((value - uRange.x) / max(uRange.y - uRange.x, 1e-20), 0.0, 1.0);

    if (uMode == 0 && reacted) {
        // Products glow yellow-white while dense, then orange, then fade to smoke grey.
        vec3 gas = k < 0.3 ? mix(vec3(1.0, 0.95, 0.75), vec3(1.0, 0.55, 0.18), k / 0.3)
                           : mix(vec3(1.0, 0.55, 0.18), vec3(0.42, 0.41, 0.4), (k - 0.3) / 0.7);
        vColour = mix(base, gas, clamp(p.burn, 0.0, 1.0));
    } else if (uMode == 0) {
        vec3 c = base;
        if ((p.flags & FLAG_PROJECTILE) != 0u) {
            // Lit from above as a metal cylinder, from the initial lateral position (the cap
            // too); the fins are seen face-on.
            float s = (p.X0.y - uBody.z) / uBody.w;
            if (abs(s) <= 1.0) {
                vec3 n = vec3(0.0, s, sqrt(max(1.0 - s * s, 0.0)));
                const vec3 L = vec3(0.0, 0.6, 0.8);
                const vec3 H = vec3(0.0, 0.3162, 0.9487);  // normalize(L + view)
                float spec = pow(max(dot(n, H), 0.0), 28.0);
                c = base * (0.28 + 0.8 * max(dot(n, L), 0.0)) + mix(vec3(1.0), base, 0.3) * 0.55 * spec;
            } else {
                c = base * 0.85;
            }
            // Sabot grooves: fine rings over the middle half of the body, 0.15 D apart (at least
            // three particle spacings).
            if (uDart != 0 && (p.flags & FLAG_FITTING) == 0u) {
                float along = p.X0.x - uBody.x, u = along / max(uBody.y - uBody.x, 1e-6);
                float pitch = max(0.3 * uBody.w, 1.5);
                if (u > 0.25 && u < 0.75 && fract(along / pitch) < 0.34) c *= 0.7;
            }
        }
        // Debris darker, fractured ceramic darkening with D; hot metal adds its own light, dark
        // below red heat.
        c *= failed ? 0.55 : 1.0 - 0.4 * clamp(p.fracture, 0.0, 1.0);
        vColour = linearToSrgb(srgbToLinear(c) + 1.5 * glowAt(p.temp).rgb);
    } else if (uMode == 5) {
        vec3 c = base * 0.35 + vec3(0.12);
        if (reacted) c = vec3(0.85, 0.5, 0.95);
        else if ((p.flags & FLAG_FAIL_MELT) != 0u) c = vec3(0.95, 0.25, 0.2);
        else if ((p.flags & FLAG_FAIL_SPALL) != 0u) c = vec3(0.3, 0.75, 0.95);
        else if ((p.flags & FLAG_FAIL_STRAIN) != 0u) c = vec3(1.0, 0.7, 0.25);
        else if ((p.flags & FLAG_FIBRE_BROKEN) != 0u) c = vec3(1.0, 0.5, 0.68);
        else if ((p.flags & FLAG_DELAMINATED) != 0u) c = vec3(0.91, 0.85, 0.29);
        else c = mix(c, vec3(0.56, 0.82, 0.42), clamp(p.fracture, 0.0, 1.0));  // ceramic fracture
        vColour = c;
    } else {
        vColour = linearToSrgb(uMode == 2 || uMode == 4 ? viridis(f) : inferno(f));
    }

    if (uPass == 2) {
        // A soft halo whose light adds up, over a hot region, to about the glow's own level:
        // larger and brighter with temperature.
        vec4 g = glowAt(p.temp);
        if (g.a > 0.003) {
            float s = min(uPointSize * (2.0 + 4.0 * g.a), uMaxPoint);
            gl_PointSize = s;
            vColour = g.rgb * 2.0 * (uPointSize / s) * (uPointSize / s);
        } else {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            gl_PointSize = 1.0;
            vColour = vec3(0.0);
        }
    }
}
