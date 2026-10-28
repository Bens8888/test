#pragma once
// Generator options. Every option has an "Auto" value meaning "let the design brain decide";
// the menu only steers the generator, it never replaces its decisions.
#include <al/level/objects.hpp>
#include <al/util/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace al::gen {

using level::Mode;
constexpr int kModeCount = int(Mode::Count);

enum class Difficulty : uint8_t {
    Easy = 0, Normal, Hard, Harder, Insane, EasyDemon, MediumDemon, HardDemon, InsaneDemon, ExtremeDemon, Auto = 255
};
const char* difficultyName(Difficulty d);

enum class Style : uint8_t { Auto = 0, Flow, Tight, Chaotic, Minimal, Retro, Modern, Memory, Count };
const char* styleName(Style s);

enum class SpeedChanges : uint8_t { Auto = 0, Off, Subtle, Aggressive };
enum class Tri : uint8_t { Auto = 0, Off, On };
enum class InputSource : uint8_t { Music = 0, Recording };

// Per-music-section steering ("chorus hard, verse easy").
struct SectionOverride {
    int sectionType = -1;          // audio::SectionType value, or -1 = none
    int difficultyDelta = 0;       // added to the level difficulty (-3..+3)
    int forcedMode = -1;           // Mode value or -1 = let the AI decide
};

struct GenOptions {
    Difficulty difficulty = Difficulty::Auto;
    float density = -1;                         // 0..1, <0 = auto
    Style style = Style::Auto;
    std::array<float, kModeCount> modeWeights;  // 0..1 per gamemode, <0 = auto
    SpeedChanges speedChanges = SpeedChanges::Auto;
    int startSpeed = -1;                        // speed index, -1 = auto
    float portalFrequency = -1;                 // 0..1, <0 = auto
    Tri gravityFlips = Tri::Auto;
    Tri sizeChanges = Tri::Auto;
    Tri dual = Tri::Auto;
    float holdBias = 0;                         // -1 tap-heavy .. +1 hold-heavy (0 = balanced/auto)
    std::vector<SectionOverride> sectionOverrides;
    uint64_t seed = 1;
    int variants = 5;
    InputSource input = InputSource::Music;
    float faithfulness = 0.8f;                  // recording mode: 1 = strict to the clicks, 0 = creative
    int maxRetriesPerSection = 24;

    GenOptions() { modeWeights.fill(-1.0f); }

    Json toJson() const;
    static GenOptions fromJson(const Json& j);
};

// Concrete tuning derived from a difficulty level.
struct DifficultyProfile {
    float clicksCalm;        // clicks per second in calm parts
    float clicksIntense;     // clicks per second in the most intense parts
    float minWindowMs;       // tightest allowed timing window for a click
    float clearance;         // flying modes: free space above + below the path (units, beyond the hitbox)
    float minClickGap;       // seconds; shortest gap between two clicks
    int maxSpikes;           // most spikes in one cube jump
    bool orbs;               // air-jump orbs allowed
    bool tripleSpikes;
    float speedBias;         // how readily faster speeds are used
};
const DifficultyProfile& profile(Difficulty d);
// Continuous difficulty (0..9) -> profile with linear interpolation of the numbers.
DifficultyProfile profileAt(float d);

} // namespace al::gen
