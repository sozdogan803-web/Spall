#include "test.h"

#include "data/armour_blocks.h"

#include <vector>

namespace {

PresetLayer full(double thickness, double gap) {
    PresetLayer l;
    l.thickness = thickness;
    l.gap = gap;
    return l;
}

PresetLayer tile(double thickness, double gap, double width, double between, double offset = 0) {
    PresetLayer l = full(thickness, gap);
    l.blockWidth = width;
    l.blockGap = between;
    l.offset = offset;
    return l;
}

PresetLayer blocks(double thickness, double gap, double width, double between, int count, double offset = 0) {
    PresetLayer l = tile(thickness, gap, width, between, offset);
    l.blockCount = count;
    return l;
}

} // namespace

TEST(preset_layer_tiles_along_face) {
    // 250 mm tiles 10 mm apart, one centred on the shot line.
    const PresetLayer era = tile(0.004, 0, 0.25, 0.01);
    const std::vector<LayerSpan> t = layerTiles(era, -0.3, 0.3);
    CHECK(t.size() == 3);
    if (t.size() == 3) {
        CHECK_NEAR(t[0].lo, -0.3, 1e-12);
        CHECK_NEAR(t[0].hi, -0.135, 1e-12);
        CHECK_NEAR(t[1].lo, -0.125, 1e-12);
        CHECK_NEAR(t[1].hi, 0.125, 1e-12);
        CHECK_NEAR(t[2].lo, 0.135, 1e-12);
        CHECK_NEAR(t[2].hi, 0.3, 1e-12);
    }
    CHECK(layerCoversShotLine(era));
    // Shifted 130 mm: the shot line runs down the gap between two tiles.
    CHECK(!layerCoversShotLine(tile(0.004, 0, 0.25, 0.01, 0.13)));
    CHECK(layerCoversShotLine(tile(0.004, 0, 0.25, 0.01, 0.12)));
    // A full plate is one span over the whole window, and always in the way.
    const std::vector<LayerSpan> f = layerTiles(full(0.05, 0), -0.4, 0.4);
    CHECK(f.size() == 1 && f[0].lo == -0.4 && f[0].hi == 0.4);
    CHECK(layerCoversShotLine(full(0.05, 0)));
    CHECK(!isBlockLayer(full(0.05, 0)) && isBlockLayer(era));
}

TEST(armour_drag_layer_clamps_gaps) {
    // A plate, a three-layer ERA sandwich 50 mm behind it, and a base plate 100 mm behind that.
    std::vector<PresetLayer> L = {full(0.02, 0), tile(0.004, 0.05, 0.25, 0.01), tile(0.004, 0, 0.25, 0.01),
                                  tile(0.004, 0, 0.25, 0.01), full(0.06, 0.1)};
    const LayerGroup g = layerGroup(L, 2);
    CHECK(g.first == 1 && g.last == 3);  // the sandwich moves as one
    // Towards the front plate: the gap in front closes at zero, the one behind opens as much.
    CHECK(dragLayers(L, 2, -0.08, 0) == 0);
    CHECK_NEAR(L[1].gap, 0, 1e-12);
    CHECK_NEAR(L[2].gap, 0, 1e-12);
    CHECK_NEAR(L[4].gap, 0.15, 1e-12);
    // Backwards: stops against the base plate.
    dragLayers(L, 3, 0.5, 0);
    CHECK_NEAR(L[1].gap, 0.15, 1e-12);
    CHECK_NEAR(L[4].gap, 0, 1e-12);
    for (const PresetLayer& l : L) CHECK(l.gap >= 0);
    // Along the face: the whole sandwich slides, the full plates do not.
    dragLayers(L, 1, 0, 0.1);
    for (int i = 1; i <= 3; ++i) CHECK_NEAR(L[static_cast<size_t>(i)].offset, 0.1, 1e-12);
    CHECK(L[0].offset == 0 && L[4].offset == 0);
    // The tiles repeat every 260 mm: the offset wraps into half a pitch either side.
    dragLayers(L, 1, 0, 0.2);
    CHECK_NEAR(L[1].offset, 0.04, 1e-12);
    // The first plate stays at the origin: dragging it back closes the gap behind it and the
    // origin moves with it.
    L[1].gap = 0.05;
    CHECK_NEAR(dragLayers(L, 0, 0.02, 0), 0.02, 1e-12);
    CHECK_NEAR(L[1].gap, 0.03, 1e-12);
    CHECK(L[0].gap == 0);
    CHECK_NEAR(dragLayers(L, 0, 0.5, 0), 0.03, 1e-12);
    CHECK_NEAR(L[1].gap, 0, 1e-12);
    // The last layer has nothing behind it to bump into.
    CHECK(dragLayers(L, 4, 0.3, 0) == 0);
    CHECK_NEAR(L[4].gap, 0.3, 1e-12);
}

TEST(preset_layer_discrete_blocks) {
    // Three 250 mm blocks 10 mm apart, the middle one on the shot line, and nothing past them.
    const PresetLayer era = blocks(0.004, 0, 0.25, 0.01, 3);
    const std::vector<LayerSpan> t = layerTiles(era, -1, 1);
    CHECK(t.size() == 3);
    if (t.size() == 3) {
        CHECK_NEAR(t[0].lo, -0.385, 1e-12);
        CHECK_NEAR(t[0].hi, -0.135, 1e-12);
        CHECK_NEAR(t[1].lo, -0.125, 1e-12);
        CHECK_NEAR(t[2].hi, 0.385, 1e-12);
    }
    CHECK(layerTiles(era, 0.4, 1).empty());
    CHECK(layerCoversShotLine(era));
    // Two blocks sit either side of the shot line, which runs down the gap between them.
    CHECK(!layerCoversShotLine(blocks(0.004, 0, 0.25, 0.01, 2)));
    CHECK(layerTiles(blocks(0.004, 0, 0.25, 0.01, 2), -1, 1).size() == 2);
    // Moved off the line a single block no longer covers it.
    CHECK(layerCoversShotLine(blocks(0.004, 0, 0.25, 0.01, 1, 0.1)));
    CHECK(!layerCoversShotLine(blocks(0.004, 0, 0.25, 0.01, 1, 0.13)));
}

TEST(armour_drag_blocks_does_not_wrap) {
    // A block layer with a full plate behind it: dragging along the face moves it as far as asked
    // (a periodic row of tiles wraps, so it looks as if it never moved) and stops at the reach.
    std::vector<PresetLayer> L = {blocks(0.004, 0, 0.25, 0.01, 1), full(0.06, 0.1)};
    dragLayers(L, 0, 0, 0.1);
    CHECK_NEAR(L[0].offset, 0.1, 1e-12);
    dragLayers(L, 0, 0, 0.1);
    CHECK_NEAR(L[0].offset, 0.2, 1e-12);  // past half a pitch, and not wrapped
    dragLayers(L, 0, 0, 5);
    CHECK_NEAR(L[0].offset, kMaxBlockReach, 1e-12);
    dragLayers(L, 0, 0, -9);
    CHECK_NEAR(L[0].offset, -kMaxBlockReach, 1e-12);
    // Three blocks: the one nearest the shot line may be at the reach, the others go beyond it.
    std::vector<PresetLayer> M = {blocks(0.004, 0, 0.25, 0.01, 3), full(0.06, 0.1)};
    dragLayers(M, 0, 0, 5);
    CHECK_NEAR(M[0].offset, kMaxBlockReach + 0.26, 1e-12);
}
