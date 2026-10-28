#pragma once
// Fusion of a click recording with the music: the musical role of every click, the player's
// timing bias, rhythmic figures (taps, holds, doubles, streams), repeating motifs, the recorded
// trajectory, and a design plan built around the clicks. The "faithfulness" option steers how
// strictly the plan keeps the recorded clicks (1 = exactly, 0 = free to snap, thin and fill in).
#include <al/audio/analysis.hpp>
#include <al/gen/options.hpp>
#include <al/gen/plan.hpp>
#include <al/rec/recording.hpp>
#include <al/util/rng.hpp>

#include <string>
#include <vector>

namespace al::gen {

enum class Figure : uint8_t { Tap, Hold, Double, Stream };
const char* figureName(Figure f);

struct ClickInfo {
    double time = 0;          // press (song seconds, latency-corrected)
    double rawTime = 0;       // as recorded
    double release = 0;
    bool hold = false;
    Figure figure = Figure::Tap;
    Role role = Role::Recorded;
    float strength = 0;       // salience of the matched musical event (0 = unmatched)
    double musicTime = -1;    // matched musical event time (-1 = none)
    float offsetMs = 0;       // click - matched event (positive = late)
    float pitch = 0;
    int motif = -1, motifPos = -1;
    bool hasState = false;
    rec::PlayerSnap state;
};

struct Motif {
    int id = 0;
    std::vector<int> pattern;     // click positions inside the bar, in 16ths
    std::vector<double> barStarts;
};

struct TrajectoryPoint {
    double time = 0;
    float x = 0, y = 0;
    Mode mode = Mode::Cube;
    int speed = 1;
    bool mini = false, upsideDown = false;
};

struct ClickFusion {
    std::vector<ClickInfo> clicks;
    std::vector<Motif> motifs;
    std::vector<TrajectoryPoint> trajectory;  // the player's state at every recorded event
    double latencyMs = 0;     // median timing bias of the player (positive = late), removed
    float matchedFraction = 0;// clicks within 70 ms of a musical event
    float meanAbsOffsetMs = 0;
    int holds = 0, doubles = 0, streams = 0;
    bool hasModes = false;    // the recording contains gamemode information worth following
    std::string summary() const;
};

// Analyse a recording against the music (player 1's jump button).
ClickFusion fuseClicks(const rec::Recording& rec, const audio::MusicAnalysis& A);

// Where a level of this recording should start in the song: a little before the first click when
// that comes late (a take of the middle of the song), else 0. The level gets this as its song
// offset; analyse with audio::trimStart and shift the recording by it (shiftRecording).
double levelStartFor(const rec::Recording& r);
// The recording with its times relative to a level starting at `songStart` of the song.
rec::Recording shiftRecording(const rec::Recording& r, double songStart);

// Design plan from the fused clicks: parts and gamemodes from the music (gamemodes that suit the
// recorded rhythm, or the recorded gamemodes when following strictly), clicks from the recording
// steered by opt.faithfulness.
DesignPlan planFromRecording(const ClickFusion& f, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng);

} // namespace al::gen
