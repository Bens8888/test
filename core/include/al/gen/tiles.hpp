#pragma once
// Structures as a grid of 30x30 cells (solid or 45-degree slope), turned into level objects.
// The outline look draws only the silhouette of each structure: every solid cell gets the outline
// piece that matches its exposed edges, interior cells get nothing (they cannot be reached).
#include <al/level/level.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace al::gen {

// Cell (cx, cy) covers x in [30 cx, 30 cx + 30), y in [30 cy, 30 cy + 30); y = 0 is the ground.
enum class CellKind : uint8_t {
    Empty = 0,
    Solid,
    SlopeFloorUp,    // "/", solid below the line (bottom and right sides full)
    SlopeFloorDown,  // "\", solid below the line (bottom and left sides full)
    SlopeCeilUp,     // "/", solid above the line (top and left sides full)
    SlopeCeilDown,   // "\", solid above the line (top and right sides full)
    // 2-wide slopes (26.6 degrees) over two cells: the left (L) and right (R) half
    WideFloorUpL, WideFloorUpR,
    WideFloorDownL, WideFloorDownR,
    WideCeilUpL, WideCeilUpR,
    WideCeilDownL, WideCeilDownR,
};

// Edge bits of a cell.
enum : uint8_t { EdgeT = 1, EdgeR = 2, EdgeB = 4, EdgeL = 8 };

// Which sides of a cell are completely filled by it (Solid: all four).
uint8_t fullSides(CellKind k);

class CellGrid {
public:
    static uint64_t key(int cx, int cy) { return (uint64_t(uint32_t(cx)) << 32) | uint32_t(cy); }
    void set(int cx, int cy, CellKind k);
    CellKind get(int cx, int cy) const;
    bool filled(int cx, int cy) const { return get(cx, cy) != CellKind::Empty; }
    void fill(int cx0, int cy0, int cx1, int cy1, CellKind k = CellKind::Solid); // inclusive
    void clear(int cx, int cy) { set(cx, cy, CellKind::Empty); }
    // Edges of the cell not covered by a neighbour (a side is covered when the neighbour's
    // facing side is full).
    uint8_t exposed(int cx, int cy) const;
    const std::unordered_map<uint64_t, CellKind>& cells() const { return m_cells; }
    size_t size() const { return m_cells.size(); }

private:
    std::unordered_map<uint64_t, CellKind> m_cells;
};

enum class TileLook : uint8_t { Outline = 0, Blocks };

// Level objects for the grid (appended to `out`).
void tileCells(const CellGrid& g, TileLook look, std::vector<level::LevelObject>& out);

// The outline piece table (one place to correct if GD draws a piece differently): the edges a
// piece draws at rotation 0. Rotating by 90 degrees clockwise moves T->R->B->L.
uint8_t pieceEdgesAtZero(int id);
uint8_t rotateEdges(uint8_t edges, int quarterTurnsClockwise);
// Edges drawn by a placed object (id + rotation/flips), 0 if it is not an edge piece.
uint8_t drawnEdges(const level::LevelObject& o);

} // namespace al::gen
