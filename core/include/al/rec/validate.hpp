#pragma once
// Simulator validation against the real game: a run recorded in GD stores the player's state at
// every input; replaying the same inputs in the simulator on the same level must reproduce it.
#include <al/level/level.hpp>
#include <al/rec/recording.hpp>

namespace al::rec {

struct Validation {
    bool simFinishes = false;
    int simTicks = 0;
    std::string simDeath;          // cause if the simulator dies
    int compared = 0;              // input events with a recorded state
    double meanDx = 0, maxDx = 0;  // |x_sim - x_game| (units)
    double meanDy = 0, maxDy = 0;
    int modeMismatches = 0;
    int firstDeviationTick = -1;   // first event deviating by more than `threshold`
};

// yOffset: GD's node y minus the level-string y (90 for a recording made in GD).
Validation validate(const level::Level& lvl, const Recording& rec, float yOffset = 90.f, double threshold = 2.0);

} // namespace al::rec
