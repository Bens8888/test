#pragma once
// Generator v2, step 3: structures around the gameplay path.
//  1. Envelope: every click is simulated pressed early and late (within the difficulty's timing
//     window); everywhere those runs go is reserved. Nothing is ever built there, so every
//     placement keeps the timing windows by construction.
//  2. Necessity: the run without the click is simulated; where it leaves the reserved space a
//     hazard is placed (a spike on the surface it runs on, a pillar or a saw in its way).
//  3. Framing: hanging pillars with spike tips over jump apexes, rising pillars under the path,
//     saws in pockets, corridors with 45-degree slopes for wave and ship, spikes on the bounds.
//  4. The cells are tiled (outline silhouettes), the result is verified with the bot.
#include <al/gen/path.hpp>
#include <al/gen/tiles.hpp>
#include <al/level/level.hpp>
#include <al/util/rng.hpp>

#include <string>
#include <vector>

namespace al::gen {

struct ClickCheck {
    int click = -1;
    bool forced = false;        // skipping the click kills the player
    int early = 0, late = 0;    // measured window (ticks the press can move and still survive)
};

struct StructureResult {
    level::Level level;         // the finished layout (path objects + structures + hazards)
    CellGrid cells;             // every solid cell (before tiling)
    std::vector<ClickCheck> checks;
    int hazards = 0, pillars = 0, saws = 0, corridorCells = 0;
    bool beatable = false;
    std::vector<std::string> notes;
};

struct StructureParams {
    TileLook look = TileLook::Outline;
    float density = -1;         // 0..1 framing density (<0 = from the style and intensity)
    bool necessity = true;      // hazards that make every click necessary
};

StructureResult buildStructures(const PathPlan& path, const DesignPlan& plan, const GenOptions& opt, Rng& rng, const StructureParams& prm = {});

} // namespace al::gen
