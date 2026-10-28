#include <al/gen/tiles.hpp>

#include <algorithm>
#include <cmath>

namespace al::gen {

namespace oid = level::id;
using level::LevelObject;

namespace {

constexpr float kCell = 30.0f;
constexpr int kOutlineSlope = 1338, kBlocksSlope = 1743;
constexpr int kOutlineWide = 1339, kBlocksWide = 1744;
constexpr float kLineHalf = 0.75f; // half thickness of the outline edge piece (30 x 1.5)

LevelObject make(int id, double x, double y, double rot = 0) {
    LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

// An edge line (outline top piece) on one side of a cell.
LevelObject edgeLine(int cx, int cy, uint8_t side) {
    const double x0 = cx * kCell, y0 = cy * kCell;
    switch (side) {
        case EdgeT: return make(oid::OutlineTop, x0 + 15, y0 + kCell - kLineHalf, 0);
        case EdgeR: return make(oid::OutlineTop, x0 + kCell - kLineHalf, y0 + 15, 90);
        case EdgeB: return make(oid::OutlineTop, x0 + 15, y0 + kLineHalf, 180);
        default: return make(oid::OutlineTop, x0 + kLineHalf, y0 + 15, 270);
    }
}

void placeSlope(int cx, int cy, CellKind k, int id, std::vector<LevelObject>& out, int wideId) {
    LevelObject o = make(id, cx * kCell + 15, cy * kCell + 15);
    switch (k) {
        // a 2-wide slope is one object centred on its two cells (placed with the left half)
        case CellKind::WideFloorUpL: case CellKind::WideFloorDownL: case CellKind::WideCeilUpL: case CellKind::WideCeilDownL:
            o = make(wideId, cx * kCell + kCell, cy * kCell + 15);
            o.flipX = k == CellKind::WideFloorDownL || k == CellKind::WideCeilUpL;
            o.flipY = k == CellKind::WideCeilUpL || k == CellKind::WideCeilDownL;
            out.push_back(o);
            return;
        case CellKind::WideFloorUpR: case CellKind::WideFloorDownR: case CellKind::WideCeilUpR: case CellKind::WideCeilDownR:
            return;
        default: break;
    }
    // simulator orientation: rot/90 + (flipX+flipY: 2, flipX: 1, flipY: 3); 0 = floor uphill
    switch (k) {
        case CellKind::SlopeFloorUp: break;
        case CellKind::SlopeFloorDown: o.flipX = true; break;
        case CellKind::SlopeCeilUp: o.flipX = o.flipY = true; break;
        case CellKind::SlopeCeilDown: o.flipY = true; break;
        default: return;
    }
    out.push_back(o);
}

} // namespace

uint8_t fullSides(CellKind k) {
    switch (k) {
        case CellKind::Solid: return EdgeT | EdgeR | EdgeB | EdgeL;
        case CellKind::SlopeFloorUp: return EdgeB | EdgeR;
        case CellKind::SlopeFloorDown: return EdgeB | EdgeL;
        case CellKind::SlopeCeilUp: return EdgeT | EdgeL;
        case CellKind::SlopeCeilDown: return EdgeT | EdgeR;
        case CellKind::WideFloorUpL: return EdgeB;
        case CellKind::WideFloorUpR: return EdgeB | EdgeR;
        case CellKind::WideFloorDownL: return EdgeB | EdgeL;
        case CellKind::WideFloorDownR: return EdgeB;
        case CellKind::WideCeilUpL: return EdgeT | EdgeL;
        case CellKind::WideCeilUpR: return EdgeT;
        case CellKind::WideCeilDownL: return EdgeT;
        case CellKind::WideCeilDownR: return EdgeT | EdgeR;
        default: return 0;
    }
}

void CellGrid::set(int cx, int cy, CellKind k) {
    if (k == CellKind::Empty) m_cells.erase(key(cx, cy));
    else m_cells[key(cx, cy)] = k;
}

CellKind CellGrid::get(int cx, int cy) const {
    auto it = m_cells.find(key(cx, cy));
    return it == m_cells.end() ? CellKind::Empty : it->second;
}

void CellGrid::fill(int cx0, int cy0, int cx1, int cy1, CellKind k) {
    for (int x = std::min(cx0, cx1); x <= std::max(cx0, cx1); ++x)
        for (int y = std::min(cy0, cy1); y <= std::max(cy0, cy1); ++y) set(x, y, k);
}

uint8_t CellGrid::exposed(int cx, int cy) const {
    const uint8_t mine = fullSides(get(cx, cy));
    uint8_t e = 0;
    if ((mine & EdgeT) && !(fullSides(get(cx, cy + 1)) & EdgeB)) e |= EdgeT;
    if ((mine & EdgeR) && !(fullSides(get(cx + 1, cy)) & EdgeL)) e |= EdgeR;
    if ((mine & EdgeB) && !(fullSides(get(cx, cy - 1)) & EdgeT)) e |= EdgeB;
    if ((mine & EdgeL) && !(fullSides(get(cx - 1, cy)) & EdgeR)) e |= EdgeL;
    return e;
}

uint8_t rotateEdges(uint8_t e, int q) {
    q = ((q % 4) + 4) % 4;
    for (int i = 0; i < q; ++i) e = uint8_t(((e << 1) | (e >> 3)) & 15);
    return e;
}

uint8_t pieceEdgesAtZero(int id) {
    switch (id) {
        case oid::OutlineSquare: return EdgeT | EdgeR | EdgeB | EdgeL;
        case oid::OutlineTop: return EdgeT;
        case oid::OutlineCorner: return EdgeT | EdgeL;
        case oid::OutlineTopPillar: return EdgeT | EdgeL | EdgeR;
        case oid::OutlinePillar: return EdgeL | EdgeR;
        default: return 0;
    }
}

uint8_t drawnEdges(const LevelObject& o) {
    uint8_t e = pieceEdgesAtZero(o.id);
    if (!e) return 0;
    if (o.flipX) e = uint8_t((e & (EdgeT | EdgeB)) | ((e & EdgeL) ? EdgeR : 0) | ((e & EdgeR) ? EdgeL : 0));
    if (o.flipY) e = uint8_t((e & (EdgeL | EdgeR)) | ((e & EdgeT) ? EdgeB : 0) | ((e & EdgeB) ? EdgeT : 0));
    return rotateEdges(e, int(std::lround(o.rotation / 90.0)));
}

void tileCells(const CellGrid& g, TileLook look, std::vector<LevelObject>& out) {
    static const int kPieces[] = {oid::OutlineSquare, oid::OutlineTopPillar, oid::OutlineCorner, oid::OutlinePillar};
    // deterministic order (column by column, bottom to top) on every platform
    std::vector<std::pair<uint64_t, CellKind>> sorted(g.cells().begin(), g.cells().end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        const int ax = int(int32_t(uint32_t(a.first >> 32))), bx = int(int32_t(uint32_t(b.first >> 32)));
        if (ax != bx) return ax < bx;
        return int(int32_t(uint32_t(a.first))) < int(int32_t(uint32_t(b.first)));
    });
    for (const auto& [k, kind] : sorted) {
        const int cx = int(int32_t(uint32_t(k >> 32))), cy = int(int32_t(uint32_t(k)));
        const uint8_t e = g.exposed(cx, cy);
        if (look == TileLook::Blocks) {
            if (kind == CellKind::Solid) { if (e) out.push_back(make(oid::Block, cx * kCell + 15, cy * kCell + 15)); }
            else placeSlope(cx, cy, kind, kBlocksSlope, out, kBlocksWide);
            continue;
        }
        if (kind != CellKind::Solid) {
            placeSlope(cx, cy, kind, kOutlineSlope, out, kOutlineWide);
            for (uint8_t side : {EdgeT, EdgeR, EdgeB, EdgeL}) if (e & side) out.push_back(edgeLine(cx, cy, side));
            continue;
        }
        if (!e) continue; // interior: unreachable, nothing drawn
        bool done = false;
        for (int id : kPieces) {
            for (int q = 0; q < 4 && !done; ++q) {
                if (rotateEdges(pieceEdgesAtZero(id), q) != e) continue;
                out.push_back(make(id, cx * kCell + 15, cy * kCell + 15, q * 90.0));
                done = true;
            }
            if (done) break;
        }
        if (done) continue;
        // a single edge (or a combination no piece covers): edge lines
        for (uint8_t side : {EdgeT, EdgeR, EdgeB, EdgeL}) if (e & side) out.push_back(edgeLine(cx, cy, side));
    }
}

} // namespace al::gen
