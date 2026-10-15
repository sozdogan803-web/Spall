// Colour of blackbody radiation: Planck's law integrated against the CIE 1931 2-degree colour
// matching functions (Wyman, Sloan & Shirley 2013 multi-lobe fit), converted to linear sRGB.
// Luminance is absolute (cd/m2), so the lookup table can be used directly as emitted radiance.
#pragma once

#include <vector>

struct Xyz {
    double X = 0, Y = 0, Z = 0;  // Y is the luminance in cd/m2
};

// CIE 1931 colour matching functions at wavelength `nm` (analytic fit).
void cieColourMatch(double nm, double& x, double& y, double& z);
// Planck spectral radiance, W / (m2 sr m), at wavelength `metres` and temperature `kelvin`.
double planckRadiance(double metres, double kelvin);
// Tristimulus values of a blackbody surface, integrated over 380-780 nm in 1 nm steps.
Xyz blackbodyXYZ(double kelvin);
// Linear sRGB (D65) of an XYZ colour; negative components (outside the gamut) are clipped.
void xyzToLinearSrgb(const Xyz& c, float rgb[3]);
// `n` entries spaced evenly from Tmin to Tmax: linear sRGB radiance in cd/m2 and, in alpha,
// the luminance before gamut clipping.
std::vector<float> makeBlackbodyLut(int n, float Tmin, float Tmax);

// How bright a hot surface looks against a dark background on a display, 0-1: rises with the
// logarithm of its luminance, from 0 below about 780 K to 1 from 2000 K.
double glowBrightness(double kelvin);
// Display glow, `n` entries spaced evenly from Tmin to Tmax: the blackbody's linear sRGB
// chromaticity (brightest channel 1) times glowBrightness, and the brightness in alpha.
std::vector<float> makeGlowLut(int n, float Tmin, float Tmax);
