#pragma once
// The design plan: how the song is split into gameplay parts (gamemode, speed, size, gravity,
// difficulty, intent) and which musical events become clicks. Built from the music analysis
// (automatic mode) or from a recording (fused with the music, see fusion/).
#include <al/audio/analysis.hpp>
#include <al/gen/options.hpp>
#include <al/util/rng.hpp>

#include <functional>
#include <string>
#include <vector>

namespace al::gen {

enum class Role : uint8_t { Downbeat, Kick, Snare, HiHat, Melody, Sustain, Syncopation, Beat, Fill, Recorded };
const char* roleName(Role r);

enum class Intent : uint8_t { Intro, Groove, Build, Climax, Breather, Outro };
const char* intentName(Intent i);

struct Click {
    double time = 0;        // press time (s)
    double release = 0;     // release time (s); == time + short tap for taps
    Role role = Role::Beat;
    float strength = 0;     // musical salience 0..1
    float pitch = 0;        // MIDI pitch of the melody at the click (0 = none)
    bool hold = false;      // a deliberate hold (robot high jump, ship climb)
    bool recorded = false;  // came from a user recording
    int motif = -1;         // repeating rhythm motif id (recordings), -1 = none
    int motifPos = -1;      // position of the click inside its motif
    int part = -1;
};

struct Part {
    int index = 0;
    double t0 = 0, t1 = 0;  // seconds
    Mode mode = Mode::Cube;
    int speed = 1;          // speed index
    bool mini = false;
    bool flipped = false;   // gravity at part start
    bool dual = false;
    float intensity = 0.5f; // 0..1
    float difficulty = 4;   // continuous 0..9
    Intent intent = Intent::Groove;
    int section = -1;       // music section index
    std::vector<int> clicks; // indices into DesignPlan::clicks
    uint64_t salt = 0;       // per-part random stream (changed by "re-roll this section")
    std::string why;        // human-readable reason for the choices
};

// What the player does, what the music does and the intent, for each click (shown in the UI).
struct Moment {
    double time = 0;
    std::string action;
    std::string music;
    float intensity = 0;
    std::string intent;
};

struct DesignPlan {
    std::vector<Part> parts;
    std::vector<Click> clicks;
    double duration = 0;
    float baseDifficulty = 4;
    Style style = Style::Modern;
    int baseSpeed = 1;
    // generator v2: `parts` are the gamemode segments; the music's own parts (intent, intensity,
    // difficulty per section) are kept here
    std::vector<Part> music;

    std::vector<Moment> moments() const;
};

// The design intent of a music section type.
Intent intentFor(audio::SectionType t);

// Resolve "Auto" style/difficulty against the music.
float resolveDifficulty(const audio::MusicAnalysis& A, const GenOptions& opt);
Style resolveStyle(const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng);

// Extra gamemode preference for a part (e.g. how well a mode fits recorded clicks), multiplied
// into the music-based weights.
using ModeFit = std::function<float(const Part& part, Mode mode)>;

// Parts (structure, gamemodes, speeds) from the music sections.
std::vector<Part> planParts(const audio::MusicAnalysis& A, const GenOptions& opt, float baseDifficulty, Style style, Rng& rng,
                            int* baseSpeedOut = nullptr, const ModeFit* fit = nullptr);

// Automatic click track for one part from the music timeline.
std::vector<Click> selectClicks(const audio::MusicAnalysis& A, const Part& part, const GenOptions& opt, Style style, Rng& rng);

// Full automatic plan.
DesignPlan planFromMusic(const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng);

// Re-plan a single part (re-roll one section) keeping the others.
void replanPart(DesignPlan& plan, int partIndex, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng);

// Musical click candidates in [t0, t1): every analysed event with a salience, merged within 35 ms.
struct Candidate {
    double t;
    float sal;
    Role role;
    float pitch;
    double sustain; // > 0 if a sustained note starts here
};
std::vector<Candidate> gatherCandidates(const audio::MusicAnalysis& A, double t0, double t1, float holdBias);
// Shortest gap between two clicks a gamemode can express fairly at a difficulty.
double minGapFor(Mode mode, int speed, bool mini, const DifficultyProfile& prof);

// Typical airtime (s) of a cube jump at a speed (used to decide when orbs are needed).
double cubeAirtime(int speed, bool mini);

} // namespace al::gen
