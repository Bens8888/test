#include "test.hpp"

#include <al/bot/bot.hpp>
#include <al/level/level.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

using namespace al;
using level::Level;
using level::LevelObject;
using level::Mode;

static LevelObject o(int id, double x, double y, double rot = 0) {
    LevelObject ob;
    ob.id = id;
    ob.x = x;
    ob.y = y;
    ob.rotation = rot;
    return ob;
}

// Hand-built test level: spikes, a block staircase, an orb gap, a ship part and a cube exit.
static Level testLevel() {
    Level L;
    L.settings.fixGravityBug = true;
    auto& v = L.objects;
    v.push_back(o(level::id::Spike, 405, 15));
    v.push_back(o(level::id::Spike, 705, 15));
    v.push_back(o(level::id::Spike, 735, 15));
    for (int i = 0; i < 4; ++i) v.push_back(o(level::id::Block, 1005 + 30 * i, 15));
    for (int i = 0; i < 4; ++i) v.push_back(o(level::id::Block, 1215 + 30 * i, 45));
    for (int i = 0; i < 3; ++i) v.push_back(o(level::id::Block, 1215 + 30 * i, 15));
    v.push_back(o(level::id::Block, 1305, 15));
    // spike pit with an orb
    for (int i = 0; i < 6; ++i) v.push_back(o(level::id::Spike, 1395 + 30 * i, 15));
    v.push_back(o(level::id::OrbYellow, 1470, 75));
    // ship section
    v.push_back(o(level::id::PortalShip, 1800, 45));
    for (int i = 0; i < 20; ++i) {
        double x = 1950 + 60 * i;
        double gapCenter = 150 + 60 * std::sin(i * 0.5);
        v.push_back(o(level::id::Block, x, gapCenter - 75));
        v.push_back(o(level::id::Block, x, gapCenter + 75));
    }
    v.push_back(o(level::id::PortalCube, 3300, 45));
    v.push_back(o(level::id::Spike, 3600, 15));
    return L;
}

TEST(bot_autosolve_finds_verified_solution) {
    Level L = testLevel();
    phys::World W(L);
    bot::SolveOptions so;
    auto res = bot::autoSolve(W, so);
    std::printf("    solved=%d inputs=%zu expanded=%d in %.3f s (bestX %.0f, end %.0f)\n", res.solved, res.inputs.events.size(), res.expanded,
                res.seconds, res.bestX, W.endX());
    REQUIRE(res.solved);
    CHECK(bot::isBeatable(W, res.inputs));
    auto f = bot::firstFailure(W, res.inputs);
    CHECK(!f.failed);
    // removing all inputs must fail (the level requires clicks)
    bot::InputTrack none;
    auto f2 = bot::firstFailure(W, none);
    CHECK(f2.failed);
    CHECK(f2.cause == phys::DeathCause::Hazard);
    CHECK_NEAR(f2.x, 405 - 15 - 3, 4.0);
}

TEST(bot_timing_windows_reasonable) {
    Level L;
    L.objects.push_back(o(level::id::Spike, 405, 15));
    L.objects.push_back(o(level::id::Spike, 1005, 15));
    L.objects.push_back(o(level::id::Spike, 1035, 15));
    phys::World W(L);
    auto res = bot::autoSolve(W);
    REQUIRE(res.solved);
    auto tw = bot::timingWindows(W, res.inputs);
    REQUIRE(!tw.empty());
    for (auto& w : tw) {
        if (!w.press) continue;
        std::printf("    press @%d x=%.0f window -%d/+%d ticks (%.0f ms)\n", w.tick, w.x, w.early, w.late, w.windowMs());
        CHECK(w.windowMs() > 20);
    }
}

TEST(bot_rollout_throughput) {
    Level L = testLevel();
    phys::World W(L);
    auto res = bot::autoSolve(W);
    REQUIRE(res.solved);
    // A "rollout" here = replaying the full solution (about 12 s of gameplay, ~2900 ticks).
    auto t0 = std::chrono::steady_clock::now();
    int n = 0, ticks = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.5) {
        auto r = bot::run(W, res.inputs);
        ticks += r.ticks;
        ++n;
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double stepsPerSec = ticks / sec;
    std::printf("    %.0f full-level rollouts/s, %.1f M physics steps/s; 5-second section rollouts: %.0f/s\n", n / sec, stepsPerSec / 1e6,
                stepsPerSec / 1200.0);
    CHECK(stepsPerSec / 1200.0 > 1000); // thousands of 5-second rollouts per second
}
