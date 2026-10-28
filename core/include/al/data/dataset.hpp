#pragma once
// Training data for the pattern model. A sample is a sequence of pattern decisions: per decision
// the 24 context features (music, click, gamemode state) and the pattern token chosen, plus the
// conditioning (style, difficulty, density). Sources:
//  - generated levels (self-play): the generator's own decisions, kept when the bot-verified
//    level scores well (rejection sampling);
//  - real levels (.gmd + their song): the bot solves the level, every jump's surrounding geometry
//    is classified back into the closest pattern token (style learning from human levels).
#include <al/audio/analysis.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/style.hpp>
#include <al/level/level.hpp>
#include <al/util/json.hpp>

#include <array>
#include <string>
#include <vector>

namespace al::data {

struct Step {
    std::array<float, gen::kContextFeatures> f{};
    int token = -1;
    double time = 0;
};

struct Sequence {
    std::string source;        // "selfplay", "level:<name>"
    int style = 0;             // gen::Style
    float difficulty = 4;      // 0..9
    float density = -1;        // -1 = auto
    float reward = 0;          // self-play score (0 for real levels)
    std::vector<Step> steps;

    Json toJson() const;
    static Sequence fromJson(const Json& j);
};

// Reward of a generated level for rejection sampling: bot-verified, fair windows, clicks that
// matter, music alignment, difficulty following the music, variety. Unbeatable = -inf.
float reward(const gen::GenResult& r);

// The decisions of a generated level.
Sequence fromGenerated(const gen::GenResult& r, const gen::GenOptions& opt);

// Real levels -------------------------------------------------------------------------------

struct ExtractOptions {
    int style = 0;               // style label of the level (0 = unknown / auto)
    float difficulty = -1;       // 0..9, <0 = estimate from the click rate
    int solveBeam = 256;
    int maxSeconds = 600;
};

struct ExtractReport {
    bool ok = false;
    std::string reason;          // why the level was rejected
    int presses = 0;
    int labelled = 0;            // decisions mapped to a pattern token
    int unlabelled = 0;          // jumps whose geometry matched no pattern
    double seconds = 0;
    std::string summary() const;
};

// Filters (broken / unsuitable levels are rejected with a reason), solves the level with the
// bot (or uses `solution` when given), classifies each decision.
Sequence fromLevel(const level::Level& lvl, const audio::MusicAnalysis& A, const ExtractOptions& opt, ExtractReport& rep,
                   const bot::InputTrack* solution = nullptr);

// The pattern token closest to the geometry of one ground-mode jump (cube/robot), -1 if none.
int classifyGroundJump(const phys::World& W, const std::vector<phys::PlayerState>& trace, const std::vector<uint32_t>& events,
                       int pressTick, int endTick, int* orbObjectId = nullptr);

// JSON Lines helpers.
bool appendJsonl(const std::string& path, const std::vector<Sequence>& seqs);
std::vector<Sequence> readJsonl(const std::string& path);

} // namespace al::data
