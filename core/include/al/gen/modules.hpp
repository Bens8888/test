#pragma once
// The structure library: pieces of real layouts and what training learned about each style
// (training/corpus/train.py writes core/src/gen/module_library.inc).
#include <cstddef>

namespace al::gen {

struct ModulePiece {
    int id;
    float x, y;       // object position from the module's lower left cell corner
    int rot;
    int fx, fy;
};

struct Module {
    int first, count; // its pieces
    int w, h;         // size in cells
    int directed;     // 1: it hangs over the path with its dangerous side down; 0: either way
    int modes;        // bit 0 ground modes, bit 1 flying modes, bit 2 wave parts
    int weight[5];    // how often each style uses it
};

struct StyleStats {
    const char* name;
    float pitchLo, pitchHi;  // common free cells between neighbouring structures in a lane
    float repeat;            // share of neighbours that are the same shape
    float rails;             // share of pieces that are thin rails
    float saws;              // saws per structure
};

// Rail, Slope, Compact, Spiky, Classic (RobTop's levels and rated Harder to Insane levels)
constexpr int kStructureStyles = 5;

const Module* moduleTable(size_t& count);
const ModulePiece* modulePieces();
const StyleStats& structureStyle(int style);

} // namespace al::gen
