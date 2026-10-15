// Saving and loading armour cases (JSON, one file per case) and exporting the time histories
// (CSV). CPU only; the app decides where the files go (src/app/armour_ui.cpp).
#pragma once

#include "data/projectile_db.h"
#include "sim/armour_run.h"
#include "sim/mpm_solver.h"

#include <string>
#include <string_view>
#include <vector>

// A case as saved: the physical setup only (projectile, impact velocity, obliquity, plates), not
// the view. Layers name their materials by catalogue id, so a file outlives a reordered catalogue.
struct SavedCase {
    std::string name;
    int projectile = -1;     // ProjectileDb::projectiles() index the case started from, -1: none
    bool custom = false;     // the penetrator was edited away from that entry (its figures are saved)
    RodMaterial rodMaterial = RodMaterial::Tungsten;
    ArmourCase c;
};

std::string caseToJson(const ProjectileDb& db, const SavedCase& saved);
// Fills `out` (a catalogue projectile keeps its fuze and shape; a custom one overlays its saved
// figures). False with `error` set for malformed or out-of-range files.
bool caseFromJson(const ProjectileDb& db, std::string_view text, SavedCase& out, std::string& error);

// A file stem for a case name: lower case letters, digits, '_' and '-', at most 48 characters;
// "case" when nothing is left.
std::string safeCaseName(std::string_view name);

// The engineering model's penetration history as CSV: time_us, depth_mm, velocity_m_s, length_mm,
// rate_m_s, layer.
std::string historyCsv(const ArmourOutcome& out);
// The deformation simulation's readouts, one row per measurement.
std::string mpmHistoryCsv(const std::vector<MpmMeasure>& measures);
