#include "app/projectile_draw.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace {

// x, y (world m) and which cut made the point (0: none), so outlines skip the cut lines.
struct Pt {
    double x, y;
    int cut;
};
using Poly = std::vector<Pt>;

ImU32 rgb(unsigned c) { return IM_COL32((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, 255); }

ImU32 shaded(ImU32 c, float shade, float alpha = 1.0f) {
    ImVec4 f = ImGui::ColorConvertU32ToFloat4(c);
    f.x = std::min(f.x * shade, 1.0f);
    f.y = std::min(f.y * shade, 1.0f);
    f.z = std::min(f.z * shade, 1.0f);
    f.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(f);
}

// Keeps a*x + b*y <= c (Sutherland-Hodgman; a non-convex outline stays one piece along the cut).
void clip(Poly& poly, double a, double b, double c, int id) {
    Poly out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Pt& p = poly[i];
        const Pt& q = poly[(i + 1) % poly.size()];
        const double fp = a * p.x + b * p.y - c, fq = a * q.x + b * q.y - c;
        if (fp <= 0) out.push_back(p);
        if ((fp < 0 && fq > 0) || (fp > 0 && fq < 0)) {
            const double k = fp / (fp - fq);
            out.push_back({p.x + k * (q.x - p.x), p.y + k * (q.y - p.y), id});
        }
    }
    poly.swap(out);
}

enum Cut { kNose = 1, kAhead, kFace };

} // namespace

unsigned partRgb(const phys::ShapePart& part) {
    using phys::PartRole;
    switch (part.role) {
    case PartRole::Filler: return 0xb8862bu;      // yellow ochre
    case PartRole::Fuze: return 0xd4b45au;        // brass
    case PartRole::Tracer: return 0x7a6a5cu;      // dull brown-grey pellet
    case PartRole::Jacket:
    case PartRole::Band: return 0xb8733au;        // copper
    case PartRole::Incendiary: return 0x8e3b32u;  // dull red
    case PartRole::Propellant: return 0x4a4036u;  // dark grey-brown grain
    default: break;
    }
    const std::string_view m = part.material;
    if (m.rfind("explosive:", 0) == 0) return 0xb8862bu;
    if (m == "lead") return 0x6f7884u;
    if (m == "tungsten_carbide") return 0x45484du;
    if (m == "light_alloy") return 0xc9ced4u;
    if (m == "cap_steel") return 0x6b7178u;
    if (m == "gilding_metal") return 0xb8733au;
    if (m == "incendiary") return 0x8e3b32u;
    if (m == "tungsten_alloy") return 0x9da2a8u;
    if (m == "du") return 0x9a9a62u;
    if (m == "core_steel") return 0x7f868eu;
    return 0x8f969eu;  // mild, case and the penetrator's own steel
}

ImU32 partColour(const phys::ShapePart& part) { return rgb(partRgb(part)); }

void drawProjectileShape(ImDrawList* draw, const phys::ProjectileShape& shape, const ShapeDrawFrame& frame) {
    // Shape x runs from the tip backwards; the penetrator's back rides at the eroded tail.
    const double tail = frame.noseX - frame.erodedLength;
    auto worldX = [&](double x) { return tail + (shape.penetratorBack - x); };
    const bool faceClip = std::isfinite(frame.firstFaceClip);

    // Wide parts first, so cores and fillers sit on top of what surrounds them.
    std::vector<const phys::ShapePart*> order;
    for (const phys::ShapePart& part : shape.parts)
        if (part.stations.size() >= 2) order.push_back(&part);
    auto widest = [](const phys::ShapePart* p) {
        double r = 0;
        for (const phys::ShapeStation& s : p->stations) r = std::max(r, s.rOuter);
        return r;
    };
    std::stable_sort(order.begin(), order.end(), [&](auto* a, auto* b) { return widest(a) > widest(b); });

    for (const phys::ShapePart* part : order) {
        const auto& st = part->stations;
        // Behind the nose everything is drawn. Ahead of it the penetrator has eroded away, and the
        // other parts (a dart's cap, a bullet's jacket tip) are crushed against the first face.
        auto clipPart = [&](const Poly& v) {
            std::vector<Poly> pieces;
            Poly behind = v;
            clip(behind, 1, 0, frame.noseX, kNose);
            if (behind.size() >= 3) pieces.push_back(std::move(behind));
            if (!part->penetrator) {
                Poly ahead = v;
                clip(ahead, -1, 0, -frame.noseX, kAhead);
                if (faceClip) clip(ahead, 1, frame.firstFaceSlope, frame.firstFaceClip, kFace);
                if (ahead.size() >= 3) pieces.push_back(std::move(ahead));
            }
            return pieces;
        };
        auto toScreen = [&](const Poly& v) {
            std::vector<ImVec2> pts;
            pts.reserve(v.size());
            for (const Pt& q : v) pts.push_back(frame.toScreen(q.x, q.y));
            return pts;
        };
        const ImU32 base = part->material.empty() && frame.ownColour ? frame.ownColour : partColour(*part);

        // Fill: one quad per station pair per side, |y| in [rInner, rOuter], lit from screen-up.
        for (const double side : {1.0, -1.0}) {
            const float shade = side * frame.mirrorY > 0 ? 1.1f : 0.78f;
            for (size_t i = 0; i + 1 < st.size(); ++i) {
                const phys::ShapeStation &a = st[i], &b = st[i + 1];
                if (b.x - a.x <= 1e-12 || (a.rOuter <= a.rInner && b.rOuter <= b.rInner)) continue;
                const Poly q = {{worldX(a.x), side * a.rInner, 0}, {worldX(b.x), side * b.rInner, 0},
                                {worldX(b.x), side * b.rOuter, 0}, {worldX(a.x), side * a.rOuter, 0}};
                for (const Poly& piece : clipPart(q)) {
                    const std::vector<ImVec2> pts = toScreen(piece);
                    draw->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), shaded(base, shade));
                }
            }
        }

        // Outline: a solid part as one loop round both sides, a hollow one as a ring per side.
        const bool solid = std::all_of(st.begin(), st.end(), [](const auto& s) { return s.rInner <= 0; });
        std::vector<Poly> loops;
        if (solid) {
            Poly v;
            for (const auto& s : st) v.push_back({worldX(s.x), s.rOuter, 0});
            for (auto it = st.rbegin(); it != st.rend(); ++it) v.push_back({worldX(it->x), -it->rOuter, 0});
            loops.push_back(std::move(v));
        } else {
            for (const double side : {1.0, -1.0}) {
                Poly v;
                for (const auto& s : st) v.push_back({worldX(s.x), side * s.rOuter, 0});
                for (auto it = st.rbegin(); it != st.rend(); ++it) v.push_back({worldX(it->x), side * it->rInner, 0});
                loops.push_back(std::move(v));
            }
        }
        const ImU32 edge = shaded(base, 0.55f);
        for (const Poly& v : loops)
            for (const Poly& piece : clipPart(v)) {
                const std::vector<ImVec2> pts = toScreen(piece);
                for (size_t i = 0; i < piece.size(); ++i) {
                    const size_t j = (i + 1) % piece.size();
                    if (piece[i].cut && piece[i].cut == piece[j].cut) continue;  // along a cut
                    draw->AddLine(pts[i], pts[j], edge, 1.0f);
                }
            }
    }
}
