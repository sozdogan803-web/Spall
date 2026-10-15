#version 460
// Composites the products buffer (shaders/mpm/products.*) over the Cross-section background as
// smoke and fire. Half the buffer is taken blurred (mipmap level 2.5), so products particles
// that the simulation has left apart read as a thin haze rather than separate dots. Smoke
// opacity 1 - exp(-tau) with tau proportional to the relative density, lit from above through
// the smoke over it; emission from the mean temperature through the display glow table
// (src/render/blackbody.h) at a shorter exposure than hot metal (brightness cubed, so 1200 K is
// a dim red and only the core above 2000 K is yellow-white), weighted by the opacity squared so
// thin gas at the edges reads as smoke and the dense core as flame. Writes display sRGB.
uniform sampler2D uGas;
uniform sampler2D uGlow;     // makeGlowLut over uGlowRange
uniform vec2 uGlowRange;     // K
uniform vec3 uBackground;    // linear
uniform float uOpacity;      // optical depth of dense products
uniform float uShadowStep;   // px between the samples towards the light
out vec4 fragColour;

vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(uGas, 0);
    vec2 uv = (gl_FragCoord.xy) / vec2(size);
    vec4 g = mix(texelFetch(uGas, px, 0), textureLod(uGas, uv, 2.5), 0.5);
    float rho = max(g.x, 0.0);
    vec3 c = uBackground;
    if (rho > 1e-5) {
        float T = 1000.0 * g.y / rho, k = clamp(g.z / rho, 0.0, 1.0);
        float alpha = 1.0 - exp(-uOpacity * rho);
        // Smoke over this point towards the light (up the screen) shades it.
        float above = 0.0;
        for (int i = 1; i <= 6; ++i) {
            vec2 q = uv + vec2(0.0, float(i) * uShadowStep / float(size.y));
            above += max(textureLod(uGas, q, 1.5).x, 0.0);
        }
        float lit = exp(-0.12 * uOpacity * above);
        // Dense smoke is sooty, expanded smoke a lighter grey.
        vec3 smoke = mix(vec3(0.018, 0.016, 0.014), vec3(0.09, 0.085, 0.08), k) * (0.45 + 0.9 * lit);
        c = mix(c, smoke, alpha);
        float u = clamp((T - uGlowRange.x) / (uGlowRange.y - uGlowRange.x), 0.0, 1.0);
        float n = float(textureSize(uGlow, 0).x);
        vec4 glow = texture(uGlow, vec2((u * (n - 1.0) + 0.5) / n, 0.5));
        vec3 emit = glow.rgb * glow.a * glow.a * alpha * alpha;
        // Added light that saturates towards white in the dense hot core.
        c = 1.0 - (1.0 - c) * exp(-4.0 * emit);
    }
    fragColour = vec4(linearToSrgb(c), 1.0);
}
