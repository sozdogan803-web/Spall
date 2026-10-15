// Layers cut into separate tiles along the face (explosive reactive armour, bricks of add-on
// armour), and moving layers by dragging them in the Cross-section window. A layer's tiles are
// blockWidth wide with blockGap between them, tile k centred offset + k (blockWidth + blockGap)
// along the face from where the shot line crosses the layer's mid-plane; blockWidth 0 is one
// full plate. With blockCount > 0 there are only that many blocks, centred on the offset, so the
// armour is a few pieces to move about and not a row across the whole plate. Distances in m.
#pragma once

#include "data/projectile_db.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

inline bool isBlockLayer(const PresetLayer& l) { return l.blockWidth > 0; }

// Dragged blocks (blockCount > 0) stop when the block nearest the shot line is this far from it.
constexpr double kMaxBlockReach = 0.2;

struct LayerSpan {
    double lo = 0, hi = 0;
};

// The parts of the layer between lo and hi along the face: each tile there, clipped to the
// window (a full plate: the whole window).
inline std::vector<LayerSpan> layerTiles(const PresetLayer& l, double lo, double hi) {
    std::vector<LayerSpan> out;
    if (!(hi > lo)) return out;
    if (!isBlockLayer(l)) return {{lo, hi}};
    const double w = l.blockWidth, pitch = w + std::max(l.blockGap, 0.0);
    if (l.blockCount > 0) {
        for (int k = 0; k < l.blockCount; ++k) {
            const double c = l.offset + (k - 0.5 * (l.blockCount - 1)) * pitch;
            const LayerSpan s{std::max(lo, c - 0.5 * w), std::min(hi, c + 0.5 * w)};
            if (s.hi > s.lo) out.push_back(s);
        }
        return out;
    }
    const double k0 = std::floor((lo - l.offset - 0.5 * w) / pitch) + 1;
    const double k1 = std::ceil((hi - l.offset + 0.5 * w) / pitch) - 1;
    for (double k = k0; k <= k1 && out.size() < 4096; ++k) {
        const double c = l.offset + k * pitch;
        const LayerSpan s{std::max(lo, c - 0.5 * w), std::min(hi, c + 0.5 * w)};
        if (s.hi > s.lo) out.push_back(s);
    }
    return out;
}

// Whether the shot line meets the layer (not a gap between its tiles).
inline bool layerCoversShotLine(const PresetLayer& l) {
    if (!isBlockLayer(l)) return true;
    const double pitch = l.blockWidth + std::max(l.blockGap, 0.0);
    if (l.blockCount > 0) {
        const double mid = 0.5 * (l.blockCount - 1);
        const double k = std::clamp(std::round(mid - l.offset / pitch), 0.0, static_cast<double>(l.blockCount - 1));
        return std::abs(l.offset + (k - mid) * pitch) <= 0.5 * l.blockWidth;
    }
    const double k = std::round(-l.offset / pitch);
    return std::abs(l.offset + k * pitch) <= 0.5 * l.blockWidth;
}

// Layers that move together when one is dragged: a run of touching layers (no gap between) cut
// the same way, such as the steel / explosive / steel of an ERA tile.
struct LayerGroup {
    int first = 0, last = 0;
};
inline LayerGroup layerGroup(const std::vector<PresetLayer>& layers, int i) {
    auto same = [&](int a, int b) {
        const PresetLayer &p = layers[static_cast<size_t>(a)], &q = layers[static_cast<size_t>(b)];
        return q.gap <= 1e-9 && std::abs(p.blockWidth - q.blockWidth) < 1e-9 &&
               (!isBlockLayer(p) || (std::abs(p.blockGap - q.blockGap) < 1e-9 && std::abs(p.offset - q.offset) < 1e-9));
    };
    LayerGroup g{i, i};
    while (g.first > 0 && same(g.first - 1, g.first)) --g.first;
    while (g.last + 1 < static_cast<int>(layers.size()) && same(g.last, g.last + 1)) ++g.last;
    return g;
}

// Drags layer i's group by du along the plate normal (+: away from the projectile) and ds along
// the face. The gaps either side stay >= 0 and the layers behind stay put. Tiles slide along the
// face (a row of tiles across the plate wraps its offset within half a pitch; a few blocks do
// not, they stop where the one nearest the shot line is kMaxBlockReach from it); full plates do
// not slide. The first plate's front
// is the origin of the armour timeline, so dragging the first group moves the origin with it:
// returns how far it moved along the normal (0 for any other group).
inline double dragLayers(std::vector<PresetLayer>& layers, int i, double du, double ds) {
    if (i < 0 || i >= static_cast<int>(layers.size())) return 0;
    const LayerGroup g = layerGroup(layers, i);
    PresetLayer* behind = g.last + 1 < static_cast<int>(layers.size()) ? &layers[static_cast<size_t>(g.last + 1)] : nullptr;
    const double inf = std::numeric_limits<double>::infinity();
    const double lo = g.first > 0 ? -layers[static_cast<size_t>(g.first)].gap : -inf;
    const double hi = behind ? behind->gap : inf;
    du = std::clamp(du, lo, hi);
    if (g.first > 0) layers[static_cast<size_t>(g.first)].gap += du;
    if (behind) behind->gap = std::max(0.0, behind->gap - du);
    if (g.first > 0) layers[static_cast<size_t>(g.first)].gap = std::max(0.0, layers[static_cast<size_t>(g.first)].gap);
    if (ds != 0)
        for (int k = g.first; k <= g.last; ++k) {
            PresetLayer& l = layers[static_cast<size_t>(k)];
            if (!isBlockLayer(l)) continue;
            const double pitch = l.blockWidth + std::max(l.blockGap, 0.0);
            l.offset += ds;
            if (l.blockCount > 0) {
                const double reach = kMaxBlockReach + 0.5 * (l.blockCount - 1) * pitch;
                l.offset = std::clamp(l.offset, -reach, reach);
            } else {
                l.offset -= pitch * std::round(l.offset / pitch);
            }
        }
    // Only the first group's move along the normal shifts the origin; with nothing behind it, it
    // cannot move (there is nothing to move against).
    if (g.first == 0) return behind ? du : 0;
    return 0;
}
