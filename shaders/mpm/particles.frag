#version 460
// Solid material is drawn as squares, explosive products as soft round puffs (blended), glow
// as gaussian halos (added); the colour is already display sRGB.
uniform int uPass;  // 0 = products, 1 = solids, 2 = glow
in vec3 vColour;
in float vAlpha;
out vec4 fragColour;

void main() {
    if (uPass == 0) {
        vec2 d = gl_PointCoord - 0.5;
        float r2 = 4.0 * dot(d, d);
        if (r2 > 1.0) discard;
        fragColour = vec4(vColour, vAlpha * (1.0 - 0.6 * r2));
    } else if (uPass == 2) {
        vec2 d = gl_PointCoord - 0.5;
        float r2 = 4.0 * dot(d, d);
        if (r2 > 1.0) discard;
        fragColour = vec4(vColour * exp(-4.0 * r2), 0.0);
    } else {
        fragColour = vec4(vColour, 1.0);
    }
}
