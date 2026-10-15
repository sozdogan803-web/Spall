// Projectile and armour-material catalogue for the armour workspace, loaded from
// data/armour_materials.json and data/projectiles.json. Approximate open-source figures, for
// terminal-ballistics education only.
#pragma once

#include "data/explosive_db.h"
#include "physics/mpm_material.h"
#include "physics/penetration.h"
#include "physics/projectile_shape.h"

#include <string>
#include <string_view>
#include <vector>

enum class MaterialGroup { Steel, LightMetal, Ceramic, Other, Count };
const char* materialGroupId(MaterialGroup g);     // "steel", "light_metal", "ceramic", "other"
const char* materialGroupLabel(MaterialGroup g);  // "Steel", "Light metal", ...

enum class RodMaterial { Tungsten, Uranium, Steel, Count };
const char* rodMaterialId(RodMaterial m);     // "tungsten", "du", "steel"
const char* rodMaterialLabel(RodMaterial m);  // "Tungsten alloy", "Depleted uranium", "Steel"
double rodMaterialDensity(RodMaterial m);     // kg/m3
phys::MpmMaterial rodMaterialMpm(RodMaterial m);  // deformation-simulation parameters

constexpr int kMaxArmourLayers = 8;

struct ArmourMaterialEntry {
    std::string id;
    std::string name;
    MaterialGroup group = MaterialGroup::Steel;
    phys::ArmourMaterial material;
    bool resistanceFromHardness = false;  // Rt derived from Brinell hardness (steels)
    phys::MpmMaterial mpm;                // deformation-simulation parameters
    bool mpmFromData = false;             // an "mpm" block was given (else group defaults)
    unsigned color = 0x8b939cu;           // 0xRRGGBB, for drawing
    Confidence confidence = Confidence::Low;
    std::vector<std::string> sources;
    std::string notes;
};

// Deformation-simulation defaults for a material without an "mpm" block, from its group,
// density, hardness and Tate resistance.
phys::MpmMaterial mpmFallback(MaterialGroup group, const phys::ArmourMaterial& material);

struct PresetLayer {
    int material = 0;        // index into ProjectileDb::materials()
    double thickness = 0;    // m
    double gap = 0;          // m
    // Separate tiles along the face (ERA, src/data/armour_blocks.h): tile k centred on
    // offset + k (blockWidth + blockGap) from the shot line. blockWidth 0: one full plate.
    double blockWidth = 0;   // m
    double blockGap = 0;     // m
    double offset = 0;       // m
    int blockCount = 0;      // that many blocks, centred on the offset; 0: tiles repeat across the whole plate
};

struct ArmourPreset {
    std::string id;
    std::string name;
    double obliquityDeg = 0;
    std::vector<PresetLayer> layers;
};

struct ProjectileEntry {
    std::string id;
    std::string name;
    std::string country;
    int year = 0;
    std::string gun;                 // gun or launcher
    phys::Penetrator penetrator;     // shaped charges: jetQuality fitted to publishedRha
    RodMaterial rodMaterial = RodMaterial::Steel;  // kinetic rounds
    double velocity = 0;             // m/s; impact velocity at the muzzle (shaped charges: flight speed, display only)
    bool hasPublished = false;
    double publishedRha = 0;         // m
    std::string publishedConditions;
    double publishedVelocity = 0;    // m/s, impact velocity for the published figure (kinetic rounds)
    Confidence confidence = Confidence::Low;
    std::vector<std::string> sources;
    std::string notes;
    phys::ShapeSpec shape;           // present = false: the generic shape for the kind
};

class ProjectileDb {
public:
    // Same contract as ExplosiveDb::load: a missing file, syntax error or wrong schema is fatal;
    // invalid entries are skipped and listed in `report`. `load` also reads explosives.json next
    // to the projectiles file (the fillers' explosives).
    bool load(const std::string& materialsPath, const std::string& projectilesPath, LoadReport& report,
              std::string& fatal);
    bool loadText(std::string_view materialsJson, std::string_view projectilesJson, LoadReport& report,
                  std::string& fatal, std::string_view explosivesJson = {});

    const std::vector<ArmourMaterialEntry>& materials() const { return materials_; }
    const std::vector<ArmourPreset>& presets() const { return presets_; }
    const std::vector<ProjectileEntry>& projectiles() const { return projectiles_; }
    int findMaterial(std::string_view id) const;
    int findProjectile(std::string_view id) const;
    const std::vector<Explosive>& explosives() const { return explosives_; }
    int findExplosive(std::string_view id) const;

private:
    std::vector<ArmourMaterialEntry> materials_;
    std::vector<ArmourPreset> presets_;
    std::vector<ProjectileEntry> projectiles_;
    std::vector<Explosive> explosives_;
};
