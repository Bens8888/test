// Dataset tests: self-play sequences carry the generator's decisions; the real-level labeller,
// run on generated levels (where the true pattern of every jump is known), recovers them; JSONL
// round trip; filters reject broken levels.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/data/dataset.hpp>
#include <al/gen/generator.hpp>

#include <cstdio>
#include <map>

using namespace al;

namespace {
const audio::MusicAnalysis& songA() {
    static const audio::MusicAnalysis A = audio::analyze(audio::render(audio::randomSong(11)));
    return A;
}
} // namespace

TEST(dataset_selfplay_sequence) {
    const auto& A = songA();
    gen::GenOptions opt;
    opt.difficulty = gen::Difficulty::Harder;
    opt.seed = 4;
    auto r = gen::generateVariant(A, opt, 4, nullptr);
    auto seq = data::fromGenerated(r, opt);
    int decisions = 0;
    for (auto& c : r.clicks) decisions += c.decision && !c.dropped ? 1 : 0;
    CHECK(int(seq.steps.size()) == decisions);
    CHECK(seq.steps.size() > 20);
    CHECK(seq.reward > -1e8f);
    for (auto& s : seq.steps) {
        CHECK(s.token >= 0 && s.token < gen::vocabSize());
        if (s.token < 0) break;
    }
    // JSONL round trip
    std::string path = "dataset_test.jsonl";
    std::remove(path.c_str());
    REQUIRE(data::appendJsonl(path, {seq, seq}));
    auto back = data::readJsonl(path);
    std::remove(path.c_str());
    REQUIRE(back.size() == 2);
    REQUIRE(back[0].steps.size() == seq.steps.size());
    CHECK(back[1].steps.back().token == seq.steps.back().token);
    CHECK_NEAR(back[0].steps[3].f[8], seq.steps[3].f[8], 1e-4);
}

TEST(dataset_labeller_recovers_generated_patterns) {
    // Exact agreement is measured on regular jumps; for orb jumps the pit width is ambiguous by a
    // cell (the pit sits at the no-click landing point), so orb tokens are compared as
    // "orb colour + pit / no pit".
    const auto& A = songA();
    int total = 0, labelled = 0, plain = 0, plainExact = 0, plainTerrain = 0, orbs = 0, orbClass = 0;
    std::map<std::string, int> confusions;
    auto coarse = [](const std::string& id) {
        auto plus = id.find('+');
        std::string terr = id.substr(plus + 1);
        return id.substr(0, plus) + (terr.rfind("gap", 0) == 0 ? "+pit" : "+" + terr);
    };
    for (int seed : {1, 2, 3}) {
        gen::GenOptions opt;
        opt.difficulty = gen::Difficulty(2 + seed);
        opt.seed = uint64_t(seed);
        opt.modeWeights.fill(0.0f);
        opt.modeWeights[0] = 1.0f; // cube
        opt.modeWeights[5] = 1.0f; // robot
        auto r = gen::generateVariant(A, opt, uint64_t(seed), nullptr);
        REQUIRE(r.report.intendedInputs);
        std::vector<std::pair<double, int>> truth;
        for (auto& c : r.clicks)
            if (c.decision && !c.dropped) truth.push_back({r.plan.clicks[size_t(c.click)].time, gen::patternToken(c.pattern)});
        data::ExtractOptions eo;
        data::ExtractReport rep;
        auto seq = data::fromLevel(r.level, A, eo, rep, &r.inputs);
        REQUIRE(rep.ok);
        labelled += rep.labelled;
        for (auto& st : seq.steps) {
            const std::pair<double, int>* best = nullptr;
            for (auto& tr : truth) if (!best || std::fabs(tr.first - st.time) < std::fabs(best->first - st.time)) best = &tr;
            if (!best || std::fabs(best->first - st.time) > 0.01) continue;
            ++total;
            const auto& got = gen::patternLibrary()[size_t(st.token)].id;
            const auto& want = gen::patternLibrary()[size_t(best->second)].id;
            if (want.find(":orb") != std::string::npos) {
                ++orbs;
                if (coarse(got) == coarse(want)) ++orbClass;
                else ++confusions[want + " -> " + got];
            } else {
                ++plain;
                if (got == want) ++plainExact;
                else ++confusions[want + " -> " + got];
                if (got.substr(got.find('+')) == want.substr(want.find('+'))) ++plainTerrain;
            }
        }
    }
    std::printf("    labelled %d decisions, %d matched to designed jumps: regular %d (exact %.0f%%, terrain %.0f%%), orb %d (class %.0f%%)\n",
                labelled, total, plain, 100.0 * plainExact / std::max(1, plain), 100.0 * plainTerrain / std::max(1, plain), orbs,
                100.0 * orbClass / std::max(1, orbs));
    std::vector<std::pair<int, std::string>> top;
    for (auto& [k, v] : confusions) top.push_back({v, k});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < std::min<size_t>(8, top.size()); ++i) std::printf("      %3d x %s\n", top[i].first, top[i].second.c_str());
    CHECK(total > 100);
    CHECK_MSG(plainExact * 100 >= plain * 70, "regular exact %d/%d", plainExact, plain);
    CHECK_MSG(plainTerrain * 100 >= plain * 85, "regular terrain %d/%d", plainTerrain, plain);
    CHECK_MSG(orbClass * 100 >= orbs * 80, "orb class %d/%d", orbClass, orbs);
}

TEST(dataset_filters_reject_broken_levels) {
    const auto& A = songA();
    level::Level L;
    data::ExtractReport rep;
    data::fromLevel(L, A, {}, rep);
    CHECK(!rep.ok);
    CHECK(rep.reason.find("too few") != std::string::npos);
    // an impossible level: a wall of spikes
    for (int i = 0; i < 40; ++i) {
        level::LevelObject o;
        o.id = level::id::Block;
        o.x = 600 + 15;
        o.y = 15 + 30 * i;
        L.objects.push_back(o);
    }
    data::fromLevel(L, A, {}, rep);
    CHECK(!rep.ok);
    CHECK_MSG(rep.reason.find("bot") != std::string::npos, "%s", rep.reason.c_str());
}
