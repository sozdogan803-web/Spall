#include "test.h"
#include "app/settings_ini.h"

TEST(settings_ini_round_trip) {
    UiSettings s;
    s.theme = "luna";
    s.hasWindowRect = true;
    s.windowX = -8;
    s.windowY = 40;
    s.windowWidth = 1600;
    s.windowHeight = 900;
    s.maximized = true;
    s.armourDeformation = true;
    s.armourColour = 3;
    s.armourQuality = 4;
    s.armourSoftMounts = true;

    UiSettings back;
    std::vector<std::string> warnings;
    const int applied = parseSettings(formatSettings(s), back, warnings);
    CHECK(warnings.empty());
    CHECK(applied == 11);
    CHECK(back == s);
    CHECK_EQ_STR(back.theme, "luna");
    CHECK(back.armourQuality == 4);
    CHECK(back.armourSoftMounts);
}

TEST(settings_ini_rejects_bad_values) {
    std::vector<std::string> warnings;
    UiSettings s;
    const int applied = parseSettings("# comment\n"
                                      "[ui]\n"
                                      "ui.theme = luna\n"
                                      "ui.window_width = 100\n"        // below the minimum
                                      "armour.quality = 2.5\n"         // not an integer
                                      "armour.colour = 9\n"            // out of range
                                      "armour.deformation = maybe\n"   // not a flag
                                      "ui.window_height = 12abc\n"     // trailing junk
                                      "no.such.key = 1\n"
                                      "junk line without equals\n"
                                      "  armour.soft_mounts =  true  \r\n",
                                      s, warnings);
    CHECK(applied == 2);
    CHECK_EQ_STR(s.theme, "luna");
    CHECK(s.armourSoftMounts);
    CHECK(warnings.size() == 7);
    const UiSettings defaults;
    CHECK(s.windowWidth == defaults.windowWidth);
    CHECK(s.windowHeight == defaults.windowHeight);
    CHECK(s.armourQuality == defaults.armourQuality);
    CHECK(s.armourColour == defaults.armourColour);
    CHECK(s.armourDeformation == defaults.armourDeformation);
}

TEST(settings_ini_ignores_retired_keys_with_a_warning) {
    // Files written by versions that still had the explosion workspace carry its keys.
    std::vector<std::string> warnings;
    UiSettings s;
    const int applied = parseSettings("ui.weapon = mk82\nsim.grid = high\nview.bloom = false\nui.theme = luna\n", s,
                                      warnings);
    CHECK(applied == 1);
    CHECK(warnings.size() == 3);
    CHECK_EQ_STR(s.theme, "luna");
}

TEST(settings_ini_empty_and_missing) {
    std::vector<std::string> warnings;
    UiSettings s;
    CHECK(parseSettings("", s, warnings) == 0);
    CHECK(warnings.empty());
    CHECK(s == UiSettings{});
    loadSettings("Z:/definitely/not/here/spall.ini", s, warnings);
    CHECK(warnings.empty());
    CHECK(s == UiSettings{});
}
