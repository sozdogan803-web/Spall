#include "render/colormap.h"

#include <algorithm>

namespace {

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

Rgb polynomial(float t, const float (&c)[7][3]) {
    t = clamp01(t);
    float out[3];
    for (int k = 0; k < 3; ++k) {
        float v = c[6][k];
        for (int i = 5; i >= 0; --i) v = c[i][k] + t * v;
        out[k] = clamp01(v);
    }
    return {out[0], out[1], out[2]};
}

}  // namespace

Rgb infernoSrgb(float t) {
    static const float c[7][3] = {
        {0.0002189403691192265f, 0.001651004631001012f, -0.01948089843709184f},
        {0.1065134194856116f, 0.5639564367884091f, 3.932712388889277f},
        {11.60249308247187f, -3.972853965665698f, -15.9423941062914f},
        {-41.70399613139459f, 17.43639888205313f, 44.35414519872813f},
        {77.162935699427f, -33.40235894210092f, -81.80730925738993f},
        {-71.31942824499214f, 32.62606426397723f, 73.20951985803202f},
        {25.13112622477341f, -12.24266895238567f, -23.07032500287172f},
    };
    return polynomial(t, c);
}

Rgb viridisSrgb(float t) {
    static const float c[7][3] = {
        {0.2777273272234177f, 0.005407344544966578f, 0.3340998053353061f},
        {0.1050930431085774f, 1.404613529898575f, 1.384590162594685f},
        {-0.3308618287255563f, 0.214847559468213f, 0.09509516302823659f},
        {-4.634230498983486f, -5.799100973351585f, -19.33244095627987f},
        {6.228269936347081f, 14.17993336680509f, 56.69055260068105f},
        {4.776384997670288f, -13.74514537774601f, -65.35303263337234f},
        {-5.435455855934631f, 4.645852612178535f, 26.3124352495832f},
    };
    return polynomial(t, c);
}

Rgb divergingSrgb(float t) {
    t = clamp01(t);
    const Rgb blue{0.230f, 0.299f, 0.754f}, white{0.865f, 0.865f, 0.865f}, red{0.706f, 0.016f, 0.150f};
    const Rgb& a = t < 0.5f ? blue : white;
    const Rgb& b = t < 0.5f ? white : red;
    const float s = t < 0.5f ? 2.0f * t : 2.0f * t - 1.0f;
    return {a.r + (b.r - a.r) * s, a.g + (b.g - a.g) * s, a.b + (b.b - a.b) * s};
}
