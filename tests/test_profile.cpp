// Style profile (generator v2): extraction from a level and a run that beats it, JSON round
// trip, and the generator following a learned preference.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/profile.hpp>

#include <cmath>
#include <memory>

using namespace al;
using namespace al::gen;

namespace {
const audio::MusicAnalysis& song() {
    static const audio::MusicAnalysis A = audio::analyze(audio::render(audio::randomSong(321)));
    return A;
}
} // namespace

TEST(profile_from_a_generated_level) {
    GenOptions opt;
    opt.difficulty = Difficulty::Insane;
    opt.seed = 11;
    auto r = generateVariant(song(), opt, 11, nullptr);
    REQUIRE(r.report.beatable && r.report.intendedInputs);
    StyleProfile p = profileFromRun(r.level, r.inputs);
    const int used = r.report.clicks - r.report.dropped;
    std::printf("    %s; designed clicks %d, segments %zu\n", p.summary().c_str(), used, r.plan.parts.size());
    // every designed click does something the profile can name
    CHECK_MSG(std::fabs(p.clicks - used) <= used * 0.05 + 1, "profile clicks %.0f vs %d", p.clicks, used);
    CHECK(p.segments >= 0.8 * double(r.plan.parts.size()) && p.segments <= 1.2 * double(r.plan.parts.size()) + 1);
    // the orbs it used are counted by colour
    int orbs = 0;
    for (auto& o : r.level.objects) if (o.info().kind == level::Kind::Orb) ++orbs;
    double countedOrbs = 0;
    for (auto& m : p.actions) for (auto& [k, v] : m) if (k.rfind("orb-", 0) == 0) countedOrbs += v;
    CHECK(countedOrbs >= 0.9 * orbs);
    // JSON round trip
    auto back = StyleProfile::fromJson(p.toJson());
    REQUIRE(back.has_value());
    CHECK(back->clicks == p.clicks && back->segments == p.segments);
    CHECK(back->actions[0] == p.actions[0]);
}

TEST(profile_steers_the_generator) {
    // a profile where cube clicks are almost always red orbs
    auto prof = std::make_shared<StyleProfile>();
    prof->levels = 1;
    prof->actions[0]["orb-red"] = 200;
    prof->actions[0]["jump"] = 20;
    prof->actions[0]["orb-yellow"] = 2;
    GenOptions opt;
    opt.difficulty = Difficulty::Harder;
    opt.seed = 5;
    opt.modeWeights.fill(0.0f);
    opt.modeWeights[0] = 1.0f; // cube only
    auto count = [&](const GenResult& r, int id) {
        int n = 0;
        for (auto& o : r.level.objects) n += o.id == id;
        return n;
    };
    auto plain = generateVariant(song(), opt, 5, nullptr);
    opt.profile = prof;
    auto styled = generateVariant(song(), opt, 5, nullptr);
    std::printf("    red orbs: %d without the profile, %d with it; yellow: %d -> %d\n", count(plain, level::id::OrbRed), count(styled, level::id::OrbRed),
                count(plain, level::id::OrbYellow), count(styled, level::id::OrbYellow));
    CHECK(styled.report.beatable && styled.report.intendedInputs);
    CHECK(count(styled, level::id::OrbRed) > count(plain, level::id::OrbRed));
    CHECK(count(styled, level::id::OrbYellow) < count(plain, level::id::OrbYellow));
}
