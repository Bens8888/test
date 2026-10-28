#pragma once
// Pattern library and pattern choice. Every gameplay pattern the builders can stamp has an id,
// a gamemode, tags and a difficulty range. A StyleModel scores the valid candidates for a
// musical context; the builders then verify the chosen candidate with the simulator (rules and the
// bot guarantee correctness, the model only decides which valid pattern feels right).
#include <al/gen/options.hpp>
#include <al/gen/plan.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace al::gen {

enum PatternTag : uint32_t {
    TagSpike = 1u << 0,
    TagBlock = 1u << 1,
    TagGap = 1u << 2,
    TagOrb = 1u << 3,
    TagPad = 1u << 4,
    TagStep = 1u << 5,
    TagSaw = 1u << 6,
    TagCorridor = 1u << 7,
    TagTight = 1u << 8,
    TagFlow = 1u << 9,
    TagClassic = 1u << 10,   // fits retro style
    TagModern = 1u << 11,
    TagGravity = 1u << 12,
    TagMulti = 1u << 13,     // multi-object / complex
    TagCalm = 1u << 14,      // suits calm moments
    TagIntense = 1u << 15,   // suits intense moments
    TagDeco = 1u << 16,      // looks decorated
};

struct PatternDef {
    std::string id;
    Mode mode;
    uint32_t tags;
    float minDiff, maxDiff;  // usable difficulty range (0..9)
    float baseWeight;
    float complexity;        // rough difficulty contribution (0..5)
    int a = 0, b = 0, c = 0; // family parameters (meaning depends on the gamemode builder)
};

// The full library (hand-authored families x parameters).
const std::vector<PatternDef>& patternLibrary();
const PatternDef* findPattern(const std::string& id);
// Token = index of the pattern in the library (the model vocabulary), -1 if unknown.
int patternToken(const PatternDef* d);
int patternToken(const std::string& id);
int vocabSize();
std::vector<const PatternDef*> patternsFor(Mode m, float difficulty);

// Feature vector of a context (shared by the rule model, the trained model and training code).
constexpr int kContextFeatures = 24;

// One earlier decision of the level being built: its context features and the chosen pattern.
struct HistoryStep {
    float f[kContextFeatures];
    int token;
};

struct PatternContext {
    Mode mode = Mode::Cube;
    float intensity = 0.5f;
    float difficulty = 4;
    Intent intent = Intent::Groove;
    Role role = Role::Beat;
    float strength = 0.5f;
    double gapPrev = 1, gapNext = 1;  // seconds to the previous / next click
    int speed = 1;
    bool mini = false;
    Style style = Style::Modern;
    float pitchDelta = 0;             // melody movement since the previous click (semitones)
    bool hold = false;
    uint32_t motifKey = 0;            // hash of the local musical context (for motif repetition)
    std::string prevPattern, prevPattern2;
    float density = -1;               // user density option (-1 = auto)
    const std::vector<HistoryStep>* history = nullptr; // earlier decisions (oldest first)
};

void contextFeatures(const PatternContext& c, float out[kContextFeatures]);

class StyleModel {
public:
    virtual ~StyleModel() = default;
    // Fill `weights` (same size as `candidates`) with non-negative preference scores.
    virtual void score(const PatternContext& ctx, const std::vector<const PatternDef*>& candidates, std::vector<float>& weights) const = 0;
    virtual std::string name() const = 0;
};

// Hand-tuned weights: style/intent/intensity affinities, variety and motif memory.
class RuleStyleModel final : public StyleModel {
public:
    void score(const PatternContext& ctx, const std::vector<const PatternDef*>& candidates, std::vector<float>& weights) const override;
    std::string name() const override { return "rules"; }
};

// Context key for motif memory: quantized rhythm + role + melodic direction.
uint32_t motifKey(double gapPrev, double gapNext, Role role, float pitchDelta, bool downbeat);

} // namespace al::gen
