// Interactive viewer: the armour vs projectile workspace, in the Classic or Luna theme. Layout,
// theme and view settings persist in spall.ini next to the executable's data folder.
#pragma once

#include <filesystem>
#include <string>

struct GLFWwindow;

constexpr double kNoObliquity = 1e9;  // keep the preset's

struct AppOptions {
    std::string projectile, preset;      // --projectile / --preset ids for the armour workspace
    double armourAt = -1;                // --at: pause the armour animation at this fraction (0-1)
    double obliquity = kNoObliquity;     // --obliquity: plate angle in degrees (< 0 leans the other way)
    bool armourMpm = false;              // --armour-mpm: show the deformation simulation
    bool prerender = false;              // --prerender: pre-render it (playback from frames)
    std::string mpmColour;               // --mpm-colour material|stress|strain|temperature|speed|failure
    std::string mpmQuality;              // --mpm-quality low|medium|high|ultra|insane|ludicrous
    bool mpmSoftMounts = false;          // --mpm-soft-mounts: tether the plate edges instead of clamping them
    std::filesystem::path capture;       // --capture: render `frames` frames off-screen, save a PNG, exit
    int frames = 90;                     // --frames, at a fixed 1/60 s step while capturing
    std::string theme;                   // --theme classic|luna (overrides spall.ini)
    bool sizeGiven = false;              // --size given: ignore the stored window rectangle
};

// Runs the main loop until the window closes (or the capture is written); returns the process
// exit code.
int runApp(GLFWwindow* window, const std::filesystem::path& root, const AppOptions& options = {});
