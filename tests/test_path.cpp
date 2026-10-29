// Generator v2 path planner: every click is used or reported, the designed inputs beat the path,
// and planning agrees with a run over the whole path.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/path.hpp>

#include <algorithm>
#include <cstdlib>

using namespace al;
using namespace al::gen;

TEST(path_planner_uses_the_clicks_and_is_beatable) {
    const int songs = std::getenv("AL_PATH_N") ? std::max(1, std::atoi(std::getenv("AL_PATH_N"))) : 6;
    int clicks = 0, used = 0, levels = 0;
    for (int i = 0; i < songs; ++i) {
        auto A = audio::analyze(audio::render(audio::randomSong(uint64_t(500 + i * 17))));
        GenOptions opt;
        opt.seed = uint64_t(i + 1);
        opt.difficulty = Difficulty(i % 10);
        Rng rng(opt.seed);
        DesignPlan plan = planFromMusicV2(A, opt, rng);
        PathPlan path = planPath(plan, A, opt, rng);
        CHECK_MSG(path.clicks.size() + path.skipped.size() == plan.clicks.size(), "song %d: %zu used + %zu skipped of %zu", i, path.clicks.size(), path.skipped.size(), plan.clicks.size());
        for (auto& n : path.notes) {
            const bool bad = n.find("differ") != std::string::npos || n.find("dies") != std::string::npos;
            CHECK_MSG(!bad, "song %d: %s", i, n.c_str());
        }
        level::Level L = pathLevel(path);
        phys::World W(L);
        CHECK_MSG(bot::isBeatable(W, path.inputs), "song %d: the designed inputs do not beat the path", i);
        // every used click presses at its tick (a wave release click lets go of the press before it)
        for (auto& c : path.clicks) {
            if (c.act == Act::Release) CHECK(path.inputs.heldAt(c.tick - 1) && !path.inputs.heldAt(c.tick));
            else CHECK(path.inputs.heldAt(c.tick));
        }
        clicks += int(plan.clicks.size());
        used += int(path.clicks.size());
        ++levels;
    }
    std::printf("    %d songs, %d of %d clicks used\n", levels, used, clicks);
    CHECK(used * 100 >= clicks * 95);
}
