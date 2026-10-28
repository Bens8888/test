#pragma once
// Playtest bot on top of the physics simulator:
//  - run / isBeatable / firstFailure for a given input track
//  - autoSolve: beam search with state de-duplication for an input track that finishes
//  - timing windows: how far each press/release can move (in ticks) and still survive
#include <al/physics/sim.hpp>
#include <al/util/progress.hpp>

#include <cstdint>
#include <vector>

namespace al::bot {

// A press or release at a physics tick (1-based tick of the step in which the state applies).
struct InputEvent {
    int tick = 0;
    bool press = true;
};

// Toggle list: strictly increasing ticks, alternating press/release starting with a press.
struct InputTrack {
    std::vector<InputEvent> events;
    bool heldAt(int tick) const;
    void normalize();               // sort, merge duplicates, enforce alternation
    void add(int tick, bool press) { events.push_back({tick, press}); }
    size_t pressCount() const;
    int lastTick() const { return events.empty() ? 0 : events.back().tick; }
};

struct RunOptions {
    int maxTicks = 240 * 60 * 10;   // 10 minutes
    bool recordTrace = false;       // keep one PlayerState per tick (player 1)
    int traceEvery = 1;
};

struct RunResult {
    bool won = false;
    bool dead = false;
    int ticks = 0;
    int deathTick = -1;
    phys::DeathCause cause = phys::DeathCause::None;
    int deathObject = -1;           // SimObject index (World::objects())
    int deathLevelObject = -1;      // index into Level::objects
    float x = 0, y = 0;
    std::vector<phys::PlayerState> trace;
    std::vector<uint32_t> traceEvents; // per traced tick: event bits (P1)
};

RunResult run(const phys::World& world, const InputTrack& inputs, const RunOptions& opt = {});
RunResult runFrom(const phys::World& world, phys::GameState state, const InputTrack& inputs, const RunOptions& opt = {});
bool isBeatable(const phys::World& world, const InputTrack& inputs);

struct Failure {
    bool failed = false;
    int tick = 0;
    float x = 0, y = 0;
    phys::DeathCause cause = phys::DeathCause::None;
    int levelObject = -1;
};
Failure firstFailure(const phys::World& world, const InputTrack& inputs);

struct SolveOptions {
    int beamWidth = 192;
    int decisionTicks = 2;           // input can change every N ticks
    int maxTicks = 240 * 60 * 10;
    bool preferFewInputs = true;
    float stopAtX = -1;              // stop successfully once x >= stopAtX (segment solving); <0 = level end
    const Progress* progress = nullptr;
};

struct SolveResult {
    bool solved = false;
    InputTrack inputs;
    int expanded = 0;
    float bestX = 0;                 // furthest x reached by any surviving state
    int failTick = 0;
    double seconds = 0;
};

SolveResult autoSolve(const phys::World& world, const SolveOptions& opt = {});
// Solve from an existing state (inputs are relative: ticks continue from state.tick).
SolveResult autoSolveFrom(const phys::World& world, const phys::GameState& start, const SolveOptions& opt = {});

struct TimingWindow {
    int eventIndex = 0;
    int tick = 0;
    bool press = true;
    int early = 0;   // how many ticks earlier still survives (0..maxShift)
    int late = 0;    // how many ticks later still survives
    float x = 0;
    double windowMs() const { return (early + late + 1) * 1000.0 / phys::kTPS; }
};

// For each input event, the shift range [-early, +late] (in ticks, capped at maxShift) over which
// the run still survives at least `horizonTicks` past the event (and to the next-next event).
std::vector<TimingWindow> timingWindows(const phys::World& world, const InputTrack& inputs, int maxShift = 24, int horizonTicks = 120);

} // namespace al::bot
