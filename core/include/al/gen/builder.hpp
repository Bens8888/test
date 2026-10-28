#pragma once
// Builder: the stateful context in which a layout is constructed part by part. It owns the level
// being built, an incremental physics world, the intended input track and simulation checkpoints,
// and provides the verification primitives (run, timing windows, necessity) the gamemode
// builders use for every placement decision.
#include <al/bot/bot.hpp>
#include <al/gen/plan.hpp>
#include <al/gen/style.hpp>
#include <al/level/level.hpp>
#include <al/physics/sim.hpp>
#include <al/util/rng.hpp>

#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace al::gen {

// Debug log for the generator (enabled with the AL_DEBUG environment variable).
bool debugEnabled();
void debugLog(const char* fmt, ...);

// A sample of a player's path (centre and half size of its hitbox).
struct PathSample { float x, y, half; int player; };

struct ClickResult {
    int click = -1;          // index into DesignPlan::clicks
    int pressEvent = -1;     // index into the input track
    std::string pattern;     // pattern id used for this click
    int windowEarly = 0, windowLate = 0; // ticks
    bool forced = true;      // skipping the click kills
    bool dropped = false;    // could not be built fairly; the click was removed
    float x = 0;
    bool decision = false;   // a pattern decision was made here (features + pattern are a training step)
    std::string label;       // training label (canonical pattern id for the geometry), empty = pattern
    std::array<float, kContextFeatures> features{};
    double windowMs() const { return (windowEarly + windowLate + 1) * 1000.0 / phys::kTPS; }
};

struct PartResult {
    int part = -1;
    bool ok = false;
    bool fallback = false;   // the part was rebuilt with the safe fallback
    int attempts = 0;
    std::string note;
};

// Pattern usage memory (motifs / variety): what was used for which musical context.
struct PatternMemory {
    struct Use { std::string id; int part; uint32_t context; double time; };
    std::vector<Use> uses;
    int countRecent(const std::string& id, size_t lookback) const;
    const Use* lastForContext(uint32_t ctx) const;
};

class Builder {
public:
    Builder(const audio::MusicAnalysis& A, const GenOptions& opt, DesignPlan& plan, const StyleModel* style, Rng rng);

    // Build the whole level (parts in order). Returns false only on cancellation.
    bool build(const Progress* progress = nullptr);
    // Rebuild only part `index` (re-roll one section); parts after it are rebuilt too because
    // the player state flows from one part into the next.
    bool rebuildFrom(int index, const Progress* progress = nullptr);

    // ---- results
    level::Level& level() { return m_level; }
    const level::Level& level() const { return m_level; }
    const bot::InputTrack& inputs() const { return m_inputs; }
    const std::vector<ClickResult>& clickResults() const { return m_clicks; }
    const std::vector<PartResult>& partResults() const { return m_parts; }
    phys::World& world() { return m_world; }
    const DesignPlan& plan() const { return m_plan; }
    const audio::MusicAnalysis& music() const { return m_A; }
    const GenOptions& options() const { return m_opt; }
    double endTime() const { return m_endTime; }

    // ---- primitives used by the gamemode builders
    struct Mark { size_t levelObjects, worldObjects, inputEvents, clickResults, history; };
    Mark mark() const;
    void rollback(const Mark& m);

    int place(level::LevelObject o);                 // gameplay object (also added to the physics world)
    int placeDeco(level::LevelObject o);             // decoration/trigger (level only)
    static level::LevelObject obj(int id, double x, double y, double rot = 0);

    void press(int tick);
    void release(int tick);
    int lastInputTick() const { return m_inputs.events.empty() ? 0 : m_inputs.events.back().tick; }
    bool heldAfterInputs() const { return !m_inputs.events.empty() && m_inputs.events.back().press; }

    // The verified state at the cursor (start of the current unverified region).
    const phys::GameState& cursor() const { return m_cursor; }
    void setCursor(const phys::GameState& s) { m_cursor = s; }
    // Simulate a copy of `from` with the current inputs until `tick`. Returns false if it died
    // (out: final state, death tick in s.tick).
    bool runTo(phys::GameState& s, int tick) const;
    bool runToWithInputs(phys::GameState& s, int tick, const bot::InputTrack& in) const;
    // Advance the cursor to `tick` (must survive). Returns false if it died.
    bool advanceCursor(int tick);

    // Timing window of input event `ev`: shifts in [-maxShift, +maxShift] (not crossing the
    // neighbouring events) that survive until `horizonTick`. Uses the cursor as the start state,
    // so the cursor must be before tick(ev) - maxShift.
    void window(int ev, int horizonTick, int maxShift, int& early, int& late) const;
    // True if removing the press `ev` (and its release) makes the run die before horizonTick.
    bool necessary(int pressEv, int horizonTick) const;
    // Tick of the first press after input event `ev` (or tick(ev) + 120 if there is none).
    int nextPressAfter(int ev) const;
    // The intended path from the cursor to endTick, plus timing variants: every input after
    // startTick shifted by +-halfWin and +-halfWin/2 (neighbouring inputs stay put, a tap moves
    // with its release), each variant followed until a little after the next press, when the
    // player can correct. Geometry kept clear of all samples gives every input at least a
    // 2*halfWin window. Samples stop at a change away from `mode`. False if the nominal path dies.
    bool pathEnvelope(int startTick, int endTick, int halfWin, level::Mode mode, std::vector<PathSample>& out) const;

    // Song time <-> ticks, x of the player at a tick on the intended path (from simulation).
    static int tickOf(double t) { return int(std::lround(t * phys::kTPS)); }
    static double timeOf(int tick) { return double(tick) / phys::kTPS; }

    Rng& rng() { return m_rng; }
    PatternMemory& memory() { return m_memory; }
    const StyleModel* styleModel() const { return m_style; }
    const DifficultyProfile& profileFor(const Part& p) const;

    // Bookkeeping used by the gamemode builders
    void addClickResult(const ClickResult& r) { m_clicks.push_back(r); }
    // Decision history (for sequence models): appended when a pattern is committed.
    const std::vector<HistoryStep>& history() const { return m_history; }
    void addDecision(const float* f, int token) { HistoryStep h; std::copy(f, f + kContextFeatures, h.f); h.token = token; m_history.push_back(h); }
    int nextGroup() { return m_nextGroup++; }

private:
    bool buildPart(int index, const Progress* progress);
    void placeTransition(const Part& prev, const Part& next, bool first);
    bool buildSafeFallback(const Part& part);
    void finish();

    const audio::MusicAnalysis& m_A;
    const GenOptions& m_opt;
    DesignPlan& m_plan;
    const StyleModel* m_style;
    Rng m_rng;
    Rng m_baseRng;
    level::Level m_level;
    phys::World m_world;
    bot::InputTrack m_inputs;
    phys::GameState m_cursor;
    std::vector<phys::GameState> m_partStart;   // state at the start of each part
    std::vector<Mark> m_partMark;
    std::vector<ClickResult> m_clicks;
    std::vector<HistoryStep> m_history;
    std::vector<PartResult> m_parts;
    PatternMemory m_memory;
    double m_endTime = 0;
    int m_nextGroup = 1000;
    mutable std::vector<DifficultyProfile> m_profiles;
};

// True if the gameplay object's simulated hitbox, grown by `margin`, touches none of the samples.
bool clearOf(const level::LevelObject& o, const std::vector<PathSample>& samples, float margin);

// Gamemode builders (implemented in modes_*.cpp). Each builds geometry + inputs for the clicks of
// one part, verifying every decision with the simulator. Return false if the part could not be
// built fairly (the caller retries, then falls back).
bool buildGroundPart(Builder& b, const Part& part, int attempt);   // cube, robot
bool buildFlyingPart(Builder& b, const Part& part, int attempt);   // ship, ufo, wave, swing
bool buildSurfacePart(Builder& b, const Part& part, int attempt);  // ball, spider

} // namespace al::gen
