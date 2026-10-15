// Armour workspace: one projectile against a stack of generic plates, with an animated 2D
// cross-section, time histories, a per-layer table and summary figures. Terminal ballistics of
// plates only: no vehicles, weak spots or vulnerability outputs.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

class ArmourUi {
public:
    ArmourUi();
    ~ArmourUi();

    // Reads data/armour_materials.json and data/projectiles.json; errors are shown in the UI.
    void load(const std::filesystem::path& root);
    // Draws the workspace windows (inside an ImGui frame) and advances playback by dt seconds.
    // `top` is the height of the main menu bar.
    void frame(double dt, float width, float height, float top);
    // Selects a catalogue projectile and target preset by id (empty keeps the current one),
    // sets the obliquity in degrees when |obliquityDeg| <= 90 (negative leans the plates the
    // other way) and, for fraction >= 0, pauses playback at that fraction of the animation.
    // Used by --capture.
    void setCase(const std::string& projectile, const std::string& preset, double fraction, double obliquityDeg = 1e9);
    // Pre-renders the deformation simulation (the Pre-render button); used by --prerender.
    void prerender();

    // Cross-section display: the engineering model or the deformation simulation, its colour
    // mode (MpmColour), quality (MpmQuality) and whether the plates' edge mounts give (soft mounts).
    // Stored in spall.ini.
    struct Display {
        bool deformation = false;
        int colour = 0;
        int quality = 1;
        bool softMounts = false;
    };
    Display display() const;
    void setDisplay(const Display& d);

private:
    struct State;
    std::unique_ptr<State> s_;
};
