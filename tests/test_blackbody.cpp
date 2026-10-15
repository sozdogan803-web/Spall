#include "render/blackbody.h"
#include "test.h"

TEST(blackbody_6500K_chromaticity_on_planckian_locus) {
    const Xyz c = blackbodyXYZ(6500.0);
    const double sum = c.X + c.Y + c.Z;
    CHECK_NEAR(c.X / sum, 0.313, 0.01);
    CHECK_NEAR(c.Y / sum, 0.324, 0.01);
}

TEST(blackbody_1500K_is_red_orange) {
    const std::vector<float> lut = makeBlackbodyLut(2, 1500.0f, 1500.0f);
    CHECK(lut[0] > lut[1]);
    CHECK(lut[1] > lut[2]);
}

TEST(blackbody_luminance_increases_with_temperature) {
    const std::vector<float> lut = makeBlackbodyLut(1024, 500.0f, 6000.0f);
    CHECK(lut.size() == 1024u * 4u);
    bool monotonic = true;
    for (size_t i = 1; i < 1024; ++i) monotonic = monotonic && lut[i * 4 + 3] > lut[(i - 1) * 4 + 3];
    CHECK(monotonic);
}

TEST(blackbody_absolute_luminance) {
    // A 2042 K blackbody (platinum freezing point) was the old candela standard: 60 cd/cm2.
    CHECK_NEAR(blackbodyXYZ(2042.0).Y / 6.0e5, 1.0, 0.05);
}

TEST(blackbody_display_glow) {
    // Dark below the Draper point, then brighter and whiter with temperature.
    CHECK(glowBrightness(700) == 0);
    CHECK(glowBrightness(900) > 0.05 && glowBrightness(900) < 0.25);
    CHECK_NEAR(glowBrightness(2000), 1.0, 0);
    CHECK_NEAR(glowBrightness(3500), 1.0, 0);
    const std::vector<float> lut = makeGlowLut(65, 500.0f, 3700.0f);  // 50 K apart
    bool rising = true;
    for (size_t i = 1; i < 65; ++i) rising = rising && lut[i * 4 + 3] >= lut[(i - 1) * 4 + 3];
    CHECK(rising);
    const float* red = &lut[10 * 4];      // 1000 K
    const float* yellow = &lut[50 * 4];   // 3000 K
    CHECK(red[0] > 0 && red[0] > 4 * red[1] && red[1] >= red[2]);
    CHECK_NEAR(yellow[0], 1.0, 1e-6);
    CHECK(yellow[1] > 0.35 && yellow[1] < 0.6 && yellow[1] > yellow[2]);  // linear, ~(1, 0.46, 0.15)
}
