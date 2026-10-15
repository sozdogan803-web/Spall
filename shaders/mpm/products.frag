#version 460
// One products puff (shaders/mpm/products.vert): a soft disc with a ragged, lumpy fbm
// pattern and hotter and cooler patches (+-20 %, so the flame is patchy as turbulent mixing
// makes it). Adds (density, density x temperature, density x expansion) to the products buffer.
in float vDeposit;
in float vTemp;
in float vExpansion;
flat in vec2 vSeed;
out vec4 fragColour;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0)), u.x),
               u.y);
}

float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int o = 0; o < 4; ++o) {
        s += a * valueNoise(p);
        p = p * 2.03 + vec2(17.1, 9.4);
        a *= 0.5;
    }
    return s / 0.9375;
}

void main() {
    vec2 d = gl_PointCoord - 0.5;
    float n = fbm(d * 3.5 + vSeed);
    // Ragged edge and lumpy body; the mean footprint stays near (1 - r^2)^2.
    float r = 2.0 * length(d) * (1.0 + 0.8 * (n - 0.5));
    if (r >= 1.0) discard;
    float a = 1.0 - r * r;
    float w = vDeposit * a * a * (0.35 + 1.3 * n);
    float hot = 0.8 + 0.4 * fbm(d * 2.5 + vSeed.yx + 31.7);
    fragColour = vec4(w, w * vTemp * hot, w * vExpansion, 0.0);
}
