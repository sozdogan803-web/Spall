#include "app/armour_ui.h"
#include "app/projectile_draw.h"

#include "core/log.h"
#include "core/sysmem.h"
#include "data/armour_blocks.h"
#include "data/projectile_db.h"
#include "render/colormap.h"
#include "render/mpm_snapshots.h"
#include "render/mpm_view.h"
#include "sim/armour_case_io.h"
#include "sim/armour_run.h"
#include "sim/mpm_checkpoints.h"
#include "sim/mpm_frames.h"
#include "sim/mpm_scene.h"
#include "sim/mpm_solver.h"
#include "sim/mpm_store.h"
#include "sim/witness_readout.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kPlotSamples = 300;
constexpr unsigned kBackground = 0x16191du;
constexpr double kMpmFrameBudgetMs = 10;  // simulation GPU time per frame while playing
constexpr double kMaxEndScale = 10;       // the animation's end can be stretched this far
// The workspace windows tile the main window below the toolbar.
constexpr ImGuiWindowFlags kTiled = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

ImU32 colour(unsigned rgb, float alpha = 1.0f, float shade = 1.0f) {
    const float r = static_cast<float>((rgb >> 16) & 255u) / 255.0f * shade;
    const float g = static_cast<float>((rgb >> 8) & 255u) / 255.0f * shade;
    const float b = static_cast<float>(rgb & 255u) / 255.0f * shade;
    return ImGui::ColorConvertFloat4ToU32({std::min(r, 1.0f), std::min(g, 1.0f), std::min(b, 1.0f), alpha});
}

constexpr unsigned kJetColour = 0xd08a4au;      // a copper liner and its jet

unsigned rodColour(RodMaterial m) {
    switch (m) {
    case RodMaterial::Tungsten: return 0x9da2a8u;
    case RodMaterial::Uranium: return 0x9a9a62u;
    default: return 0xc9a46au;
    }
}

// Clips a convex polygon to the half-plane a*x + b*y <= c (Sutherland-Hodgman).
void clipHalfPlane(std::vector<std::array<double, 2>>& poly, double a, double b, double c) {
    std::vector<std::array<double, 2>> out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& p = poly[i];
        const auto& q = poly[(i + 1) % poly.size()];
        const double fp = a * p[0] + b * p[1] - c, fq = a * q[0] + b * q[1] - c;
        if (fp <= 0) out.push_back(p);
        if ((fp < 0 && fq > 0) || (fp > 0 && fq < 0)) {
            const double k = fp / (fp - fq);
            out.push_back({p[0] + k * (q[0] - p[0]), p[1] + k * (q[1] - p[1])});
        }
    }
    poly.swap(out);
}

// History sample interpolated at time t (clamped to the recorded range).
phys::PenetrationSample sampleAt(const std::vector<phys::PenetrationSample>& h, double t) {
    if (h.empty()) return {};
    if (t <= h.front().time) return h.front();
    if (t >= h.back().time) return h.back();
    const auto it = std::upper_bound(h.begin(), h.end(), t,
                                     [](double v, const phys::PenetrationSample& s) { return v < s.time; });
    const phys::PenetrationSample& b = *it;
    const phys::PenetrationSample& a = *(it - 1);
    const double f = b.time > a.time ? (t - a.time) / (b.time - a.time) : 1.0;
    phys::PenetrationSample s = b;
    s.time = t;
    s.depth = a.depth + f * (b.depth - a.depth);
    s.velocity = a.velocity + f * (b.velocity - a.velocity);
    s.length = a.length + f * (b.length - a.length);
    s.craterRadius = a.craterRadius + f * (b.craterRadius - a.craterRadius);
    return s;
}

std::string mm(double metres, int decimals = 0) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f mm", decimals, metres * 1e3);
    return buf;
}

void plot(const char* title, const char* unit, const std::vector<float>& ys, double t0, double t1, double tNow,
          float height) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 100.0f);
    ImGui::InvisibleButton(title, {width, height});
    const ImVec2 end{origin.x + width, origin.y + height};
    draw->AddRectFilled(origin, end, colour(0x101215u));
    draw->AddRect(origin, end, colour(0x3a3f46u));
    if (ys.size() < 2 || t1 <= t0) return;
    float lo = std::min(0.0f, *std::min_element(ys.begin(), ys.end()));
    float hi = *std::max_element(ys.begin(), ys.end());
    if (hi - lo < 1e-6f) hi = lo + 1.0f;
    const float pad = 4.0f, top = origin.y + 16.0f, bottom = end.y - pad;
    auto px = [&](size_t i) { return origin.x + pad + (width - 2 * pad) * static_cast<float>(i) / (ys.size() - 1); };
    auto py = [&](float v) { return bottom - (bottom - top) * (v - lo) / (hi - lo); };
    std::vector<ImVec2> pts(ys.size());
    for (size_t i = 0; i < ys.size(); ++i) pts[i] = {px(i), py(ys[i])};
    draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), colour(0x7fb6ffu), 0, 1.6f);
    const double f = (tNow - t0) / (t1 - t0);
    float current = ys.back();
    if (f >= 0 && f <= 1) {
        const float x = origin.x + pad + (width - 2 * pad) * static_cast<float>(f);
        draw->AddLine({x, origin.y + 1}, {x, end.y - 1}, colour(0xffcc66u, 0.8f));
        current = ys[static_cast<size_t>(std::lround(f * (ys.size() - 1)))];
    } else if (f < 0) {
        current = ys.front();
    }
    char label[128];
    std::snprintf(label, sizeof(label), "%s: %.4g %s  (max %.4g)", title, current, unit, hi);
    draw->AddText({origin.x + 6, origin.y + 2}, colour(0xd0d4dau), label);
    if (ImGui::IsItemHovered()) {
        const float mf = std::clamp((ImGui::GetIO().MousePos.x - origin.x - pad) / (width - 2 * pad), 0.0f, 1.0f);
        const size_t i = static_cast<size_t>(std::lround(mf * (ys.size() - 1)));
        ImGui::SetTooltip("t = %.1f \xC2\xB5s\n%.4g %s", (t0 + mf * (t1 - t0)) * 1e6, ys[i], unit);
    }
}

}  // namespace

struct ArmourUi::State {
    ProjectileDb db;
    std::string error;
    int projectile = -1;      // catalogue entry the case started from
    bool custom = false;      // penetrator edited away from the catalogue entry
    RodMaterial rodMaterial = RodMaterial::Tungsten;
    ArmourCase c;
    int preset = -1;          // -1 = edited stack
    ArmourOutcome out;
    double publishedModel = 0;  // model depth at the published velocity, m
    bool dirty = true;
    bool valid = false;
    double evalMs = 0;
    // Resampled time histories for the plots.
    double plotT0 = 0, plotT1 = 0;
    std::vector<float> plotDepth, plotVelocity, plotLength;
    // Playback, physical seconds.
    double t = 0;
    bool playing = true;
    bool loop = false;
    float playSeconds = 6;  // wall-clock seconds the model's event takes to play
    double endScale = 1;  // animation length over the model's (baseEnd)
    // Cross-section view: centre (m) and pixels per metre.
    bool fit = true;
    double cx = 0, cy = 0, scale = 1000;
    // Dragging a plate (or a group of touching tiles) in the cross-section.
    int dragLayer = -1;         // layer under the mouse when the drag started, -1: panning
    bool draggingPlate = false; // a plate is moving: the simulation waits for the release
    bool keepView = false;      // the next simulation rebuild keeps the view (a plate was moved)
    // Deformation simulation display (MPM).
    std::filesystem::path root;
    bool deformation = false;
    int mpmColour = 0;   // MpmColour
    int mpmQuality = 1;  // MpmQuality
    bool mpmSoftMounts = false;  // plate edges tethered by damped springs, not clamped
    bool mpmUseSystemRam = true;  // particles and frames that do not fit video memory wait in system memory
    bool mpmStale = true;    // case or quality changed: rebuild the scene
    bool mpmInit = false;    // shaders compiled (or mpmError set)
    bool mpmBehind = false;  // still simulating up to t
    bool blocking = false;   // --capture: simulate up to t within the frame
    std::string mpmError, mpmMessage;
    std::string mpmNote;  // a quality level gave way to a lower one (shown beside the readouts)
    MpmSolver solver;
    MpmView mpmView;
    // Pre-render: the whole run simulated first into frames, then played at any speed.
    MpmSnapshots snaps;
    // Live: copies of the state along the run, so a seek back resumes from one instead of starting again.
    MpmCheckpoints ckpt;
    bool prerendering = false;
    double snapEnd = 0;  // animEnd() the frames were made for
    MpmMeasure measure;
    double mpmReach = 0;  // furthest the penetrator got this run, m (it may leave the grid)
    std::vector<MpmMeasure> mpmLog;  // the live run's readouts so far (for the CSV; pre-rendered runs keep theirs)
    // Witness sheets are read back from the GPU on request, never every frame.
    std::vector<WitnessReadout> witness;
    double witnessTime = 0;  // simulated time of that reading
    bool witnessRead = false;
    phys::ProjectileShape shape;  // the round's parts, rebuilt with the case
    // Saved cases (<root>/cases/*.json) and exported histories (<root>/exports/*.csv).
    char caseName[64] = "";
    std::vector<std::string> caseFiles;
    std::string fileMessage;

    const ProjectileEntry* entry() const {
        return projectile >= 0 ? &db.projectiles()[static_cast<size_t>(projectile)] : nullptr;
    }
    bool kinetic() const { return c.penetrator.kind != phys::PenetratorKind::ShapedCharge; }
    bool warhead() const { return c.penetrator.kind == phys::PenetratorKind::HeWarhead; }
    // No intact penetrator left in the grid, yet most of it never failed: it flew out of the region.
    bool mpmLeftGrid() const {
        if (!measure.valid || measure.intact > 0 || !solver.loaded()) return false;
        const int failed = measure.failedStrain + measure.failedSpall + measure.failedMelt;
        return mpmReach > stackBack() || 2 * failed < solver.scene().bodyCount;
    }
    double animStart() const {
        if (c.penetrator.kind == phys::PenetratorKind::LongRod)
            return -0.3 * c.penetrator.length / std::max(c.velocity, 1.0);
        return out.result.startTime;
    }
    // The simulation runs a quarter longer than the model's event (MpmScene::tEnd).
    double baseEnd() const {
        const double end = deformation && kinetic() ? 1.25 * out.result.endTime : out.result.endTime;
        return std::max(end, animStart() + 1e-7);
    }
    // The user can let the animation run on past that, up to kMaxEndScale times as long.
    double animEnd() const { return animStart() + (baseEnd() - animStart()) * endScale; }
    // Plates leaning the other way (negative obliquity) are drawn mirrored across the shot line.
    double mirrorY() const { return c.obliquityDeg < 0 ? -1.0 : 1.0; }
    double theta() const { return std::clamp(std::abs(c.obliquityDeg), 0.0, phys::kMaxObliquityDeg) * kPi / 180.0; }
    double stackBack() const { return out.result.layers.empty() ? 0.0 : out.result.layers.back().back; }

    void selectProjectile(int index);
    void applyPreset(int index);
    void evaluate();
    void setupWindow(float x, float y, float w, float h);
    void tilesPopup(int layer);
    void casesPopup();
    void listCases();
    void saveCase();
    void loadCase(const std::string& stem);
    void applyCase(const SavedCase& saved);
    void exportCsv();
    void readWitness();
    void witnessReadouts();
    void resultsWindow(float x, float y, float w, float h);
    void historyWindow(float x, float y, float w, float h);
    void sectionWindow(float x, float y, float w, float h);
    void drawSection(ImDrawList* draw, ImVec2 origin, ImVec2 size);
    // The plates as drawn: each layer's tiles (or the whole plate) between -half and half along
    // the face, as quads in world coordinates (shot line along x, first face at the origin).
    struct PlatePiece {
        int layer = 0;
        std::array<std::array<double, 2>, 4> q{};
    };
    std::vector<PlatePiece> platePieces(double half) const;
    int plateAt(double wx, double wy, double tolerance) const;  // layer under a point, -1: none
    void dragPlate(double dx, double dy);                        // moves dragLayer by (dx, dy), m
    void drawOverlays(ImDrawList* draw, ImVec2 origin, ImVec2 size, const std::string& phase);
    void updateMpm();
    void stepPrerender();
    void loadMpmScene();
    void drawMpm(ImDrawList* draw, ImVec2 origin, ImVec2 size);
    void mpmReadouts();
    void fuzeReadouts();
};

void ArmourUi::State::selectProjectile(int index) {
    projectile = index;
    custom = false;
    const ProjectileEntry& e = db.projectiles()[static_cast<size_t>(index)];
    c.penetrator = e.penetrator;
    c.velocity = e.velocity;
    rodMaterial = e.rodMaterial;
    // HE missiles have no engineering estimate: their story is in the deformation simulation.
    if (warhead()) deformation = true;
    dirty = true;
    fit = true;
}

void ArmourUi::State::applyPreset(int index) {
    preset = index;
    const ArmourPreset& p = db.presets()[static_cast<size_t>(index)];
    c.layers = p.layers;
    c.obliquityDeg = p.obliquityDeg;
    dirty = true;
    fit = true;
}

void ArmourUi::State::evaluate() {
    const auto start = std::chrono::steady_clock::now();
    const double fraction = (t - animStart()) / (animEnd() - animStart());
    out = evaluateArmour(db, c);
    const ProjectileEntry* e = entry();
    shape = kinetic() ? phys::shapeForCase(e && e->shape.present ? &e->shape : nullptr, c.penetrator)
                      : phys::ProjectileShape{};
    publishedModel = (e && e->hasPublished && !custom)
                         ? phys::rhaPenetration(c.penetrator, kinetic() ? e->publishedVelocity : 0.0)
                         : 0.0;
    valid = !out.result.history.empty();
    mpmStale = true;
    evalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    dirty = false;
    if (playing || !std::isfinite(fraction) || valid == false)
        t = animStart();
    else
        t = animStart() + std::clamp(fraction, 0.0, 1.0) * (animEnd() - animStart());

    plotDepth.clear();
    plotVelocity.clear();
    plotLength.clear();
    if (!valid) return;
    const auto& h = out.result.history;
    plotT0 = h.front().time;
    plotT1 = std::max(h.back().time, plotT0 + 1e-9);
    for (int i = 0; i < kPlotSamples; ++i) {
        const double ti = plotT0 + (plotT1 - plotT0) * i / (kPlotSamples - 1);
        const phys::PenetrationSample s = sampleAt(h, ti);
        plotDepth.push_back(static_cast<float>(s.depth * 1e3));
        plotVelocity.push_back(static_cast<float>(s.velocity));
        plotLength.push_back(static_cast<float>(s.length * 1e3));
    }
}

void ArmourUi::State::setupWindow(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ui::beginWindow("Armour setup", nullptr, kTiled);
    if (!error.empty()) {
        ImGui::TextColored(ui::palette().bad, "%s", error.c_str());
        ui::endWindow();
        return;
    }

    if (ui::button("Cases...")) {
        listCases();
        ImGui::OpenPopup("cases");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save this setup to a file, or load one saved earlier.");
    if (ImGui::BeginPopup("cases")) {
        casesPopup();
        ImGui::EndPopup();
    }

    // Projectile.
    ImGui::SeparatorText("Projectile");
    std::string preview = "Custom";
    if (const ProjectileEntry* e = entry()) preview = custom ? "Custom (from " + e->name + ")" : e->name;
    ImGui::SetNextItemWidth(-1);
    if (ui::beginCombo("##projectile", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int k = 0; k < static_cast<int>(phys::PenetratorKind::Count); ++k) {
            ImGui::SeparatorText(phys::penetratorKindLabel(static_cast<phys::PenetratorKind>(k)));
            for (size_t i = 0; i < db.projectiles().size(); ++i) {
                const ProjectileEntry& e = db.projectiles()[i];
                if (static_cast<int>(e.penetrator.kind) != k) continue;
                char label[160];
                std::snprintf(label, sizeof(label), "%s  (%s, %d)##%zu", e.name.c_str(), e.country.c_str(), e.year, i);
                if (ui::comboItem(label, !custom && projectile == static_cast<int>(i)))
                    selectProjectile(static_cast<int>(i));
            }
        }
        ui::endCombo();
    }
    phys::Penetrator& p = c.penetrator;
    if (kinetic()) {
        float v = static_cast<float>(c.velocity);
        ImGui::SetNextItemWidth(-std::floor(90 * ui::scale()));
        if (ImGui::SliderFloat("##velocity", &v, 100.0f, 2500.0f, "%.0f m/s")) {
            c.velocity = v;
            dirty = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(entry() == nullptr);
        if (ui::button("Muzzle", {-1, 0}) && entry()) {
            c.velocity = entry()->velocity;
            dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Impact velocity");
    } else {
        ImGui::TextDisabled("Jet velocity is set by the charge (tip %.0f m/s).", phys::jetParameters(p).tipVelocity);
    }

    if (ImGui::TreeNode("Projectile parameters")) {
        bool edited = false;
        int kind = static_cast<int>(p.kind);
        if (ui::combo("Kind", &kind, "Long rod (APFSDS)\0Compact AP (shot or core)\0Shaped charge (HEAT)\0")) {
            p = phys::Penetrator{};
            p.kind = static_cast<phys::PenetratorKind>(kind);
            if (p.kind == phys::PenetratorKind::LongRod) {
                rodMaterial = RodMaterial::Tungsten;
                p.diameter = 0.025;
                p.length = 0.6;
                c.velocity = std::max(c.velocity, 1500.0);
            } else if (p.kind == phys::PenetratorKind::CompactAp) {
                rodMaterial = RodMaterial::Steel;
                p.diameter = 0.075;
                p.length = 0.3;
                p.mass = 6.8;
                c.velocity = std::min(c.velocity, 800.0);
            } else {
                p.coneDiameter = 0.08;
                p.standoff = 2.0;
                p.jetQuality = 0.5;
            }
            p.density = rodMaterialDensity(rodMaterial);
            edited = true;
        }
        auto dragMm = [&](const char* label, double& metres, float lo, float hi) {
            float v = static_cast<float>(metres * 1e3);
            if (ImGui::DragFloat(label, &v, 0.5f, lo, hi, "%.1f mm", ImGuiSliderFlags_AlwaysClamp)) {
                metres = v * 1e-3;
                edited = true;
            }
        };
        if (kinetic()) {
            int material = static_cast<int>(rodMaterial);
            if (ui::combo("Material", &material, "Tungsten alloy\0Depleted uranium\0Steel\0")) {
                rodMaterial = static_cast<RodMaterial>(material);
                p.density = rodMaterialDensity(rodMaterial);
                p.uranium = rodMaterial == RodMaterial::Uranium;
                edited = true;
            }
            dragMm("Diameter", p.diameter, 2.0f, 200.0f);
            dragMm("Length", p.length, 5.0f, 2000.0f);
            if (p.kind == phys::PenetratorKind::CompactAp) {
                float m = static_cast<float>(p.mass);
                if (ImGui::DragFloat("Mass", &m, 0.05f, 0.005f, 100.0f, "%.3f kg", ImGuiSliderFlags_AlwaysClamp)) {
                    p.mass = m;
                    edited = true;
                }
            }
            ImGui::TextDisabled("Mass %.2f kg, L/D %.1f, density %.0f kg/m3", phys::penetratorMass(p),
                                p.length / std::max(p.diameter, 1e-6), p.density);
        } else {
            dragMm("Cone diameter", p.coneDiameter, 10.0f, 500.0f);
            float so = static_cast<float>(p.standoff);
            if (ImGui::SliderFloat("Standoff", &so, 0.0f, 20.0f, "%.1f CD")) {
                p.standoff = so;
                edited = true;
            }
            float q = static_cast<float>(p.jetQuality);
            if (ImGui::SliderFloat("Jet quality q", &q, -0.8f, 1.5f, "%.2f")) {
                p.jetQuality = q;
                edited = true;
            }
            ImGui::TextDisabled("q sets tip speed, breakup time and particle drift.");
        }
        if (edited) {
            custom = true;
            dirty = true;
            fit = true;
        }
        if (custom && entry() && ui::button("Reset to catalogue values")) selectProjectile(projectile);
        ImGui::TreePop();
    }

    // Target.
    ImGui::SeparatorText("Target");
    const char* presetName = preset >= 0 ? db.presets()[static_cast<size_t>(preset)].name.c_str() : "Edited stack";
    ImGui::SetNextItemWidth(-1);
    if (ui::beginCombo("##preset", presetName, ImGuiComboFlags_HeightLarge)) {
        for (size_t i = 0; i < db.presets().size(); ++i)
            if (ui::comboItem(db.presets()[i].name.c_str(), preset == static_cast<int>(i)))
                applyPreset(static_cast<int>(i));
        ui::endCombo();
    }
    float ob = static_cast<float>(c.obliquityDeg);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##obliquity", &ob, -static_cast<float>(phys::kMaxObliquityDeg),
                           static_cast<float>(phys::kMaxObliquityDeg), "obliquity %.0f deg")) {
        c.obliquityDeg = ob;
        preset = -1;
        dirty = true;
    }

    int moveUp = -1, remove = -1;
    if (ImGui::BeginTable("layers", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, std::floor(14 * ui::scale()));
        ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Thick", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Gap", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 3 * ImGui::GetFrameHeight() + 4);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < c.layers.size(); ++i) {
            PresetLayer& l = c.layers[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            const bool validMaterial = l.material >= 0 && l.material < static_cast<int>(db.materials().size());
            const char* name = validMaterial ? db.materials()[static_cast<size_t>(l.material)].name.c_str() : "?";
            if (ui::beginCombo("##m", name, ImGuiComboFlags_HeightLarge)) {
                for (int g = 0; g < static_cast<int>(MaterialGroup::Count); ++g) {
                    ImGui::SeparatorText(materialGroupLabel(static_cast<MaterialGroup>(g)));
                    for (size_t m = 0; m < db.materials().size(); ++m) {
                        const ArmourMaterialEntry& me = db.materials()[m];
                        if (static_cast<int>(me.group) != g) continue;
                        ImGui::PushID(static_cast<int>(m));
                        const ImVec2 sw = ImGui::GetCursorScreenPos();
                        const float sc = ui::scale(), swatch = std::floor(10 * sc);
                        const float sy = std::floor(sw.y + 0.5f * (ImGui::GetTextLineHeight() - swatch));
                        ImGui::GetWindowDrawList()->AddRectFilled({sw.x, sy}, {sw.x + swatch, sy + swatch}, colour(me.color));
                        ImGui::Indent(std::floor(16 * sc));
                        if (ui::comboItem(me.name.c_str(), l.material == static_cast<int>(m))) {
                            l.material = static_cast<int>(m);
                            preset = -1;
                            dirty = true;
                        }
                        ImGui::Unindent(std::floor(16 * ui::scale()));
                        ImGui::PopID();
                    }
                }
                ui::endCombo();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float th = static_cast<float>(l.thickness * 1e3);
            if (ImGui::DragFloat("##t", &th, 0.5f, 0.1f, 2000.0f, "%.0f mm", ImGuiSliderFlags_AlwaysClamp)) {
                l.thickness = th * 1e-3;
                preset = -1;
                dirty = true;
            }
            ImGui::TableNextColumn();
            if (i > 0) {
                ImGui::SetNextItemWidth(-1);
                float gap = static_cast<float>(l.gap * 1e3);
                if (ImGui::DragFloat("##g", &gap, 1.0f, 0.0f, 5000.0f, "%.0f mm", ImGuiSliderFlags_AlwaysClamp)) {
                    l.gap = gap * 1e-3;
                    preset = -1;
                    dirty = true;
                }
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            // Separate tiles along the face (ERA): edited for the layer and the layers touching it.
            const bool tiled = isBlockLayer(l);
            if (tiled) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ui::button("T", {ImGui::GetFrameHeight(), ImGui::GetFrameHeight()})) ImGui::OpenPopup("tiles");
            if (tiled) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(tiled ? "Separate tiles: %.0f mm wide, %.0f mm apart" : "One full plate (click to cut it into tiles)",
                                  l.blockWidth * 1e3, l.blockGap * 1e3);
            if (ImGui::BeginPopup("tiles")) {
                tilesPopup(static_cast<int>(i));
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = static_cast<int>(i);
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(c.layers.size() <= 1);
            if (ui::button("x", {ImGui::GetFrameHeight(), ImGui::GetFrameHeight()})) remove = static_cast<int>(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (moveUp > 0) {
        std::swap(c.layers[static_cast<size_t>(moveUp)], c.layers[static_cast<size_t>(moveUp - 1)]);
        std::swap(c.layers[static_cast<size_t>(moveUp)].gap, c.layers[static_cast<size_t>(moveUp - 1)].gap);
        preset = -1;
        dirty = true;
    }
    if (remove >= 0) {
        c.layers.erase(c.layers.begin() + remove);
        if (!c.layers.empty()) c.layers.front().gap = 0;
        preset = -1;
        dirty = true;
        fit = true;
    }
    ImGui::BeginDisabled(c.layers.size() >= static_cast<size_t>(kMaxArmourLayers));
    if (ui::button("Add layer")) {
        PresetLayer l = c.layers.empty() ? PresetLayer{std::max(db.findMaterial("rha"), 0), 0.05, 0.0} : c.layers.back();
        if (!c.layers.empty()) l.gap = 0;
        c.layers.push_back(l);
        preset = -1;
        dirty = true;
        fit = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("up to %d layers; gap is in front of the layer", kMaxArmourLayers);
    ui::endWindow();
}

// Cuts a layer, and the layers touching it (no gap between: the plates and explosive of one
// ERA tile), into tiles along the face, or makes them full plates again.
void ArmourUi::State::tilesPopup(int layer) {
    int first = layer, last = layer;
    while (first > 0 && c.layers[static_cast<size_t>(first)].gap <= 1e-9) --first;
    while (last + 1 < static_cast<int>(c.layers.size()) && c.layers[static_cast<size_t>(last + 1)].gap <= 1e-9) ++last;
    PresetLayer& l = c.layers[static_cast<size_t>(layer)];
    bool tiled = isBlockLayer(l);
    float width = static_cast<float>((tiled ? l.blockWidth : 0.25) * 1e3);
    float gap = static_cast<float>(l.blockGap * 1e3), offset = static_cast<float>(l.offset * 1e3);
    int blocks = l.blockCount;
    bool changed = false;
    if (first == last)
        ImGui::Text("Layer %d", layer + 1);
    else
        ImGui::Text("Layers %d-%d (touching)", first + 1, last + 1);
    if (ImGui::Checkbox("Separate tiles", &tiled)) {
        changed = true;
        if (tiled && gap <= 0) gap = 10;
    }
    ImGui::BeginDisabled(!tiled);
    ImGui::SetNextItemWidth(std::floor(160 * ui::scale()));
    changed |= ImGui::DragFloat("width", &width, 1.0f, 10.0f, 2000.0f, "%.0f mm", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(std::floor(160 * ui::scale()));
    changed |= ImGui::DragFloat("gap between", &gap, 0.5f, 0.0f, 500.0f, "%.0f mm", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(std::floor(160 * ui::scale()));
    changed |= ImGui::DragInt("blocks", &blocks, 0.1f, 0, 12, blocks == 0 ? "endless row" : "%d", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many separate blocks the layer holds, centred on the offset. Each is a\n"
                          "free piece: drag them together, and they can be moved off the shot line.");
    ImGui::SetNextItemWidth(std::floor(160 * ui::scale()));
    changed |= ImGui::DragFloat("offset", &offset, 1.0f, -1000.0f, 1000.0f, "%.0f mm", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Where the tile centres sit along the face, from the shot line. Or drag the tiles\n"
                          "in the cross-section.");
    ImGui::EndDisabled();
    ImGui::TextDisabled(tiled && !layerCoversShotLine(l) ? "the shot passes between two tiles" : " ");
    if (!changed) return;
    for (int k = first; k <= last; ++k) {
        PresetLayer& e = c.layers[static_cast<size_t>(k)];
        e.blockWidth = tiled ? std::max(width, 10.0f) * 1e-3 : 0.0;
        e.blockGap = tiled ? gap * 1e-3 : 0.0;
        e.offset = tiled ? offset * 1e-3 : 0.0;
        e.blockCount = tiled ? blocks : 0;
    }
    preset = -1;
    dirty = true;
}

// ---- Saved cases, CSV export and witness-sheet readouts ----

namespace {

constexpr int kMaxReadbackParticles = 3000000;  // witness sheets are read back through the host
constexpr uintmax_t kMaxCaseFileBytes = 1u << 20;

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

bool writeFile(const std::filesystem::path& path, const std::string& text, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    f.flush();
    if (!f) {
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

}  // namespace

void ArmourUi::State::listCases() {
    caseFiles.clear();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root / "cases", ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") caseFiles.push_back(entry.path().stem().string());
    std::sort(caseFiles.begin(), caseFiles.end());
}

void ArmourUi::State::saveCase() {
    SavedCase saved;
    saved.name = caseName;
    saved.projectile = projectile;
    saved.custom = custom;
    saved.rodMaterial = rodMaterial;
    saved.c = c;
    const std::string stem = safeCaseName(caseName);
    if (saved.name.empty()) saved.name = stem;
    const std::filesystem::path path = root / "cases" / (stem + ".json");
    const bool replaced = std::filesystem::exists(path);
    std::string problem;
    if (!writeFile(path, caseToJson(db, saved), problem)) {
        fileMessage = "Not saved: " + problem;
        logx::warn("armour: %s", fileMessage.c_str());
        return;
    }
    fileMessage = std::string(replaced ? "Replaced cases/" : "Saved cases/") + stem + ".json";
    listCases();
}

void ArmourUi::State::applyCase(const SavedCase& saved) {
    projectile = saved.projectile;
    custom = saved.custom;
    rodMaterial = saved.rodMaterial;
    c = saved.c;
    preset = -1;
    if (warhead()) deformation = true;
    dirty = true;
    fit = true;
}

void ArmourUi::State::loadCase(const std::string& stem) {
    const std::filesystem::path path = root / "cases" / (stem + ".json");
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxCaseFileBytes) {
        fileMessage = "Could not read cases/" + stem + ".json";
        return;
    }
    std::ifstream f(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    SavedCase saved;
    std::string problem;
    if (!caseFromJson(db, text, saved, problem)) {
        fileMessage = "cases/" + stem + ".json: " + problem;
        logx::warn("armour: %s", fileMessage.c_str());
        return;
    }
    applyCase(saved);
    std::snprintf(caseName, sizeof(caseName), "%s", stem.c_str());
    fileMessage = "Loaded cases/" + stem + ".json";
    ImGui::CloseCurrentPopup();
}

void ArmourUi::State::casesPopup() {
    ImGui::TextUnformatted("Save this setup as");
    ImGui::SetNextItemWidth(std::floor(220 * ui::scale()));
    ImGui::InputTextWithHint("##casename", "name", caseName, sizeof(caseName));
    ImGui::SameLine();
    ImGui::BeginDisabled(caseName[0] == '\0');
    if (ui::button("Save")) saveCase();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Saved cases");
    if (caseFiles.empty()) ImGui::TextDisabled("None yet. Files go in cases/ next to the program.");
    ImGui::BeginChild("##caselist", {std::floor(320 * ui::scale()), std::floor(160 * ui::scale())}, 0);
    std::string load;
    for (const std::string& stem : caseFiles)
        if (ImGui::Selectable(stem.c_str())) load = stem;
    ImGui::EndChild();
    if (!load.empty()) loadCase(load);
    if (!fileMessage.empty()) ImGui::TextDisabled("%s", fileMessage.c_str());
}

// Writes the engineering model's histories, and the deformation simulation's when it has run, as
// CSV in exports/ (a pre-rendered run has a reading for every frame, a live one for as far as it
// has played).
void ArmourUi::State::exportCsv() {
    if (!valid) {
        fileMessage = "Nothing to export yet.";
        return;
    }
    const std::string stem = safeCaseName((entry() ? entry()->id : std::string("custom")) + "_" +
                                          (preset >= 0 ? db.presets()[static_cast<size_t>(preset)].id : std::string("edited"))) +
                             "_" + timestamp();
    const std::filesystem::path dir = root / "exports";
    std::string problem;
    if (!writeFile(dir / (stem + ".csv"), historyCsv(out), problem)) {
        fileMessage = "Not exported: " + problem;
        logx::warn("armour: %s", fileMessage.c_str());
        return;
    }
    fileMessage = "Wrote exports/" + stem + ".csv";
    if (!deformation) return;
    const std::vector<MpmMeasure> rows = snaps.complete() ? snaps.measures() : mpmLog;
    if (rows.empty()) return;
    if (!writeFile(dir / (stem + "_deformation.csv"), mpmHistoryCsv(rows), problem)) {
        fileMessage += "; the deformation readouts were not written: " + problem;
        return;
    }
    fileMessage += " and exports/" + stem + "_deformation.csv";
}

// Reads the witness sheets' material back from the GPU at the time shown: from the pre-rendered
// frame there, or from the live simulation.
void ArmourUi::State::readWitness() {
    witness.clear();
    witnessRead = true;
    witnessTime = measure.time;
    if (!solver.loaded()) return;
    const MpmScene& sc = solver.scene();
    const std::vector<MpmParticleState> state = snaps.complete() ? snaps.readFrame(t, sc) : solver.readParticles();
    for (int l = 1; l < static_cast<int>(c.layers.size()); ++l) {
        if (!isWitnessSheet(c, l)) continue;
        const WitnessReadout w = measureWitness(sc, state, l, measure);
        if (w.valid) witness.push_back(w);
    }
}

void ArmourUi::State::witnessReadouts() {
    int sheets = 0;
    for (int l = 1; l < static_cast<int>(c.layers.size()); ++l) sheets += isWitnessSheet(c, l) ? 1 : 0;
    if (sheets == 0 || !solver.loaded()) return;
    ImGui::SeparatorText("Witness sheets");
    const bool tooBig = solver.particleCount() > kMaxReadbackParticles;
    ImGui::BeginDisabled(prerendering || tooBig);
    if (ui::button(witnessRead ? "Read again" : "Read the sheets")) readWitness();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(tooBig ? "Too many particles to read back at this quality."
                                 : "Reads the sheets back from the GPU at the time shown.");
    if (!witnessRead) return;
    ImGui::SameLine();
    ImGui::TextDisabled("read at %.1f \xC2\xB5s", witnessTime * 1e6);
    if (std::abs(witnessTime - measure.time) > 0.5 * solver.scene().dt)
        ImGui::TextDisabled("The simulation is now at %.1f \xC2\xB5s.", measure.time * 1e6);
    for (const WitnessReadout& w : witness) {
        if (w.layer >= static_cast<int>(c.layers.size())) continue;  // the stack changed since the reading
        const PresetLayer& l = c.layers[static_cast<size_t>(w.layer)];
        ImGui::Text("Sheet %d: %s, %s thick", w.layer + 1, db.materials()[static_cast<size_t>(l.material)].name.c_str(),
                    mm(l.thickness, 1).c_str());
        ImGui::Indent();
        if (w.holeWidth > 0)
            ImGui::Text("Hole: %s wide", mm(w.holeWidth, 1).c_str());
        else
            ImGui::TextUnformatted("No hole through the sheet");
        ImGui::Text("Bulge: %s forward, fastest at %.0f m/s", mm(w.bulge, 1).c_str(), w.peakSpeed);
        if (w.residualVelocity > 0)
            ImGui::Text("Penetrator past it at %.0f m/s", w.residualVelocity);
        else
            ImGui::TextUnformatted("Penetrator not past it");
        ImGui::Unindent();
    }
    if (witness.empty()) ImGui::TextDisabled("No sheet material in the simulation.");
}

void ArmourUi::State::resultsWindow(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ui::beginWindow("Penetration results", nullptr, kTiled);
    if (!valid) {
        ImGui::TextDisabled("No result.");
        ui::endWindow();
        return;
    }
    const phys::PenetrationResult& r = out.result;
    const phys::Penetrator& p = c.penetrator;
    const ProjectileEntry* e = custom ? nullptr : entry();
    const double cosT = std::cos(theta());

    if (e) {
        ImGui::TextWrapped("%s", e->name.c_str());
        ImGui::TextDisabled("%s, %d, %s", e->country.c_str(), e->year, e->gun.c_str());
    } else {
        ImGui::TextUnformatted("Custom projectile");
    }
    ImGui::Text("%s", phys::penetratorKindLabel(p.kind));
    if (warhead()) {
        ImGui::Text("%.0f mm diameter, %.0f mm long, %.1f kg", p.diameter * 1e3, p.length * 1e3, phys::penetratorMass(p));
        ImGui::Text("Impact %.0f m/s at %.0f deg obliquity", c.velocity, c.obliquityDeg);
    } else if (kinetic()) {
        ImGui::Text("%s, %.0f x %.0f mm, %.2f kg", rodMaterialLabel(rodMaterial), p.diameter * 1e3, p.length * 1e3,
                    phys::penetratorMass(p));
        ImGui::Text("Impact %.0f m/s at %.0f deg obliquity", c.velocity, c.obliquityDeg);
    } else {
        const phys::JetParameters jp = phys::jetParameters(p);
        ImGui::Text("Cone %.0f mm, standoff %.1f CD, q %.2f", p.coneDiameter * 1e3, p.standoff, p.jetQuality);
        ImGui::Text("Jet tip %.0f m/s, %.0f deg obliquity", jp.tipVelocity, c.obliquityDeg);
    }
    if (e) ImGui::Text("Confidence: %s", confidenceLabel(e->confidence));

    ImGui::SeparatorText("Outcome");
    if (warhead()) {
        ImGui::TextWrapped("Detonation and plate response: see the deformation simulation");
        for (size_t i = 0; i < r.history.size(); ++i)
            ImGui::Text("Nose at plate %zu after %.1f \xC2\xB5s", i + 1, r.history[i].time * 1e6);
    } else if (r.perforated) {
        ImGui::TextColored(ui::palette().bad, "Stack perforated");
        if (kinetic()) {
            ImGui::Text("Residual velocity: %.0f m/s", r.residualVelocity);
            if (p.kind == phys::PenetratorKind::LongRod)
                ImGui::Text("Residual rod: %s (%.2f kg)", mm(r.residualLength).c_str(), r.residualMass);
            else
                ImGui::Text("Residual shot: %.2f kg", r.residualMass);
        } else {
            ImGui::Text("Residual jet: %s, tip %.0f m/s", mm(r.residualLength).c_str(), r.residualVelocity);
        }
        ImGui::Text("Through the last plate after %.1f \xC2\xB5s", r.exitTime * 1e6);
    } else {
        int stopLayer = -1;
        for (size_t i = 0; i < r.layers.size(); ++i)
            if (!r.layers[i].perforated) {
                stopLayer = static_cast<int>(i);
                break;
            }
        ImGui::TextColored(ui::palette().good, "Stopped in layer %d", stopLayer + 1);
        ImGui::Text("Depth: %s along the shot line (%s normal)", mm(r.depth).c_str(), mm(r.depth * cosT).c_str());
        if (p.kind == phys::PenetratorKind::LongRod)
            ImGui::Text("Remaining rod: %s", mm(r.residualLength).c_str());
    }
    fuzeReadouts();
    if (kinetic()) {
        if (out.ballisticLimit > 0)
            ImGui::Text("Ballistic limit: %.0f m/s", out.ballisticLimit);
        else if (out.ballisticLimit < 0)
            ImGui::Text("Ballistic limit: above 3000 m/s");
    }
    if (!warhead()) {
        ImGui::Text("RHA witness block behind: %s", mm(out.witnessDepth).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Depth along the shot line into a semi-infinite RHA block placed directly\n"
                              "behind the last plate (%s with no stack in front).", mm(out.witnessReference).c_str());
        ImGui::Text("Semi-infinite RHA (normal): %s", mm(out.rhaSemiInfinite).c_str());
    }
    if (e && e->hasPublished) {
        if (kinetic())
            ImGui::TextWrapped("Published: %s at %.0f m/s (model %s)", mm(e->publishedRha).c_str(), e->publishedVelocity,
                               mm(publishedModel).c_str());
        else
            ImGui::TextWrapped("Published: %s (jet fitted to it)", mm(e->publishedRha).c_str());
        if (!e->publishedConditions.empty()) ImGui::TextDisabled("%s", e->publishedConditions.c_str());
    }

    if (deformation) mpmReadouts();

    ImGui::SeparatorText("Stack");
    ImGui::Text("Line of sight %s, areal density %.0f kg/m2", mm(out.lineOfSight).c_str(), out.arealDensity);
    if (!warhead()) ImGui::Text("RHA equivalent (DoP): %s%s", out.rhaEquivalentIsBound ? "at least " : "", mm(out.rhaEquivalent).c_str());
    ImGui::Text("Efficiency: thickness %.2f, mass %.2f%s", out.thicknessEfficiency, out.massEfficiency,
                out.rhaEquivalentIsBound ? " (lower bounds)" : "");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Depth-of-penetration method: RHA witness depth without the stack minus with it,\n"
                          "divided by the stack's line-of-sight thickness (thickness) or by its areal\n"
                          "density over RHA's (mass). 1.0 = as good as RHA.");
    if (ImGui::BeginTable("layerResults", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Material");
        ImGui::TableSetupColumn("LOS mm");
        ImGui::TableSetupColumn("Depth mm");
        ImGui::TableSetupColumn("Through");
        ImGui::TableSetupColumn("Exit m/s");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.layers.size() && i < c.layers.size(); ++i) {
            const phys::LayerResult& l = r.layers[i];
            const int m = c.layers[i].material;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m >= 0 && m < static_cast<int>(db.materials().size())
                                       ? db.materials()[static_cast<size_t>(m)].id.c_str()
                                       : "?");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", (l.back - l.front) * 1e3);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", l.depth * 1e3);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(l.perforated ? "yes" : "no");
            ImGui::TableNextColumn();
            if (l.perforated && kinetic())
                ImGui::Text("%.0f", l.exitVelocity);
            else
                ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Notes and sources")) {
        if (e) {
            if (!e->notes.empty()) ImGui::TextWrapped("%s", e->notes.c_str());
            for (const std::string& src : e->sources) ImGui::BulletText("%s", src.c_str());
        }
        std::vector<int> seen;
        for (const PresetLayer& l : c.layers) {
            if (l.material < 0 || l.material >= static_cast<int>(db.materials().size())) continue;
            if (std::find(seen.begin(), seen.end(), l.material) != seen.end()) continue;
            seen.push_back(l.material);
            const ArmourMaterialEntry& me = db.materials()[static_cast<size_t>(l.material)];
            ImGui::Separator();
            ImGui::TextWrapped("%s (%s confidence)", me.name.c_str(), confidenceLabel(me.confidence));
            if (!me.notes.empty()) ImGui::TextWrapped("%s", me.notes.c_str());
            for (const std::string& src : me.sources) ImGui::BulletText("%s", src.c_str());
        }
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Engineering estimates for generic plates, for education only. Evaluated in %.1f ms.", evalMs);
    ImGui::PopStyleColor();
    ui::endWindow();
}

void ArmourUi::State::historyWindow(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ui::beginWindow("Time histories", nullptr, kTiled);
    if (valid) {
        const float footer = ImGui::GetFrameHeightWithSpacing();
        const float ph = std::max(50.0f, (ImGui::GetContentRegionAvail().y - footer - 2 * ImGui::GetStyle().ItemSpacing.y) / 3);
        const bool jet = !kinetic();
        plot(jet ? "Crater depth" : "Nose depth", "mm", plotDepth, plotT0, plotT1, t, ph);
        plot(jet ? "Jet velocity at the crater" : "Velocity", "m/s", plotVelocity, plotT0, plotT1, t, ph);
        plot(jet ? "Jet length still to arrive" : "Penetrator length", "mm", plotLength, plotT0, plotT1, t, ph);
        if (ui::button("Export CSV")) exportCsv();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes the time histories to exports/ next to the program\n(and the deformation readouts when that view has run).");
        if (!fileMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", fileMessage.c_str());
        }
    }
    ui::endWindow();
}

void ArmourUi::State::sectionWindow(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ui::beginWindow("Cross-section", nullptr, kTiled | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!valid) {
        ImGui::TextDisabled("No result.");
        ui::endWindow();
        return;
    }
    if (ImGui::RadioButton("Engineering model", !deformation)) {
        deformation = false;
        fit = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Deformation (MPM)", deformation)) {
        deformation = true;
        fit = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("2D plane-strain material point simulation: the projectile and the plates deform,\n"
                          "erode and break up. A slab of unit depth stands in for the round rod, so depths\n"
                          "are indicative; the engineering model's figures stay in the results.");
    if (deformation) {
        static const char* colours[] = {mpmColourLabel(MpmColour::Material),     mpmColourLabel(MpmColour::Stress),
                                        mpmColourLabel(MpmColour::PlasticStrain), mpmColourLabel(MpmColour::Temperature),
                                        mpmColourLabel(MpmColour::Speed),         mpmColourLabel(MpmColour::Failure)};
        static const char* qualities[] = {mpmQualityLabel(MpmQuality::Low),   mpmQualityLabel(MpmQuality::Medium),
                                          mpmQualityLabel(MpmQuality::High),  mpmQualityLabel(MpmQuality::Ultra),
                                          mpmQualityLabel(MpmQuality::Insane), mpmQualityLabel(MpmQuality::Ludicrous)};
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(140 * ui::scale()));
        ui::combo("Colour", &mpmColour, colours, IM_ARRAYSIZE(colours));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(90 * ui::scale()));
        if (ui::combo("Quality", &mpmQuality, qualities, IM_ARRAYSIZE(qualities))) mpmStale = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cells across the rod's diameter: Low 8, Medium 14, High 22.\n"
                              "Ultra, Insane and Ludicrous (4, 12 and 100 million particles) aim for 128, 256\n"
                              "and 512 cells, but the grid (3072, 4096 and 6144 nodes wide) and the particle\n"
                              "budget set the real cell; the cells are filled with extra particles for smoother\n"
                              "material and finer debris. They run far below real time: Ultra a few ms a step,\n"
                              "Insane about 15 ms, Ludicrous about 0.1 s or more (a long shot is hours), so use\n"
                              "them with slow motion or Pre-render. Ludicrous needs about 12 GB; with System RAM\n"
                              "on, what does not fit the card waits in the computer's memory and is streamed in\n"
                              "every step. Pre-renders keep every frame in video memory when they can, else one\n"
                              "particle in a few, with the rest of the frames in system memory.\n"
                              "A level that cannot be fitted gives way to the next one down.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Soft mounts", &mpmSoftMounts)) mpmStale = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The plates are held at their edges. Rigid (the default): the edge strip is clamped\n"
                              "and cannot move. Soft: it is tied to where it started by a damped spring, so the\n"
                              "mounting gives under the blow and rings down, as a real frame or bolted joint does.");
        ImGui::SameLine();
        if (ImGui::Checkbox("System RAM", &mpmUseSystemRam)) mpmStale = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When the particles do not fit the free video memory, keep the rest in the computer's\n"
                              "own memory and stream it through the card every step, so a finer quality still runs\n"
                              "(slower: about 0.1 s more a step for every GB streamed). The processor supplies the\n"
                              "memory, not the arithmetic. Pre-rendered frames that do not fit wait there too.\n"
                              "Off: a quality that does not fit the card gives way to a lower one.");
        if (solver.loaded()) {
            // On its own line: beside the other controls it ran off the end of a narrow pane.
            const MpmStorePlan& plan = solver.storePlan();
            if (plan.spilledChunks > 0)
                ImGui::TextDisabled("%d particles, cell %.2f mm, %.1f GB in system memory", solver.particleCount(),
                                    solver.scene().dx * 1e3, static_cast<double>(plan.hostBytes) / 1073741824.0);
            else
                ImGui::TextDisabled("%d particles, cell %.2f mm", solver.particleCount(), solver.scene().dx * 1e3);
        }
    }
    const float controls = ImGui::GetFrameHeightWithSpacing() * 2 + 4;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 100.0f), std::max(avail.y - controls, 100.0f)};
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    if (fit) {
        const phys::Penetrator& p = c.penetrator;
        double xmin, xmax;
        const double back = stackBack();
        if (p.kind == phys::PenetratorKind::LongRod) {
            xmin = -1.35 * p.length;
        } else if (p.kind == phys::PenetratorKind::CompactAp) {
            xmin = -1.5 * std::max(p.length, 2 * p.diameter) - 1.1 * p.length;
        } else {
            xmin = -(p.standoff + 1.6) * p.coneDiameter;
        }
        // Far enough to see the residual penetrator (or jet) leave the stack.
        double reach = back;
        if (out.result.perforated)
            reach = kinetic() ? back + out.result.residualVelocity * (out.result.endTime - out.result.exitTime)
                              : back + 2 * p.coneDiameter;
        xmax = std::max(reach, back + 0.1 * (back - xmin)) + (kinetic() ? 3 * p.diameter : 0.5 * p.coneDiameter);
        double halfY = std::max(kinetic() ? 4 * p.diameter : p.coneDiameter, 0.02);
        if (deformation && solver.loaded()) {
            // The simulation domain, which holds everything that can move.
            const MpmScene& sc = solver.scene();
            xmin = sc.originX;
            xmax = sc.originX + (sc.nx - 1) * sc.dx;
            halfY = std::max(-sc.originY, sc.originY + (sc.ny - 1) * sc.dx);
        }
        scale = 0.92 * std::min(size.x / (xmax - xmin), size.y / (2 * halfY));
        cx = 0.5 * (xmin + xmax);
        cy = 0;
        fit = false;
    }
    if (hovered && io.MouseWheel != 0) {
        const double mx = cx + (io.MousePos.x - origin.x - 0.5 * size.x) / scale;
        const double my = cy - (io.MousePos.y - origin.y - 0.5 * size.y) / scale;
        const double factor = std::pow(1.2, io.MouseWheel);
        scale = std::clamp(scale * factor, 10.0, 2e6);
        cx = mx - (io.MousePos.x - origin.x - 0.5 * size.x) / scale;
        cy = my + (io.MousePos.y - origin.y - 0.5 * size.y) / scale;
    }
    // A press on a plate drags it (and the touching layers cut the same way); anywhere else pans.
    const double my = mirrorY();
    auto plateUnder = [&](ImVec2 at) {
        const double wx = cx + (at.x - origin.x - 0.5 * size.x) / scale;
        const double wy = my * (cy - (at.y - origin.y - 0.5 * size.y) / scale);
        return plateAt(wx, wy, 8.0 / scale);
    };
    if (ImGui::IsItemActivated()) dragLayer = plateUnder(io.MouseClickedPos[ImGuiMouseButton_Left]);
    if (!ImGui::IsItemActive()) {
        dragLayer = -1;
        draggingPlate = false;
    }
    if (hovered && !ImGui::IsItemActive() && plateUnder(io.MousePos) >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (dragLayer >= 0) {
            draggingPlate = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            dragPlate(io.MouseDelta.x / scale, -my * io.MouseDelta.y / scale);
        } else {
            cx -= io.MouseDelta.x / scale;
            cy += io.MouseDelta.y / scale;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fit = true;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    if (deformation)
        drawMpm(draw, origin, size);
    else
        drawSection(draw, origin, size);
    draw->PopClipRect();

    // Playback controls.
    const double t0 = animStart(), t1 = animEnd();
    if (ui::button(playing ? "Pause" : "Play", {std::floor(60 * ui::scale()), 0})) {
        if (!playing && t >= t1) t = t0;
        playing = !playing;
    }
    ImGui::SameLine();
    if (ui::button("Replay")) {
        t = t0;
        playing = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::floor(110 * ui::scale()));
    ImGui::SliderFloat("##lasts", &playSeconds, 0.2f, 300.0f, "lasts %.1f s", ImGuiSliderFlags_Logarithmic);
    playSeconds = std::clamp(playSeconds, 0.2f, 300.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How long the model's event takes to play: slow motion x%.0f.%s", playSeconds / (baseEnd() - t0),
                          deformation && !snaps.complete()
                              ? "\nThe simulation has to keep up; pre-render it to play at any speed."
                              : "");
    ImGui::SameLine();
    if (deformation) {
        // Pre-render: simulate the whole run first, then play and scrub it freely.
        if (prerendering) {
            if (ui::button("Cancel")) {
                prerendering = false;
                snaps.clear();
            }
            ImGui::SameLine();
            char label[32];
            std::snprintf(label, sizeof(label), "%.0f%%", 100 * snaps.progress());
            ImGui::ProgressBar(static_cast<float>(snaps.progress()), {std::floor(90 * ui::scale()), 0}, label);
        } else if (snaps.complete()) {
            if (ui::button("Live")) snaps.clear();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Playing %d pre-rendered frames%s%s. Drop them and simulate as it plays.", snaps.frames(),
                                  snaps.stride() > 1 ? " of one particle in " : "",
                                  snaps.stride() > 1 ? std::to_string(snaps.stride()).c_str() : "");
        } else {
            if (ui::button("Pre-render")) {
                prerendering = true;
                playing = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Simulate the whole run first, then play it at any speed and scrub it freely.");
        }
        ImGui::SameLine();
    }
    // Where the animation (and the simulation) stops: the model's end or later.
    const double base = baseEnd();
    float endUs = static_cast<float>(t1 * 1e6);
    ImGui::SetNextItemWidth(std::floor(150 * ui::scale()));
    if (ImGui::SliderFloat("##end", &endUs, static_cast<float>(base * 1e6),
                           static_cast<float>((t0 + kMaxEndScale * (base - t0)) * 1e6), "ends at %.1f \xC2\xB5s")) {
        endScale = std::clamp((endUs * 1e-6 - t0) / (base - t0), 1.0, kMaxEndScale);
        t = std::min(t, animEnd());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run the animation on past the model's end, up to %.0f times as long.", kMaxEndScale);
    ImGui::SameLine();
    if (ui::button("Fit view")) fit = true;
    float tf = static_cast<float>(t * 1e6);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time", &tf, static_cast<float>(t0 * 1e6), static_cast<float>(t1 * 1e6),
                           kinetic() ? "%.1f \xC2\xB5s after first contact" : "%.1f \xC2\xB5s after jet formation")) {
        t = tf * 1e-6;
        playing = false;
    }
    ui::endWindow();
}

// A plate's front face crosses the shot line at layers[i].front, its back face thickness / cos
// further on. Along the face, sigma is measured from where the shot line crosses the mid-plane
// (src/data/armour_blocks.h, src/sim/mpm_scene.cpp): the front face's crossing is at sigma
// +thickness tan / 2, the back face's at -thickness tan / 2.
std::vector<ArmourUi::State::PlatePiece> ArmourUi::State::platePieces(double half) const {
    std::vector<PlatePiece> pieces;
    const double theta = this->theta(), sinT = std::sin(theta), cosT = std::cos(theta), tanT = std::tan(theta);
    // The point g along a face that crosses the shot line at s.
    auto at = [&](double s, double g) { return std::array<double, 2>{s - g * sinT, g * cosT}; };
    for (size_t i = 0; i < out.result.layers.size() && i < c.layers.size(); ++i) {
        const double thick = std::max(0.0, c.layers[i].thickness);
        const double front = out.result.layers[i].front, back = front + thick / cosT, shift = 0.5 * thick * tanT;
        for (const LayerSpan& sp : layerTiles(c.layers[i], -half, half))
            pieces.push_back({static_cast<int>(i),
                              {at(front, sp.lo - shift), at(back, sp.lo + shift), at(back, sp.hi + shift), at(front, sp.hi - shift)}});
    }
    return pieces;
}

int ArmourUi::State::plateAt(double wx, double wy, double tolerance) const {
    const double theta = this->theta(), sinT = std::sin(theta), cosT = std::cos(theta), tanT = std::tan(theta);
    int best = -1;
    double bestMiss = tolerance;
    for (size_t i = 0; i < out.result.layers.size() && i < c.layers.size(); ++i) {
        const double thick = std::max(0.0, c.layers[i].thickness), front = out.result.layers[i].front;
        const double u = (wx - front) * cosT + wy * sinT;  // depth into the plate along its normal
        const double miss = std::max({0.0, -u, u - thick});
        if (miss > bestMiss) continue;
        const double sigma = -(wx - front) * sinT + wy * cosT + 0.5 * thick * tanT;
        if (layerTiles(c.layers[i], sigma - tolerance, sigma + tolerance).empty()) continue;  // between tiles
        best = static_cast<int>(i);
        bestMiss = miss;
        if (miss == 0) break;
    }
    return best;
}

// The mouse moved (dx, dy) in the simulation's frame: the part along the plate normal opens one
// gap and closes the other, the part along the face slides tiles. Moving the first plate moves
// the origin of the timeline; the camera follows so that everything else stays put on screen.
void ArmourUi::State::dragPlate(double dx, double dy) {
    if (dragLayer < 0 || dragLayer >= static_cast<int>(c.layers.size())) return;
    const double theta = this->theta(), sinT = std::sin(theta), cosT = std::cos(theta), tanT = std::tan(theta);
    const double du = dx * cosT + dy * sinT, ds = -dx * sinT + dy * cosT;
    const LayerGroup g = layerGroup(c.layers, dragLayer);
    const double before = g.first > 0 ? c.layers[static_cast<size_t>(g.first)].gap : 0;
    const double shift = dragLayers(c.layers, dragLayer, du, 0);
    const double moved = g.first > 0 ? c.layers[static_cast<size_t>(g.first)].gap - before : shift;
    // Tiles are placed from where the shot line crosses the plate, which slides along the face
    // as the plate moves along its normal: keep them moving straight along the normal.
    dragLayers(c.layers, dragLayer, 0, ds + moved * tanT);
    cx -= shift / cosT;
    if (moved != 0 || ds != 0) {
        preset = -1;
        dirty = true;
        keepView = true;
    }
}

void ArmourUi::State::drawSection(ImDrawList* draw, ImVec2 origin, ImVec2 size) {
    const phys::PenetrationResult& r = out.result;
    const phys::Penetrator& p = c.penetrator;
    const double theta = this->theta();
    const double sinT = std::sin(theta), cosT = std::cos(theta);
    const double my = mirrorY();
    auto toScreen = [&](double wx, double wy) {
        return ImVec2{static_cast<float>(origin.x + 0.5 * size.x + (wx - cx) * scale),
                      static_cast<float>(origin.y + 0.5 * size.y - (my * wy - cy) * scale)};
    };
    const ImVec2 end{origin.x + size.x, origin.y + size.y};
    draw->AddRectFilled(origin, end, colour(kBackground));
    const double viewHalf = 0.5 * std::hypot(size.x, size.y) / scale + std::abs(cy) + std::abs(cx) + 1.0;

    // Plates: bands between tilted faces, extended past the canvas; tile layers as separate tiles.
    for (const PlatePiece& piece : platePieces(viewHalf)) {
        const int m = c.layers[static_cast<size_t>(piece.layer)].material;
        const unsigned rgb = m >= 0 && m < static_cast<int>(db.materials().size())
                                 ? db.materials()[static_cast<size_t>(m)].color
                                 : 0x8b939cu;
        ImVec2 pts[4];
        for (int k = 0; k < 4; ++k) pts[k] = toScreen(piece.q[static_cast<size_t>(k)][0], piece.q[static_cast<size_t>(k)][1]);
        const float lift = piece.layer == dragLayer && draggingPlate ? 1.25f : 1.0f;
        draw->AddConvexPolyFilled(pts, 4, colour(rgb, 1.0f, 0.85f * lift));
        draw->AddLine(pts[0], pts[3], colour(rgb, 1.0f, 1.15f * lift), 1.0f);  // front face
        draw->AddLine(pts[1], pts[2], colour(rgb, 1.0f, 0.55f * lift), 1.0f);  // back face
        if (isBlockLayer(c.layers[static_cast<size_t>(piece.layer)])) {
            draw->AddLine(pts[0], pts[1], colour(rgb, 1.0f, 0.55f * lift), 1.0f);  // tile edges
            draw->AddLine(pts[3], pts[2], colour(rgb, 1.0f, 0.55f * lift), 1.0f);
        }
    }

    // Shot line and plate normal at the impact point.
    draw->AddLine(toScreen(cx - viewHalf, 0), toScreen(cx + viewHalf, 0), colour(0xffffffu, 0.08f), 1.0f);
    if (theta > 1e-3) {
        const double len = 0.12 * size.x / scale;
        draw->AddLine(toScreen(0, 0), toScreen(-len * cosT, -len * sinT), colour(0xffffffu, 0.35f), 1.0f);
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "normal, %.0f deg", c.obliquityDeg);
        const ImVec2 at = toScreen(-len * cosT, -len * sinT);
        draw->AddText({at.x - 40, at.y + 2}, colour(0xffffffu, 0.5f), lbl);
    }

    // Where the nose (kinetic) or the crater bottom (jet) is along the shot line.
    const auto& h = r.history;
    const bool jet = p.kind == phys::PenetratorKind::ShapedCharge;
    const phys::PenetrationSample now = sampleAt(h, t);
    const bool contact = t >= h.front().time;
    const bool ended = t >= h.back().time;
    const bool jetThrough = jet && r.perforated && t >= r.exitTime;
    double nose = now.depth, length = now.length;
    if (!contact) {
        nose = jet ? h.front().depth : h.front().depth + c.velocity * (t - h.front().time);
        length = h.front().length;
    } else if (jetThrough) {
        nose = stackBack() + 2 * p.coneDiameter;
    } else if (!jet && ended && r.perforated) {
        nose = h.back().depth + r.residualVelocity * (t - h.back().time);
        length = h.back().length;
    }

    // Crater: every plate is cut where the penetrator has passed (|y| <= crater radius, behind
    // `nose`). Samples are merged to about a pixel, and each piece is clipped to each plate.
    if (contact) {
        struct Cut {
            double depth, radius;
        };
        std::vector<Cut> cuts;
        for (const phys::PenetrationSample& s : h) {
            if (s.time > t) break;
            if (!cuts.empty() && s.depth - cuts.back().depth < 0.75 / scale &&
                std::abs(s.craterRadius - cuts.back().radius) < 0.5 / scale)
                continue;
            cuts.push_back({s.depth, s.craterRadius});
        }
        if (t < h.back().time && now.depth > cuts.back().depth) cuts.push_back({now.depth, now.craterRadius});
        // Gaps report no crater: carry the last radius forward (and the first one back).
        double last = 0;
        for (Cut& cut : cuts) last = cut.radius = cut.radius > 0 ? cut.radius : last;
        for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) last = it->radius = it->radius > 0 ? it->radius : last;
        const double tanT = std::tan(theta);
        const unsigned liner = jet ? kJetColour : rodColour(rodMaterial);
        auto cut = [&](double x0, double x1, double rc) {
            x1 = std::min(x1, nose);
            if (rc <= 0 || x1 <= x0) return;
            for (const phys::LayerResult& l : r.layers) {
                // Plate i: front <= x + y tan(theta) <= back.
                std::vector<std::array<double, 2>> poly = {{x0, -rc}, {x1, -rc}, {x1, rc}, {x0, rc}};
                clipHalfPlane(poly, -1, -tanT, -l.front);
                clipHalfPlane(poly, 1, tanT, l.back);
                if (poly.size() < 3) continue;
                std::vector<ImVec2> pts;
                for (const auto& v : poly) pts.push_back(toScreen(v[0], v[1]));
                draw->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), colour(kBackground));
                for (const double y : {rc, -rc}) {
                    const double a = std::max(x0, l.front - y * tanT), b = std::min(x1, l.back - y * tanT);
                    if (b > a) draw->AddLine(toScreen(a, y), toScreen(b, y), colour(liner, 0.6f), 1.0f);
                }
            }
        };
        if (!cuts.empty()) {
            cut(cuts.front().depth - cuts.front().radius * tanT - 1e-4, cuts.front().depth, cuts.front().radius);
            for (size_t i = 1; i < cuts.size(); ++i) cut(cuts[i - 1].depth, cuts[i].depth, cuts[i].radius);
            cut(cuts.back().depth, nose, cuts.back().radius);
        }
    }

    // Projectile.
    auto glow = [&](double x, double radius) {
        const ImVec2 c0 = toScreen(x, 0);
        const float rp = std::max(static_cast<float>(radius * scale), 3.0f);
        draw->AddCircleFilled(c0, rp * 1.8f, colour(0xff8a3au, 0.18f));
        draw->AddCircleFilled(c0, rp * 1.1f, colour(0xffc070u, 0.35f));
        draw->AddCircleFilled(c0, rp * 0.5f, colour(0xfff4d8u, 0.8f));
    };
    const bool penetrating = contact && !ended && now.layer >= 0 && now.layer < static_cast<int>(r.layers.size()) &&
                             now.rate > 0;
    if (!jet) {
        const double R = 0.5 * p.diameter;
        const unsigned rc = rodColour(rodMaterial);
        ShapeDrawFrame frame;
        frame.toScreen = toScreen;
        frame.noseX = nose;
        frame.firstFaceClip = r.layers.empty() ? frame.firstFaceClip : r.layers.front().front;
        frame.firstFaceSlope = std::tan(theta);
        frame.mirrorY = my;
        frame.ownColour = colour(rc);
        if (p.kind == phys::PenetratorKind::LongRod) {
            if (length > 0) {
                frame.erodedLength = length;
                drawProjectileShape(draw, shape, frame);
                if (mpmIsDart(p)) {
                    // Sabot grooves over the middle half of the body, when they are a few pixels apart.
                    const double pitch = 0.15 * p.diameter, tail = nose - length;
                    if (pitch * scale >= 3)
                        for (double x = tail + 0.25 * p.length; x < std::min(tail + 0.75 * p.length, nose); x += pitch)
                            draw->AddLine(toScreen(x, R), toScreen(x, -R), colour(0x000000u, 0.3f), 1.0f);
                }
            }
            if (penetrating) glow(nose, std::max(now.craterRadius, R));
        } else {
            // Compact shot: drawn whole (the models do not erode it).
            frame.erodedLength = shape.penetratorBack - shape.penetratorFront;
            drawProjectileShape(draw, shape, frame);
            if (penetrating) glow(nose, R);
            // Plugs pushed out of each perforated block of touching plates.
            for (size_t i = 0; i < r.layers.size() && i < c.layers.size(); ++i) {
                const bool lastOfBlock = i + 1 == r.layers.size() || c.layers[i + 1].gap > 1e-9;
                if (!lastOfBlock || !r.layers[i].perforated || t < r.layers[i].exitTime) continue;
                size_t first = i;
                while (first > 0 && c.layers[first].gap <= 1e-9) --first;
                const double thick = (r.layers[i].back - r.layers[first].front) * cosT;
                const double vPlug = 1.08 * std::max(r.layers[i].exitVelocity, 1.0);
                const double front = r.layers[i].back + vPlug * (t - r.layers[i].exitTime);
                if (i + 1 < r.layers.size() && front > r.layers[i + 1].front) continue;  // absorbed by the next plate
                const double half = 0.6 * p.diameter;
                const ImVec2 q[4] = {toScreen(front - thick / cosT + half * sinT, -half * cosT),
                                     toScreen(front + half * sinT, -half * cosT), toScreen(front - half * sinT, half * cosT),
                                     toScreen(front - thick / cosT - half * sinT, half * cosT)};
                const int m = c.layers[i].material;
                const unsigned prgb = m >= 0 && m < static_cast<int>(db.materials().size())
                                          ? db.materials()[static_cast<size_t>(m)].color
                                          : 0x8b939cu;
                draw->AddConvexPolyFilled(q, 4, colour(prgb, 1.0f, 1.1f));
            }
        }
    } else {
        // Charge outline (already detonated) and jet particles.
        const double cd = p.coneDiameter;
        const double baseX = -p.standoff * cd;
        draw->AddRect(toScreen(baseX - 1.5 * cd, 0.5 * cd), toScreen(baseX, -0.5 * cd), colour(0x9aa0a6u, 0.35f));
        draw->AddLine(toScreen(baseX - 1.1 * cd, 0), toScreen(baseX, 0.48 * cd), colour(kJetColour, 0.35f), 1.5f);
        draw->AddLine(toScreen(baseX - 1.1 * cd, 0), toScreen(baseX, -0.48 * cd), colour(kJetColour, 0.35f), 1.5f);
        const phys::JetParameters jp = phys::jetParameters(p);
        const double S = jp.virtualOrigin;
        const int n = static_cast<int>(r.jet.size());
        const double dv = n > 0 ? (jp.tipVelocity - jp.minVelocity) / n : 0;
        const double bottom = contact ? now.depth : h.front().depth;
        const bool through = jetThrough;
        const bool broken = t > jp.breakupTime;
        const double back = stackBack();
        // After breakup the particles drift apart; when neighbours are only a pixel or two apart,
        // draw every stride-th one (standing for the group) so they read as beads, not hatching.
        const double gapPx = dv * t * scale;
        const int stride = broken && gapPx > 0 ? std::clamp(static_cast<int>(std::ceil(5.0 / gapPx)), 1, 16) : 1;
        bool consuming = false;
        for (int k = 0; k < n; ++k) {
            const phys::JetElement& el = r.jet[static_cast<size_t>(k)];
            const double v = el.velocity;
            const double stretch = dv * std::min(t, jp.breakupTime);
            double front = v * t - S + 0.5 * stretch, len = std::max(stretch, 1e-5);
            bool eating = false;
            if (el.fate == phys::JetElementFate::Residual) {
                if (!through) front = std::min(front, bottom);
            } else if (t < el.arrival) {
                front = std::min(front, bottom);
            } else if (t < el.finish) {
                const double f = (el.finish - t) / std::max(el.finish - el.arrival, 1e-12);
                front = bottom;
                len = std::max(el.length * f, 1e-5);
                consuming = eating = true;
            } else {
                continue;
            }
            if (!eating) {
                if (k % stride != 0) continue;
                len *= stride;
            }
            double y = 0;
            float alpha = 1.0f;
            if (broken) {
                // Particles that drift wider than the crater would hit its wall: drawn faint. Inside
                // the stack they stay in the hole.
                y = jp.drift * v * (t - jp.breakupTime) * phys::jetParticleOffset(k);
                if (std::abs(y) > jp.craterRadius) alpha = 0.3f;
                const bool inHole = front > h.front().depth && front - len < back;
                if (inHole || (!through && front > 0)) y = std::clamp(y, -jp.craterRadius, jp.craterRadius);
            }
            const double f = static_cast<double>(k) / std::max(n - 1, 1);
            const double radius = jp.jetRadius * (0.6 + 1.8 * f * f);
            const float hot = static_cast<float>(1.0 - f);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32({1.0f, 0.55f + 0.4f * hot, 0.25f + 0.6f * hot * hot, alpha});
            ImVec2 a = toScreen(front - len, y + radius), b = toScreen(front, y - radius);
            const float minW = broken ? 2.5f : 1.0f;
            if (b.x - a.x < minW) a.x = b.x - minW;
            if (b.y - a.y < 2.0f) {
                const float mid = 0.5f * (a.y + b.y);
                a.y = mid - 1.0f;
                b.y = mid + 1.0f;
            }
            draw->AddRectFilled(a, b, col, broken ? 0.5f * std::min(b.x - a.x, b.y - a.y) : 0.0f);
        }
        if (consuming && !through) glow(bottom, jp.jetRadius * 3);
    }

    std::string phase;
    if (!contact) {
        phase = jet ? "Jet stretching towards the plate" : "Approaching";
    } else if (!ended) {
        if (now.layer < 0)
            phase = "Crossing a gap";
        else if (now.layer < static_cast<int>(c.layers.size()))
            phase = "Penetrating layer " + std::to_string(now.layer + 1);
        else
            phase = "Through the stack";
    } else {
        char buf[96];
        if (r.perforated)
            std::snprintf(buf, sizeof(buf), "Perforated, residual %.0f m/s", r.residualVelocity);
        else
            std::snprintf(buf, sizeof(buf), "Stopped at %.0f mm along the shot line", r.depth * 1e3);
        phase = buf;
    }
    drawOverlays(draw, origin, size, phase);
}

// Legend, scale bar, time, phase and hint, shared by both displays.
void ArmourUi::State::drawOverlays(ImDrawList* draw, ImVec2 origin, ImVec2 size, const std::string& phase) {
    const ImVec2 end{origin.x + size.x, origin.y + size.y};
    // Where the engineering model puts the burst: a ring on the shot line, filled once it has burst.
    if (out.fuze.triggered) {
        const ImVec2 at{static_cast<float>(origin.x + 0.5 * size.x + (out.fuze.burstDepth - cx) * scale),
                        static_cast<float>(origin.y + 0.5 * size.y + cy * scale)};  // on the shot line, either mirror
        const float r = 7.0f;
        if (t >= out.fuze.burstTime) draw->AddCircleFilled(at, r, colour(0xff8a3au, 0.55f));
        draw->AddCircle(at, r, colour(0xffc070u, 0.9f), 0, 1.5f);
        draw->AddLine({at.x - r - 4, at.y}, {at.x - r + 2, at.y}, colour(0xffc070u, 0.9f), 1.5f);
        draw->AddLine({at.x + r - 2, at.y}, {at.x + r + 4, at.y}, colour(0xffc070u, 0.9f), 1.5f);
        draw->AddText({at.x + r + 4, at.y - r - 14}, colour(0xffc070u, 0.9f), "burst");
    }
    float legendW = 0;
    for (size_t i = 0; i < c.layers.size(); ++i) {
        const int m = c.layers[i].material;
        if (m >= 0 && m < static_cast<int>(db.materials().size()))
            legendW = std::max(legendW, ImGui::CalcTextSize(db.materials()[static_cast<size_t>(m)].name.c_str()).x);
    }
    draw->AddRectFilled({origin.x + 4, origin.y + 4},
                        {origin.x + 130 + legendW, origin.y + 8 + c.layers.size() * (ImGui::GetTextLineHeight() + 2)},
                        colour(kBackground, 0.75f), 4.0f);
    float ly = origin.y + 6;
    for (size_t i = 0; i < c.layers.size(); ++i) {
        const int m = c.layers[i].material;
        if (m < 0 || m >= static_cast<int>(db.materials().size())) continue;
        const ArmourMaterialEntry& me = db.materials()[static_cast<size_t>(m)];
        draw->AddRectFilled({origin.x + 8, ly + 2}, {origin.x + 18, ly + 12}, colour(me.color));
        char lbl[200];
        std::snprintf(lbl, sizeof(lbl), "%zu  %s, %.0f mm%s", i + 1, me.name.c_str(), c.layers[i].thickness * 1e3,
                      i > 0 && c.layers[i].gap > 0 ? (", gap " + mm(c.layers[i].gap)).c_str() : "");
        draw->AddText({origin.x + 24, ly}, colour(0xe6e8ebu), lbl);
        ly += ImGui::GetTextLineHeight() + 2;
    }
    double bar = std::pow(10.0, std::floor(std::log10(120.0 / scale)));
    if (bar * scale < 50) bar *= (bar * 5 * scale < 150) ? 5 : 2;
    const ImVec2 b0{origin.x + 12, end.y - 14};
    const ImVec2 b1{b0.x + static_cast<float>(bar * scale), b0.y};
    draw->AddLine(b0, b1, colour(0xffffffu, 0.8f), 2.0f);
    draw->AddLine({b0.x, b0.y - 4}, {b0.x, b0.y + 4}, colour(0xffffffu, 0.8f));
    draw->AddLine({b1.x, b1.y - 4}, {b1.x, b1.y + 4}, colour(0xffffffu, 0.8f));
    char sl[32];
    if (bar >= 1)
        std::snprintf(sl, sizeof(sl), "%.0f m", bar);
    else
        std::snprintf(sl, sizeof(sl), "%.4g mm", bar * 1e3);
    draw->AddText({b0.x, b0.y - 18}, colour(0xffffffu, 0.8f), sl);


    char tl[64];
    std::snprintf(tl, sizeof(tl), "t = %.1f \xC2\xB5s", t * 1e6);
    const ImVec2 ts = ImGui::CalcTextSize(tl), ps = ImGui::CalcTextSize(phase.c_str());
    draw->AddText({end.x - ts.x - 10, origin.y + 6}, colour(0xffffffu, 0.9f), tl);
    draw->AddText({end.x - ps.x - 10, origin.y + 8 + ts.y}, colour(0xffd28au, 0.9f), phase.c_str());
    const char* hint = "wheel: zoom  drag a plate: move it  drag elsewhere: pan  double-click: fit";
    draw->AddText({end.x - ImGui::CalcTextSize(hint).x - 10, end.y - 20}, colour(0xffffffu, 0.35f), hint);
}

// Builds the scene at the chosen quality and loads it. What does not fit the free video memory
// waits in system memory (when allowed); a level that still does not fit, or fails to load (the
// card or the system is out of memory), gives way to the next one down, with a note; at Low the
// failure is shown instead.
void ArmourUi::State::loadMpmScene() {
    mpmNote.clear();
    mpmMessage.clear();
    // The scene being replaced gives its video memory back when the new one loads.
    const size_t held = solver.loaded() ? solver.storePlan().videoBytes : 0;
    for (;;) {
        const MpmQuality q = static_cast<MpmQuality>(mpmQuality);
        std::string why;
        try {
            MpmScene scene = buildMpmScene(db, c, rodMaterial, shape, out, q, mpmSoftMounts);
            mpmMessage = scene.supported ? std::string() : scene.message;
            if (scene.supported) {
                MpmStoreLimits limits;
                const size_t avail = gl::freeVideoBytes();
                limits.freeVideoBytes = avail > 0 ? avail + held : 0;
                limits.freeHostBytes = freeHostBytes();
                limits.maxBlockBytes = gl::maxStorageBlockBytes();
                limits.allowSpill = mpmUseSystemRam;
                const MpmStorePlan plan = mpmPlanStore(scene.particles.size(), scene.nx, scene.ny, limits);
                if (!plan.ok)
                    why = plan.problem;
                else
                    solver.load(std::move(scene), &limits);
                if (plan.ok && plan.spilledChunks > 0)
                    logx::info("MPM %s: %d of %d chunks wait in system memory (%.1f GB)", mpmQualityLabel(q), plan.spilledChunks,
                               plan.chunks, static_cast<double>(plan.hostBytes) / 1073741824.0);
            } else {
                solver.load(std::move(scene));
            }
        } catch (const std::exception& e) {
            solver.unload();
            why = std::string("could not be loaded (") + e.what() + ")";
        }
        if (why.empty()) return;
        logx::warn("MPM quality %s %s", mpmQualityLabel(q), why.c_str());
        if (mpmQuality <= 0) {
            mpmMessage = std::string("The deformation simulation ") + why + ".";
            return;
        }
        --mpmQuality;
        mpmNote += std::string(mpmQualityLabel(q)) + " " + why + "; showing " +
                   mpmQualityLabel(static_cast<MpmQuality>(mpmQuality)) + " instead.\n";
    }
}

// Builds the simulation when the case or quality changed and steps it up to the playback time
// within the frame budget; while it is behind, playback waits for it. Seeking back resumes from
// the latest checkpoint before the time (or restarts it when none fits in memory).
void ArmourUi::State::updateMpm() {
    mpmBehind = false;
    if (!deformation && snaps.active()) {  // the frames hold a lot of GPU memory
        snaps.clear();
        prerendering = false;
    }
    if (!deformation) ckpt.clear();
    if (!deformation || !valid) return;
    if (!mpmInit) {
        mpmInit = true;
        try {
            solver.init(root);
            mpmView.init(root);
            snaps.init(root);
        } catch (const std::exception& e) {
            mpmError = std::string("The deformation simulation could not start:\n") + e.what();
            logx::error("%s", mpmError.c_str());
        }
    }
    if (!mpmError.empty()) return;
    if (mpmStale && !draggingPlate) {  // a plate being dragged: rebuild once it is let go
        mpmStale = false;
        if (!keepView) fit = true;  // the view frames the new domain (not after moving a plate)
        keepView = false;
        snaps.clear();  // a pre-render under way starts again on the new case (and frees its memory)
        ckpt.clear();   // the checkpoints are of the old scene
        loadMpmScene();
        measure = solver.measure();
        mpmReach = 0;
        mpmLog.clear();
        witness.clear();
        witnessRead = false;
    }
    if (!solver.loaded()) return;
    solver.extendTo(animEnd());
    // Frames are only good for the end they were made for.
    if (snaps.active() && snapEnd != animEnd()) snaps.clear();
    if (prerendering) {
        ckpt.clear();  // the frames need the memory
        stepPrerender();
        return;
    }
    if (snaps.complete()) {
        ckpt.clear();
        measure = snaps.measureAt(t);
        mpmReach = measure.valid ? measure.reach : 0;
        return;
    }
    const MpmScene& sc = solver.scene();
    const double target = std::min(t, sc.tEnd);
    if (ckpt.end() != animEnd()) ckpt.begin(solver, animEnd());
    bool sought = false;
    if (solver.steps() > 0 && target < solver.time() - 0.5 * sc.dt) {
        double reach = 0;
        if (ckpt.restore(solver, target, reach)) {
            mpmReach = reach;
        } else {
            solver.reset();
            mpmReach = 0;
        }
        sought = true;
    }
    const int before = solver.steps();
    // Stops at each checkpoint due on the way to take it.
    const double budget = blocking && !playing ? 1e9 : kMpmFrameBudgetMs;
    const auto start = std::chrono::steady_clock::now();
    bool caught = false;
    for (;;) {
        const double stop = std::min(target, ckpt.nextTime());
        const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        caught = solver.advanceTo(stop, std::max(budget - spent, 0.0));
        ckpt.capture(solver, mpmReach);
        if (!caught || stop >= target) break;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budget) {
            caught = false;
            break;
        }
    }
    if (!caught) {
        mpmBehind = true;
        if (playing) t = solver.time();
    }
    // While playing the readouts come back a frame or two late instead of stalling the GPU.
    MpmMeasure m;
    if (solver.pollMeasure(m)) measure = m;
    if (solver.steps() != before || sought) {
        if (playing)
            solver.requestMeasure();
        else
            measure = solver.measure();
    }
    if (measure.valid) {
        mpmReach = std::max(mpmReach, measure.reach);
        mpmLogMeasure(mpmLog, measure, (animEnd() - animStart()) / 800);
    }
}

// Simulates the run from its start, packing frames as it goes, within the frame budget (all of
// it for --capture); playback then comes from the frames. Meanwhile the view follows the solver.
void ArmourUi::State::stepPrerender() {
    const MpmScene& sc = solver.scene();
    if (!snaps.active()) {
        solver.reset();
        const double t0 = std::max(animStart(), sc.tStart);
        snaps.begin(solver, t0, std::max(std::min(animEnd(), sc.tEnd), t0 + sc.dt),
                    MpmSnapshots::limitsForThisMachine(mpmUseSystemRam));
        snapEnd = animEnd();
        if (!snaps.active()) {
            prerendering = false;
            mpmMessage = "Not enough memory to pre-render this case.";
            return;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const double budget = blocking ? 1e9 : kMpmFrameBudgetMs;
    while (!snaps.capture(solver)) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ms > budget) break;
        solver.advanceTo(snaps.nextTime(), budget - ms);
    }
    if (!blocking) t = std::clamp(solver.time(), animStart(), animEnd());
    measure = snaps.measureAt(t);
    mpmReach = measure.valid ? measure.reach : 0;
    if (snaps.complete()) {
        prerendering = false;
        if (!blocking) {
            t = animStart();
            playing = true;
        }
    }
}

void ArmourUi::State::drawMpm(ImDrawList* draw, ImVec2 origin, ImVec2 size) {
    const ImVec2 end{origin.x + size.x, origin.y + size.y};
    if (!solver.loaded()) {
        draw->AddRectFilled(origin, end, colour(kBackground));
        drawOverlays(draw, origin, size, "");
        const char* text = !mpmError.empty()     ? mpmError.c_str()
                           : !mpmMessage.empty() ? mpmMessage.c_str()
                                                 : "Preparing the simulation...";
        const float wrap = std::min(size.x - 40, 520.0f);
        const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, wrap);
        draw->AddText(nullptr, 0, {origin.x + 0.5f * (size.x - ts.x), origin.y + 0.5f * (size.y - ts.y)},
                      colour(0xffd28au), text, nullptr, wrap);
        return;
    }
    const MpmScene& sc = solver.scene();
    const float fbScale = ImGui::GetIO().DisplayFramebufferScale.x;
    MpmView::Camera camera;
    camera.cx = cx;
    camera.cy = mirrorY() * cy;  // the simulation's own frame; a mirrored view flips the image
    camera.scale = scale * fbScale;
    camera.width = static_cast<int>(std::lround(size.x * fbScale));
    camera.height = static_cast<int>(std::lround(size.y * fbScale));
    std::vector<unsigned> colours{sc.jet ? kJetColour : rodColour(rodMaterial)};
    for (const PresetLayer& l : c.layers)
        colours.push_back(l.material >= 0 && l.material < static_cast<int>(db.materials().size())
                              ? db.materials()[static_cast<size_t>(l.material)].color
                              : 0x8b939cu);
    for (const std::string& id : sc.partMaterials) {
        phys::ShapePart part;
        part.material = id;
        colours.push_back(partRgb(part));
    }
    // Before the simulation starts a kinetic projectile flies rigidly: draw the start state
    // shifted. A jet starts at the model's own start, so it needs none.
    const bool frames = snaps.complete();
    const double shift = !sc.jet && (frames || solver.steps() == 0) && t < sc.tStart ? c.velocity * (t - sc.tStart) : 0.0;
    const MpmColour mode = static_cast<MpmColour>(mpmColour);
    mpmView.render(solver, camera, mode, colours, shift, frames ? &snaps : nullptr, t);
    const double my = mirrorY();
    draw->AddImage(ImTextureRef(static_cast<ImTextureID>(mpmView.texture())), origin, end, {0, my > 0 ? 1.0f : 0.0f},
                   {1, my > 0 ? 0.0f : 1.0f});

    // Undeformed plate faces, faint, so bulging and flexing stand out; and the shot line.
    auto toScreen = [&](double wx, double wy) {
        return ImVec2{static_cast<float>(origin.x + 0.5 * size.x + (wx - cx) * scale),
                      static_cast<float>(origin.y + 0.5 * size.y - (my * wy - cy) * scale)};
    };
    // What the simulation leaves out when a round stops in a thick block (MpmScene::cutDepth): the
    // rest of the stack, drawn dimmed behind the held cut face.
    if (sc.cutDepth > 0) {
        const double theta = this->theta(), sinT = std::sin(theta), cosT = std::cos(theta), tanT = std::tan(theta);
        const double H = sc.plateHalfExtent;
        for (size_t i = 0; i < out.result.layers.size() && i < c.layers.size(); ++i) {
            const double thick = std::max(0.0, c.layers[i].thickness), front = out.result.layers[i].front;
            const double back = front + thick / cosT, from = std::max(front, sc.cutDepth);
            if (!(back > from) || isBlockLayer(c.layers[i])) continue;
            // The face crossing the shot line at s runs from sigma -H to H, shifted like platePieces' faces.
            auto corner = [&](double s, double g) {
                const double f = (s - front) / (back - front), shift = 0.5 * thick * tanT * (2 * f - 1);
                return toScreen(s - (g + shift) * sinT, (g + shift) * cosT);
            };
            const ImVec2 quad[4] = {corner(from, -H), corner(back, -H), corner(back, H), corner(from, H)};
            draw->AddConvexPolyFilled(quad, 4, colour(colours[1 + i], 0.5f));
            draw->AddLine(quad[0], quad[3], colour(0xffffffu, 0.35f), 1.0f);
        }
    }
    // While a plate is dragged the simulation waits for the release: the picture is dimmed and
    // the plates' new places are drawn filled, the dragged layer brightest.
    if (draggingPlate) draw->AddRectFilled(origin, end, colour(kBackground, 0.6f));
    for (const PlatePiece& piece : platePieces(sc.plateHalfExtent)) {
        ImVec2 pts[4];
        for (int k = 0; k < 4; ++k) pts[k] = toScreen(piece.q[static_cast<size_t>(k)][0], piece.q[static_cast<size_t>(k)][1]);
        if (draggingPlate) {
            const bool grabbed = piece.layer == dragLayer;
            draw->AddConvexPolyFilled(pts, 4, colour(colours[1 + static_cast<size_t>(piece.layer)], grabbed ? 0.8f : 0.4f));
            draw->AddPolyline(pts, 4, colour(0xffffffu, piece.layer == dragLayer ? 0.85f : 0.4f), ImDrawFlags_Closed, 1.5f);
        } else {
            draw->AddLine(pts[0], pts[3], colour(0xffffffu, 0.16f), 1.0f);
            draw->AddLine(pts[1], pts[2], colour(0xffffffu, 0.16f), 1.0f);
        }
    }
    draw->AddLine(toScreen(sc.originX, 0), toScreen(sc.originX + sc.nx * sc.dx, 0), colour(0xffffffu, 0.08f), 1.0f);

    // Colour bar or key.
    if (mode == MpmColour::Failure) {
        struct Key {
            unsigned rgb;
            const char* label;
        };
        std::vector<Key> keys = {{0xffb240u, "plastic strain"},
                                 {0x4dbff2u, "spall (tension)"},
                                 {0xf24033u, "melt"},
                                 {0xd980f2u, "reacted"}};
        if (sc.ceramicCount > 0) keys.push_back({0x8fd16au, "comminuted"});
        if (sc.compositeCount > 0) {
            keys.push_back({0xe8d84au, "delaminated"});
            keys.push_back({0xff7faeu, "fibre break"});
        }
        float width = ImGui::CalcTextSize("Failed by:").x + 10;
        for (const auto& k : keys) width += 24 + ImGui::CalcTextSize(k.label).x;
        // A line above the hint and the scale bar.
        float x = origin.x + 0.5f * (size.x - width), y = end.y - 40;
        draw->AddText({x, y}, colour(0xffffffu, 0.8f), "Failed by:");
        x += ImGui::CalcTextSize("Failed by:").x + 10;
        for (const auto& k : keys) {
            draw->AddRectFilled({x, y + 3}, {x + 10, y + 13}, colour(k.rgb));
            draw->AddText({x + 14, y}, colour(0xffffffu, 0.8f), k.label);
            x += 24 + ImGui::CalcTextSize(k.label).x;
        }
    } else if (mode != MpmColour::Material) {
        const MpmColourScale cs = mpmColourScale(mode, sc);
        const float w = std::min(260.0f, 0.4f * size.x), x0 = origin.x + 0.5f * (size.x - w), y0 = end.y - 30;
        const int n = 48;
        for (int i = 0; i < n; ++i) {
            const float f = (i + 0.5f) / n;
            const Rgb rgb = cs.viridis ? viridisSrgb(f) : infernoSrgb(f);
            draw->AddRectFilled({x0 + w * i / n, y0}, {x0 + w * (i + 1) / n + 0.5f, y0 + 10},
                                ImGui::ColorConvertFloat4ToU32({rgb.r, rgb.g, rgb.b, 1.0f}));
        }
        draw->AddRect({x0 - 1, y0 - 1}, {x0 + w + 1, y0 + 11}, colour(0xffffffu, 0.4f));
        char lo[32], hi[64];
        std::snprintf(lo, sizeof(lo), "%.4g", cs.lo * cs.display);
        std::snprintf(hi, sizeof(hi), "%.4g %s", cs.hi * cs.display, cs.unit);
        draw->AddText({x0, y0 + 12}, colour(0xffffffu, 0.8f), lo);
        draw->AddText({x0 + w - ImGui::CalcTextSize(hi).x, y0 + 12}, colour(0xffffffu, 0.8f), hi);
        const char* title = mpmColourLabel(mode);
        draw->AddText({x0 + 0.5f * (w - ImGui::CalcTextSize(title).x), y0 - 18}, colour(0xffffffu, 0.8f), title);
    }

    std::string phase;
    char buf[128];
    if (prerendering) {
        std::snprintf(buf, sizeof(buf), "Pre-rendering... %.0f%%", 100 * snaps.progress());
        phase = buf;
    } else if (mpmBehind) {
        std::snprintf(buf, sizeof(buf), "Simulating... %.1f \xC2\xB5s", solver.time() * 1e6);
        phase = buf;
    } else if (sc.jet && measure.valid) {
        if (measure.reach > stackBack()) {
            std::snprintf(buf, sizeof(buf), "Jet through the stack, tip %.0f m/s", measure.leadVelocity);
            phase = buf;
        } else if (measure.reacted > 0 && measure.reacted < sc.reactiveCount) {
            std::snprintf(buf, sizeof(buf), "Explosive reacting, %.0f%%", 100.0 * measure.reacted / sc.reactiveCount);
            phase = buf;
        } else if (measure.reach > 0) {
            std::snprintf(buf, sizeof(buf), "Jet tip at %.0f mm along the shot line", measure.reach * 1e3);
            phase = buf;
        } else {
            phase = measure.jetBroken > 0 ? "Jet broken up, approaching the plate" : "Jet stretching towards the plate";
        }
    } else if (solver.steps() == 0) {
        phase = "Approaching";
    } else if (mpmLeftGrid()) {
        phase = "Through the stack, out of the simulated region";
    } else if (measure.valid && measure.intact == 0) {
        phase = "Penetrator fully eroded";
    } else if (measure.valid && measure.nose > stackBack() + 0.5 * c.penetrator.diameter) {
        std::snprintf(buf, sizeof(buf), "Through the stack, %.0f m/s", measure.velocity);
        phase = buf;
    } else if (measure.valid && measure.reacted > 0 && measure.reacted < sc.reactiveCount) {
        std::snprintf(buf, sizeof(buf), "Explosive reacting, %.0f%%", 100.0 * measure.reacted / sc.reactiveCount);
        phase = buf;
    } else if (measure.valid && measure.nose > 0) {
        std::snprintf(buf, sizeof(buf), "Nose at %.0f mm along the shot line", measure.nose * 1e3);
        phase = buf;
    } else {
        phase = "Impact";
    }
    drawOverlays(draw, origin, size, phase);
}

// The fuze and burst of a round with an explosive filler, from the engineering model.
void ArmourUi::State::fuzeReadouts() {
    if (c.penetrator.fuze.type == phys::FuzeType::None || warhead()) return;
    const phys::FuzeEvent& f = out.fuze;
    if (!f.triggered) {
        ImGui::TextUnformatted("Fuze not triggered");
        return;
    }
    const std::vector<phys::LayerResult>& layers = out.result.layers;
    if (f.triggerLayer >= 0)
        ImGui::Text("Fuze triggered at %.1f \xC2\xB5s (plate %d)", f.triggerTime * 1e6, f.triggerLayer + 1);
    else
        ImGui::Text("Fuze triggered at %.1f \xC2\xB5s", f.triggerTime * 1e6);
    char where[64];
    if (f.burstLayer >= 0) {
        std::snprintf(where, sizeof(where), "%s plate %d", f.stopped ? "stopped in" : "in", f.burstLayer + 1);
    } else if (f.burstDepth > stackBack()) {
        std::snprintf(where, sizeof(where), "behind the stack");
    } else {
        int after = 0;
        for (size_t i = 0; i < layers.size(); ++i)
            if (layers[i].back < f.burstDepth) after = static_cast<int>(i) + 1;
        std::snprintf(where, sizeof(where), "in the gap after plate %d", std::max(after, 1));
    }
    const double face = layers.empty() ? 0.0 : layers.front().front;
    ImGui::Text("Burster at %.1f \xC2\xB5s, %s past the first face (%s)", f.burstTime * 1e6,
                mm(f.burstDepth - face).c_str(), where);
}

// Simulated figures next to the engineering model's at the same time.
void ArmourUi::State::mpmReadouts() {
    ImGui::SeparatorText("Deformation simulation");
    if (!mpmNote.empty()) ImGui::TextWrapped("%s", mpmNote.c_str());
    if (mpmBestPrerendered(static_cast<MpmQuality>(mpmQuality))) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s is best pre-rendered: press Pre-render, then watch at any speed.%s",
                           mpmQualityLabel(static_cast<MpmQuality>(mpmQuality)),
                           solver.loaded() && solver.storePlan().spilledChunks > 0
                               ? " Part of the particles are streamed from system memory, so every step is slower."
                               : "");
        ImGui::PopStyleColor();
    }
    if (!mpmError.empty() || !mpmMessage.empty()) {
        ImGui::TextWrapped("%s", !mpmError.empty() ? mpmError.c_str() : mpmMessage.c_str());
        return;
    }
    if (!solver.loaded() || !measure.valid) {
        ImGui::TextDisabled("Preparing...");
        return;
    }
    const phys::PenetrationSample model = sampleAt(out.result.history, t);
    const bool contact = t >= out.result.history.front().time;
    const bool rod = c.penetrator.kind == phys::PenetratorKind::LongRod;
    const MpmScene& sc = solver.scene();
    if (snaps.complete())
        ImGui::Text("Pre-rendered frame at %.1f \xC2\xB5s (of %d)", measure.time * 1e6, snaps.frames());
    else if (prerendering)
        ImGui::Text("Pre-rendering: %.1f \xC2\xB5s (%d steps)", measure.time * 1e6, solver.steps());
    else
        ImGui::Text("Simulated to %.1f \xC2\xB5s (%d steps)", measure.time * 1e6, solver.steps());
    if (sc.jet) {
        ImGui::Text("Jet tip: %s along the shot line", mm(measure.reach).c_str());
        if (contact) {
            ImGui::SameLine();
            ImGui::TextDisabled("(model crater bottom %s)", mm(model.depth).c_str());
        }
        ImGui::Text("Fastest jet material: %.0f m/s", measure.leadVelocity);
        if (contact) {
            ImGui::SameLine();
            ImGui::TextDisabled("(model element at the bottom %.0f m/s)", model.velocity);
        }
        ImGui::Text("Jet intact: %.0f%%", 100.0 * measure.intact / std::max(sc.bodyCount, 1));
        if (measure.jetBroken > 0)
            ImGui::Text("Broken up into segments at %.1f \xC2\xB5s", sc.jetBreakup * 1e6);
        else
            ImGui::Text("Coherent; breaks up at %.1f \xC2\xB5s", sc.jetBreakup * 1e6);
    } else if (measure.intact > 0) {
        ImGui::Text("Nose: %s along the shot line", mm(measure.nose).c_str());
        if (contact) {
            ImGui::SameLine();
            ImGui::TextDisabled("(model %s)", mm(model.depth).c_str());
        }
        ImGui::Text("%s: %s", rod ? "Intact rod" : "Intact shot", mm(measure.residualLength).c_str());
        if (contact && rod) {
            ImGui::SameLine();
            ImGui::TextDisabled("(model %s)", mm(model.length).c_str());
        }
        ImGui::Text("Velocity: %.0f m/s", measure.velocity);
        if (contact) {
            ImGui::SameLine();
            ImGui::TextDisabled("(model %.0f m/s)", model.velocity);
        }
    } else if (mpmLeftGrid()) {
        ImGui::TextUnformatted("Through the stack and out of the simulated region");
    } else {
        ImGui::TextUnformatted("Penetrator fully eroded");
    }
    if (!sc.jet) ImGui::Text("Furthest penetrator material: %s", mm(std::max(measure.reach, mpmReach)).c_str());
    ImGui::Text("Peak: %.0f K, %.2f GPa von Mises", measure.maxTemperature, measure.maxStress * 1e-9);
    ImGui::Text("Peak plastic strain: %.2f", measure.maxPlasticStrain);
    ImGui::Text("Failed plate material: %d particles", measure.failedPlate);
    ImGui::Text("Fastest intact plate material: %.0f m/s", measure.plateSpeed);
    if (const int n = sc.reactiveCount; n > 0)
        ImGui::Text("Explosive reacted: %.0f%%", 100.0 * std::min(measure.reacted, n) / n);
    if (const int n = sc.ceramicCount; n > 0)
        ImGui::Text("Ceramic comminuted: %.0f%%", 100.0 * std::min(measure.comminuted, n) / n);
    if (const int n = sc.compositeCount; n > 0)
        ImGui::Text("Laminate delaminated: %.0f%%, fibres broken: %.0f%%", 100.0 * std::min(measure.delaminated, n) / n,
                    100.0 * std::min(measure.fibreBroken, n) / n);
    ImGui::TextDisabled("%s failed by strain %d, spall %d, melt %d", sc.jet ? "Jet" : "Penetrator", measure.failedStrain,
                        measure.failedSpall, measure.failedMelt);
    if (sc.fillerCount > 0) {
        if (measure.fuzeTriggered)
            ImGui::Text("Fuze triggered at %.1f \xC2\xB5s", measure.fuzeTime * 1e6);
        else
            ImGui::TextUnformatted("Fuze not triggered");
        ImGui::Text("Filler detonated: %.0f%%", 100.0 * std::min(measure.fillerLit, sc.fillerCount) / sc.fillerCount);
    }
    if (measure.retired > 0)
        ImGui::TextColored(ui::palette().warn, "%d particles went unstable and were removed", measure.retired);
    witnessReadouts();
}

ArmourUi::ArmourUi() : s_(std::make_unique<State>()) {}
ArmourUi::~ArmourUi() = default;

void ArmourUi::load(const std::filesystem::path& root) {
    State& s = *s_;
    s.root = root;
    LoadReport report;
    if (!s.db.load((root / "data" / "armour_materials.json").string(), (root / "data" / "projectiles.json").string(),
                   report, s.error)) {
        logx::error("armour catalogue: %s", s.error.c_str());
        return;
    }
    for (const std::string& m : report.messages) logx::warn("%s", m.c_str());
    if (s.db.projectiles().empty() || s.db.presets().empty() || s.db.materials().empty()) {
        s.error = "The armour catalogue is empty.";
        return;
    }
    const int projectile = s.db.findProjectile("m829");
    s.selectProjectile(projectile >= 0 ? projectile : 0);
    int preset = 0;
    for (size_t i = 0; i < s.db.presets().size(); ++i)
        if (s.db.presets()[i].id == "rha_100_60") preset = static_cast<int>(i);
    s.applyPreset(preset);
}

void ArmourUi::setCase(const std::string& projectile, const std::string& preset, double fraction, double obliquityDeg) {
    State& s = *s_;
    if (!s.error.empty()) return;
    if (!projectile.empty()) {
        const int i = s.db.findProjectile(projectile);
        if (i >= 0)
            s.selectProjectile(i);
        else
            logx::warn("armour: unknown projectile '%s'", projectile.c_str());
    }
    if (!preset.empty()) {
        bool found = false;
        for (size_t i = 0; i < s.db.presets().size(); ++i)
            if (s.db.presets()[i].id == preset) {
                s.applyPreset(static_cast<int>(i));
                found = true;
            }
        if (!found) logx::warn("armour: unknown preset '%s'", preset.c_str());
    }
    if (std::abs(obliquityDeg) <= 90) {
        s.c.obliquityDeg = std::clamp(obliquityDeg, -phys::kMaxObliquityDeg, phys::kMaxObliquityDeg);
        s.dirty = true;
    }
    if (fraction >= 0) {
        s.playing = false;
        s.blocking = true;
        s.evaluate();
        s.t = s.animStart() + std::clamp(fraction, 0.0, 1.0) * (s.animEnd() - s.animStart());
    }
}

void ArmourUi::prerender() {
    s_->prerendering = true;
}

ArmourUi::Display ArmourUi::display() const {
    return {s_->deformation, s_->mpmColour, s_->mpmQuality, s_->mpmSoftMounts};
}

void ArmourUi::setDisplay(const Display& d) {
    State& s = *s_;
    s.deformation = d.deformation;
    s.mpmColour = std::clamp(d.colour, 0, static_cast<int>(MpmColour::Count) - 1);
    const int quality = std::clamp(d.quality, 0, static_cast<int>(MpmQuality::Count) - 1);
    if (quality != s.mpmQuality || d.softMounts != s.mpmSoftMounts) s.mpmStale = true;
    s.mpmQuality = quality;
    s.mpmSoftMounts = d.softMounts;
}

void ArmourUi::frame(double dt, float width, float height, float top) {
    State& s = *s_;
    if (s.error.empty() && s.dirty && !s.c.layers.empty()) s.evaluate();
    if (s.valid && s.playing && !s.prerendering) {
        const double t0 = s.animStart(), t1 = s.animEnd();
        // The same pace however far the end is stretched.
        s.t += std::min(dt, 0.1) * (s.baseEnd() - t0) / s.playSeconds;
        if (s.t >= t1) {
            if (s.loop) {
                s.t = t0;
            } else {
                s.t = t1;
                s.playing = false;
            }
        }
    }
    s.updateMpm();
    const float sc = ui::scale(), pad = std::floor(4 * sc);
    const float left = std::floor(std::min(400.0f * sc, 0.3f * width)), right = std::floor(std::min(380.0f * sc, 0.28f * width));
    const float midW = std::max(width - left - right - 4 * pad, 200.0f * sc);
    const float avail = height - top - 2 * pad;
    s.setupWindow(pad, top + pad, left, avail);
    s.sectionWindow(left + 2 * pad, top + pad, midW, avail * 0.64f);
    s.historyWindow(left + 2 * pad, top + 2 * pad + avail * 0.64f, midW, avail * 0.36f - pad);
    s.resultsWindow(width - right - pad, top + pad, right, avail);
}
