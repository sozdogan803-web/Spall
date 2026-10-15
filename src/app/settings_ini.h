// spall.ini: the window, theme and the armour-view choices, as plain "key = value" lines.
// Unknown keys and out-of-range values are skipped with a warning, so a damaged or older file
// never stops the app from starting.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct UiSettings {
    std::string theme = "classic";  // ui::themeId
    bool hasWindowRect = false;     // window position and size were stored
    int windowX = 0, windowY = 0, windowWidth = 1280, windowHeight = 800;
    bool maximized = false;
    bool armourDeformation = false;  // armour cross-section shows the deformation simulation
    int armourColour = 0;            // MpmColour
    int armourQuality = 1;           // MpmQuality
    bool armourSoftMounts = false;   // deformation simulation: plate edges tethered, not clamped

    bool operator==(const UiSettings&) const;
};

// Parses ini text over the defaults in `out`; returns the number of values applied.
int parseSettings(const std::string& text, UiSettings& out, std::vector<std::string>& warnings);
std::string formatSettings(const UiSettings& s);

// A missing file leaves the defaults and is not a warning.
void loadSettings(const std::filesystem::path& file, UiSettings& out, std::vector<std::string>& warnings);
bool saveSettings(const std::filesystem::path& file, const UiSettings& s, std::string& error);
