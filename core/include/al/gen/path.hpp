#pragma once
// Generator v2, step 2: the gameplay path. The clicks are walked through with the physics
// simulator; at every click the planner chooses what the click does (a jump, an orb, a tap or a
// hold, a ball/spider flip) and between clicks what keeps the player moving (a platform to land
// on, a pad, a gravity portal, the portals of the next gamemode segment). A small beam search
// keeps the best few partial paths. Structures are built around the result afterwards (v2 step 3).
#include <al/audio/analysis.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/options.hpp>
#include <al/gen/plan.hpp>
#include <al/gen/tiles.hpp>
#include <al/level/level.hpp>
#include <al/physics/sim.hpp>
#include <al/util/progress.hpp>
#include <al/util/rng.hpp>

#include <string>
#include <vector>

namespace al::gen {

enum class Act : uint8_t {
    None = 0,
    Jump,        // cube/robot jump, ball flip, spider teleport from a surface
    Tap,         // flying modes: a short press
    Hold,        // flying modes / robot: a press held for a while
    Orb,         // a click on an orb (objId says which)
    // events without a click
    Land,        // a platform to land on
    Pad,
    GravityPortal,
    ModePortal,  // the start of a new gamemode segment (with its speed/size/gravity portals)
};
const char* actName(Act a);

// One gamemode stretch of the song.
struct PathSegment {
    int first = 0, last = 0;    // click indices (inclusive)
    Mode mode = Mode::Cube;
    int speed = 1;              // speed index
    bool mini = false;
    bool flipped = false;       // gravity at the segment start
    float intensity = 0.5f;
    std::string why;
};

// A platform the player stands on: cells [cx0, cx1] x [cy0, cy1]. g = +1: the player runs on the
// top row; g = -1: the player runs under the bottom row (upside down).
struct PathSurface {
    int cx0 = 0, cx1 = 0, cy0 = 0, cy1 = 0;
    int g = 1;
    int click = -1;             // the click it was built for
};

struct PathObject {
    level::LevelObject o;
    Act act = Act::None;
    int click = -1;
};

struct PathClick {
    int click = -1;             // index into DesignPlan::clicks
    int tick = 0;
    int releaseTick = 0;
    Act act = Act::None;
    int objId = 0;              // the orb / pad id for Orb clicks
    Mode mode = Mode::Cube;
    float x = 0, y = 0;         // player position at the press
    bool grounded = false;
    int segment = 0;
};

// The player's position at every tick of the designed run (compact).
struct TraceSample {
    float x = 0, y = 0;
    float half = 15;            // half hitbox size
    Mode mode = Mode::Cube;
    int8_t g = 1;
    bool onGround = false;
    bool pressed = false;
};

struct PathParams {
    int beamWidth = 12;
    float switchRate = -1;      // 0..1 how often the gamemode changes (<0 = from the style and music)
};

struct PathPlan {
    level::LevelSettings start;
    std::vector<PathSegment> segments;
    std::vector<PathObject> objects;   // orbs, pads, portals
    std::vector<PathSurface> surfaces; // platforms
    bot::InputTrack inputs;
    std::vector<PathClick> clicks;     // one per designed click that was used
    std::vector<int> skipped;          // clicks that could not be used
    std::vector<TraceSample> trace;     // tick by tick
    double score = 0;
    int endTick = 0;
    std::vector<std::string> notes;
};

// Clicks from the music for generator v2: chosen per music part without a gamemode (v2 decides
// the gamemodes from the clicks afterwards).
DesignPlan planFromMusicV2(const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng);

// Gamemode segments for the clicks of the plan.
std::vector<PathSegment> planSegments(const DesignPlan& plan, const audio::MusicAnalysis& A, const GenOptions& opt, float switchRate, Rng& rng);

// The gameplay path for the plan's clicks.
PathPlan planPath(const DesignPlan& plan, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng,
                  const PathParams& prm = {}, const Progress* progress = nullptr);

// The path as a level: its objects plus the platforms (solid cells added to `cells`).
level::Level pathLevel(const PathPlan& path, CellGrid* cells = nullptr);

} // namespace al::gen
