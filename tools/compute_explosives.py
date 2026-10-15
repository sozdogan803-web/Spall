"""Generate data/explosives.json.

Measured properties (density, detonation velocity, relative effectiveness) are entered by hand
from the cited references. Composition-dependent properties are derived here:

  * oxygen balance (CO2 basis, %)
  * free-carbon (soot) yield of the detonation products, kg per kg of explosive
  * afterburn energy of the organic detonation products, MJ per kg of explosive

Detonation products follow the Kistiakowsky-Wilson rules: Cl takes H as HCl, H burns to H2O,
then C to CO, then CO to CO2; leftover C is solid carbon. Aluminium is excluded from these
products and is handled separately by the blast model. The afterburn energy is the heat of
burning the CO, C(s) and H2 products in air (283.0, 393.5 and 241.8 kJ/mol).

Run:  python tools/compute_explosives.py
"""

import json
import os

ATOMIC_MASS = {"C": 12.011, "H": 1.008, "N": 14.007, "O": 15.999, "Al": 26.982, "Cl": 35.45, "Ca": 40.078}

# Component formulas (atoms per molecule or per repeat unit). Polymers and oils use a
# representative repeat unit, which is accurate enough for oxygen balance and soot yield.
COMPONENTS = {
    "TNT": {"C": 7, "H": 5, "N": 3, "O": 6},
    "RDX": {"C": 3, "H": 6, "N": 6, "O": 6},
    "HMX": {"C": 4, "H": 8, "N": 8, "O": 8},
    "PETN": {"C": 5, "H": 8, "N": 4, "O": 12},
    "AN": {"H": 4, "N": 2, "O": 3},                  # ammonium nitrate
    "AP": {"H": 4, "N": 1, "Cl": 1, "O": 4},         # ammonium perchlorate
    "Al": {"Al": 1},
    "PA": {"C": 6, "H": 3, "N": 3, "O": 7},          # picric acid
    "AmPic": {"C": 6, "H": 6, "N": 4, "O": 7},       # ammonium picrate (Explosive D)
    "TNA": {"C": 7, "H": 5, "N": 3, "O": 7},         # trinitroanisole
    "HND": {"C": 12, "H": 5, "N": 7, "O": 12},       # hexanitrodiphenylamine
    "DNP": {"C": 6, "H": 4, "N": 2, "O": 5},         # 2,4-dinitrophenol
    "TMETN": {"C": 5, "H": 9, "N": 3, "O": 9},
    "TEGDN": {"C": 6, "H": 12, "N": 2, "O": 8},
    "wax": {"C": 25, "H": 52},                       # paraffin
    "stearic": {"C": 18, "H": 36, "O": 2},
    "DOS": {"C": 26, "H": 50, "O": 4},               # dioctyl sebacate
    "DOA": {"C": 22, "H": 42, "O": 4},               # dioctyl adipate
    "PIB": {"C": 4, "H": 8},                         # polyisobutylene
    "oil": {"C": 20, "H": 42},                       # mineral oil
    "fuel_oil": {"C": 12, "H": 26},
    "HTPB": {"C": 4, "H": 6},                        # polybutadiene repeat unit
    "PMMA": {"C": 5, "H": 8, "O": 2},                # acrylic binder
    "SBR": {"C": 12, "H": 14},                       # styrene-butadiene binder
    "PS": {"C": 8, "H": 8},                          # polystyrene
    "EO": {"C": 2, "H": 4, "O": 1},                  # ethylene oxide
    "C": {"C": 1},                                   # charcoal / graphite
    "CaCl2": {"Ca": 1, "Cl": 2},                     # inert desiccant
}
INERT = {"CaCl2"}

TABLE = "Wikipedia: TNT equivalent (relative effectiveness table)"

# id, name, composition (mass %), density g/cm3, detonation velocity m/s, RE, confidence, sources, notes
EXPLOSIVES = [
    ("tnt", "TNT", {"TNT": 100}, 1.60, 6900, 1.00, "high", [TABLE], "Trinitrotoluene; the reference explosive."),
    ("comp_b", "Composition B", {"RDX": 59.5, "TNT": 39.5, "wax": 1.0}, 1.72, 7840, 1.33, "high", [TABLE],
     "RDX/TNT 60/40 with wax. Also used here for Comp B-4 and 50/50 TNT/RDX fills."),
    ("comp_a5", "Composition A-5", {"RDX": 98.5, "stearic": 1.5}, 1.65, 8470, 1.60, "high", [TABLE], "Phlegmatized RDX."),
    ("c4", "Composition C-4", {"RDX": 91.0, "DOS": 5.3, "PIB": 2.1, "oil": 1.6}, 1.59, 8040, 1.34, "high",
     [TABLE, "Wikipedia: C-4 (explosive)"], "Plastic explosive. The C-4 article gives 8,092 m/s."),
    ("tritonal", "Tritonal (80% TNT / 20% Al)", {"TNT": 80, "Al": 20}, 1.70, 6650, 1.05, "high", [TABLE],
     "Aluminized TNT; standard fill of US general-purpose bombs."),
    ("h6", "H-6", {"RDX": 45.1, "TNT": 29.2, "Al": 21.0, "wax": 4.7, "CaCl2": 0.47}, 1.74, 7367, 1.35, "high",
     ["Wikipedia: H6 (explosive)"], "Aluminized RDX/TNT used in bombs, mines and depth charges."),
    ("hbx1", "HBX-1", {"RDX": 40.4, "TNT": 37.8, "Al": 17.1, "wax": 4.7}, 1.72, 7300, 1.27, "low",
     ["Wikipedia: HBX"], "Density and detonation velocity estimated; RE taken as 98% of Torpex."),
    ("torpex", "Torpex", {"RDX": 42, "TNT": 40, "Al": 18}, 1.80, 7440, 1.30, "high", [TABLE],
     "Aluminized RDX/TNT for underwater and blast weapons."),
    ("minol2", "Minol-2", {"TNT": 40, "AN": 40, "Al": 20}, 1.70, 5900, 1.20, "low", ["Wikipedia: Minol (explosive)"],
     "Composition sourced; density, detonation velocity and RE are estimates."),
    ("amatol_5050", "Amatol 50/50", {"AN": 50, "TNT": 50}, 1.50, 6290, 0.91, "high", [TABLE],
     "Also used where a source says only 'amatol'."),
    ("amatol_8020", "Amatol 80/20 (80% AN)", {"AN": 80, "TNT": 20}, 1.55, 6570, 1.10, "medium", [TABLE],
     "Values as listed in the RE table."),
    ("amatol_60tnt", "Amatol (60% TNT / 40% AN)", {"TNT": 60, "AN": 40}, 1.54, 6410, 0.93, "low", ["Wikipedia: Amatol"],
     "German/WWII TNT-rich grade. Properties interpolated between TNT and amatol 50/50."),
    ("trialen_105", "Trialen 105", {"TNT": 70, "RDX": 15, "Al": 15}, 1.70, 6800, 1.20, "low",
     ["Wikipedia: SC1000 bomb"], "German aluminized fill; properties estimated."),
    ("ammonal", "Ammonal (WWI)", {"AN": 65, "Al": 17, "TNT": 15, "C": 3}, 1.10, 4400, 0.90, "low",
     ["Wikipedia: Ammonal"], "Composition and detonation velocity sourced; density and RE estimated."),
    ("explosive_d", "Explosive D (ammonium picrate)", {"AmPic": 100}, 1.55, 7150, 0.95, "low",
     ["Wikipedia: Ammonium picrate"], "Crystal density 1.719 g/cm3; loaded density, velocity and RE estimated."),
    ("picric_acid", "Picric acid (Lyddite / Melinite / Shimose)", {"PA": 100}, 1.71, 7350, 1.17, "high", [TABLE], ""),
    ("shellite", "Shellite", {"PA": 70, "DNP": 30}, 1.60, 7000, 1.10, "low", ["Wikipedia: Disney bomb"],
     "Picric acid / dinitrophenol mixture; proportions and properties estimated."),
    ("hexanite", "Hexanite (TNT 60 / HND 40)", {"TNT": 60, "HND": 40}, 1.60, 7000, 0.98, "low",
     ["Wikipedia: Hexanite"], "Also Japanese Type 97 and German Schiesswolle 36. Velocity and density estimated; "
     "the source calls it slightly less powerful than TNT."),
    ("tna", "Trinitroanisole (Type 91)", {"TNA": 100}, 1.57, 7200, 1.00, "low", [], "No source cited; all values are estimates."),
    ("rdx", "RDX", {"RDX": 100}, 1.78, 8600, 1.60, "high", [TABLE], ""),
    ("hmx", "HMX", {"HMX": 100}, 1.95, 9100, 1.70, "high", [TABLE], ""),
    ("petn", "PETN", {"PETN": 100}, 1.77, 8400, 1.66, "high", [TABLE], ""),
    ("octol", "Octol 75/25", {"HMX": 75, "TNT": 25}, 1.83, 8690, 1.54, "high", [TABLE], "Used in shaped-charge warheads."),
    ("pentolite", "Pentolite 50/50", {"PETN": 50, "TNT": 50}, 1.66, 7520, 1.33, "high", [TABLE], ""),
    ("semtex_1a", "Semtex 1A", {"PETN": 76.6, "RDX": 4.6, "SBR": 18.8}, 1.55, 7670, 1.35, "medium", [TABLE],
     "Binder approximated as styrene-butadiene."),
    ("pbxn_109", "PBXN-109", {"RDX": 64, "Al": 20, "HTPB": 8, "DOA": 8}, 1.68, 7450, 1.17, "medium", [TABLE],
     "Insensitive aluminized PBX; binder split approximated."),
    ("pbxih_135", "PBXIH-135 EB (thermobaric)", {"HMX": 42, "Al": 33, "HTPB": 25}, 1.81, 7060, 1.17, "medium", [TABLE],
     "Composition approximated."),
    ("afx_757", "AFX-757", {"RDX": 25, "AP": 30, "Al": 33, "HTPB": 12}, 1.84, 6080, 1.90, "medium", [TABLE],
     "Blast-optimized PBX; composition approximated. The RE factor is the table value."),
    ("pbxn_107", "PBXN-107", {"RDX": 86, "PMMA": 14}, 1.64, 8100, 1.40, "low", [],
     "No source cited; all values are estimates. RDX with acrylic binder."),
    ("pbxn_103", "PBXN-103", {"AP": 40, "Al": 27, "TMETN": 23, "TEGDN": 10}, 1.89, 5950, 1.30, "low", [],
     "No source cited; composition and properties are estimates (underwater explosive)."),
    ("destex", "Destex", {"TNT": 74.8, "Al": 18.7, "wax": 4.7, "C": 1.8}, 1.70, 6600, 1.05, "low", [],
     "No source cited; properties are estimates (aluminized TNT with wax and graphite, tritonal-like)."),
    ("gsx", "GSX slurry", {"AN": 80, "Al": 15, "PS": 5}, 1.30, 4000, 0.85, "low", ["Wikipedia: BLU-82"],
     "Ammonium nitrate / aluminium / polystyrene gel; proportions and properties estimated."),
    ("anfo", "ANFO", {"AN": 94.3, "fuel_oil": 5.7}, 0.92, 4200, 0.74, "high", [TABLE], "Industrial blasting agent."),
    ("tgaf_5", "TGAF-5", {"TNT": 40, "RDX": 40, "Al": 17, "wax": 3}, 1.75, 7200, 1.30, "low",
     ["AOAV: What is a Grad?"], "Soviet rocket fill; composition sourced, properties estimated."),
    ("okfol", "OKFOL (HMX 95 / wax 5)", {"HMX": 95, "wax": 5}, 1.77, 8670, 1.60, "low", ["Wikipedia: RPG-7"],
     "Composition sourced; properties estimated."),
    ("thermobaric", "Thermobaric mixture (generic)", {"RDX": 30, "Al": 30, "EO": 40}, 1.30, 3000, 1.30, "low", [],
     "No source cited; illustrative composition for fuel-rich thermobaric fills, not a specific formulation."),
]


def element_moles_per_kg(composition):
    total = sum(composition.values())
    moles = {k: 0.0 for k in ATOMIC_MASS}
    inert_mass = 0.0
    for name, pct in composition.items():
        formula = COMPONENTS[name]
        molar_mass = sum(ATOMIC_MASS[el] * n for el, n in formula.items())
        frac = pct / total
        if name in INERT:
            inert_mass += frac
            continue
        mol = frac * 1000.0 / molar_mass
        for el, n in formula.items():
            moles[el] += n * mol
    return moles, inert_mass


def derived_properties(composition):
    m, _ = element_moles_per_kg(composition)
    nC, nH, nO, nAl, nCl = m["C"], m["H"], m["O"], m["Al"], m["Cl"]
    oxygen_balance = -1.6 * (2 * nC + (nH - nCl) / 2 + 1.5 * nAl - nO)  # % (CO2 basis)

    # Kistiakowsky-Wilson products, aluminium excluded.
    h = nH - nCl
    o = nO
    water = min(h / 2, o)
    o -= water
    h2 = (h - 2 * water) / 2
    co = min(nC, o)
    o -= co
    free_c = nC - co
    co2 = min(co, o)
    co -= co2
    afterburn_kj = 283.0 * co + 393.5 * free_c + 241.8 * h2
    return {
        "oxygen_balance_pct": round(oxygen_balance, 1),
        "soot_yield": round(free_c * ATOMIC_MASS["C"] / 1000.0, 4),
        "afterburn_mj_per_kg": round(afterburn_kj / 1000.0, 2),
    }


def main():
    out = []
    for (eid, name, comp, rho, vod, re, conf, sources, notes) in EXPLOSIVES:
        total = sum(comp.values())
        entry = {
            "id": eid,
            "name": name,
            "composition": [{"component": k, "mass_pct": round(v * 100.0 / total, 2)} for k, v in comp.items()],
            "re_factor": re,
            "detonation_velocity_mps": vod,
            "density_gcc": rho,
            "aluminum_fraction": round(comp.get("Al", 0.0) / total, 4),
        }
        entry.update(derived_properties(comp))
        entry["confidence"] = conf
        entry["sources"] = sources
        entry["notes"] = notes
        out.append(entry)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "data", "explosives.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"schema": 1, "explosives": out}, f, indent=2, ensure_ascii=False)
        f.write("\n")
    for e in out:
        print(f"{e['id']:<14} OB {e['oxygen_balance_pct']:>7.1f}%  soot {e['soot_yield']:.3f}  "
              f"afterburn {e['afterburn_mj_per_kg']:>5.2f} MJ/kg  Al {e['aluminum_fraction']:.2f}")
    print(f"wrote {len(out)} explosives to {path}")


if __name__ == "__main__":
    main()
