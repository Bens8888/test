#pragma once
// Top-level generation: plan -> build -> verify -> report, for one or many variants.
#include <al/audio/analysis.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/builder.hpp>
#include <al/gen/options.hpp>
#include <al/gen/plan.hpp>
#include <al/gen/style.hpp>
#include <al/level/level.hpp>
#include <al/util/json.hpp>
#include <al/util/progress.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace al::gen {

struct Report {
    int clicks = 0, dropped = 0, forced = 0;
    double minWindowMs = 0, meanWindowMs = 0;
    struct Risky { double time; float x; double windowMs; std::string pattern; };
    std::vector<Risky> risky;
    double longestGap = 0, longestGapAt = 0;           // seconds without input (during gameplay)
    std::vector<std::pair<double, float>> difficultyCurve;  // (time, 0..1) per 2 s window
    std::vector<std::pair<double, float>> intensityCurve;   // music intensity at the same times
    float curveCorrelation = 0;                        // difficulty vs music intensity
    float musicAlignment = 0;                          // fraction of clicks on strong musical events
    int objects = 0, gameplayObjects = 0, portals = 0, triggers = 0;
    std::array<double, 8> modeSeconds{};
    bool beatable = false;
    bool intendedInputs = false;   // the level is beaten with the designed inputs (not a bot search)
    int fallbackParts = 0;
    bool failed = false; double failTime = 0; float failX = 0, failY = 0; std::string failCause; // where it fails if not beatable
    std::vector<std::string> notes;

    Json toJson() const;
    std::string toText() const;
};

struct GenResult {
    level::Level level;
    bot::InputTrack inputs;
    DesignPlan plan;
    std::vector<ClickResult> clicks;
    std::vector<PartResult> parts;
    Report report;
    float score = 0;
    uint64_t seed = 0;
    double seconds = 0;
    std::string model;
    std::vector<std::string> notes;
};

Report makeReport(const level::Level& lvl, const bot::InputTrack& inputs, const DesignPlan& plan, const std::vector<ClickResult>& clicks,
                  const std::vector<PartResult>& parts, const audio::MusicAnalysis& A, bool beatable);

// "Feel" score of a finished layout (higher is better); used to rank variants.
float feelScore(const GenResult& r, const audio::MusicAnalysis& A);

// One variant. `plan` (optional) fixes the design plan (recording mode, re-rolls); otherwise the
// plan is made from the music with the seed.
GenResult generateVariant(const audio::MusicAnalysis& A, const GenOptions& opt, uint64_t seed, const StyleModel* model,
                          const Progress* progress = nullptr, const DesignPlan* plan = nullptr);

// Generator v2 (path first, structures around it); generateVariant calls it when opt.engine == 2.
// `salts` (optional): one per gamemode segment, re-rolls a segment without changing the path before it.
// `lockSegment` + `baseSalts` (re-roll): the path before that segment stays exactly as baseSalts made it.
GenResult generateVariantV2(const audio::MusicAnalysis& A, const GenOptions& opt, uint64_t seed, const Progress* progress = nullptr,
                            const DesignPlan* plan = nullptr, const std::vector<uint64_t>* salts = nullptr, int lockSegment = -1,
                            const std::vector<uint64_t>* baseSalts = nullptr);

// N variants (opt.variants) in parallel, best first.
std::vector<GenResult> generateVariants(const audio::MusicAnalysis& A, const GenOptions& opt, const StyleModel* model,
                                        const Progress* progress = nullptr, int threads = 0, const DesignPlan* plan = nullptr);

// Re-roll a single part of an existing result (new seed for that part only).
GenResult rerollPart(const GenResult& base, int partIndex, const audio::MusicAnalysis& A, const GenOptions& opt, uint64_t seed,
                     const StyleModel* model);

} // namespace al::gen
