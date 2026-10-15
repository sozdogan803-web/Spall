// CPU mirrors of shaders/common/colormap.glsl for the colour bars drawn by the UI. Input in
// [0, 1]; output is display sRGB in [0, 1] (the shaders return linear sRGB for the tone map).
#pragma once

struct Rgb {
    float r, g, b;
};

Rgb infernoSrgb(float t);
Rgb viridisSrgb(float t);
Rgb divergingSrgb(float t);
