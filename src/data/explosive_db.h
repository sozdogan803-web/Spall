// Explosive-filler catalogue loaded from data/explosives.json. Values are approximate public
// figures, for visualization and education only.
#pragma once

#include <string>
#include <string_view>
#include <vector>

enum class Confidence { High, Medium, Low };

const char* confidenceLabel(Confidence c);

struct Component {
    std::string name;
    double massPct = 0;
};

struct Explosive {
    std::string id;
    std::string name;
    std::vector<Component> composition;
    double reFactor = 1;             // relative effectiveness vs TNT
    double detonationVelocity = 0;   // m/s at nominal density
    double density = 0;              // kg/m3 (the JSON stores g/cm3)
    double aluminumFraction = 0;     // mass fraction
    double oxygenBalancePct = 0;     // CO2 basis; negative = fuel-rich
    double sootYield = 0;            // kg free carbon per kg filler (Kistiakowsky-Wilson)
    double afterburnMJPerKg = 0;     // organic afterburn energy
    Confidence confidence = Confidence::Medium;
    std::vector<std::string> sources;
    std::string notes;
};

struct LoadReport {
    int skipped = 0;                 // invalid entries left out of the catalogue
    int warnings = 0;                // entries loaded with a warning
    std::vector<std::string> messages;
};

class ExplosiveDb {
public:
    // A missing file, a JSON syntax error or a wrong schema is fatal: returns false with
    // `fatal` set ("explosives.json: line L, column C: ...") and leaves the catalogue empty.
    // Invalid entries are skipped and listed in `report`.
    bool load(const std::string& path, LoadReport& report, std::string& fatal);
    bool loadText(std::string_view json, LoadReport& report, std::string& fatal);

    const std::vector<Explosive>& explosives() const { return explosives_; }
    int findExplosive(std::string_view id) const;

private:
    std::vector<Explosive> explosives_;
};
