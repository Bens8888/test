#include "test.hpp"

#include <algorithm>
#include <al/level/level.hpp>
#include <al/physics/sim.hpp>

#include <cmath>
#include <cstring>

using namespace al;
using namespace al::phys;
using level::Level;
using level::LevelObject;
using level::Mode;

static Level emptyLevel(Mode m = Mode::Cube, int speed = 1, bool mini = false) {
    Level L;
    L.settings.startMode = m;
    L.settings.startSpeed = speed;
    L.settings.startMini = mini;
    L.settings.fixGravityBug = true;
    // far-away marker so the level is long
    LevelObject end;
    end.id = level::id::TrigEnd;
    end.x = 100000;
    end.y = 300;
    L.objects.push_back(end);
    return L;
}

static LevelObject obj(int id, double x, double y, double rot = 0) {
    LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

struct JumpStats { float apex; int airTicks; float distance; };

// Press on the ground at tick 1, release at tick 2, measure until landing.
static JumpStats measureJump(Mode m, int speed, bool mini, int holdTicks = 1) {
    Level L = emptyLevel(m, speed, mini);
    World W(L);
    GameState g = initialState(W);
    for (int i = 0; i < 5; ++i) step(W, g, false); // settle on the ground
    float y0 = g.p[0].y, x0 = g.p[0].x;
    float apex = y0;
    int ticks = 0;
    bool left = false;
    for (int t = 0; t < 2000; ++t) {
        step(W, g, t < holdTicks);
        apex = std::max(apex, g.p[0].y);
        ++ticks;
        if (g.p[0].y > y0 + 0.01f) left = true;
        if (left && (g.p[0].events & EvLand)) break;
    }
    return {apex - y0, ticks, g.p[0].x - x0};
}

TEST(physics_speeds_units_per_second) {
    CHECK_NEAR(unitsPerSecond(0), 251.16, 0.01);
    CHECK_NEAR(unitsPerSecond(1), 311.58, 0.01);
    CHECK_NEAR(unitsPerSecond(2), 387.42, 0.01);
    CHECK_NEAR(unitsPerSecond(3), 468.00, 0.01);
    CHECK_NEAR(unitsPerSecond(4), 576.00, 0.01);
    // and the simulated x after one second
    Level L = emptyLevel();
    World W(L);
    GameState g = initialState(W);
    for (int i = 0; i < 240; ++i) step(W, g, false);
    CHECK_NEAR(g.p[0].x, 311.58, 0.05);
}

TEST(physics_cube_jump_matches_spec) {
    // Fresh-press model (press-time jump check + one gravity step), spec §16.1
    auto j = measureJump(Mode::Cube, 1, false);
    CHECK_NEAR(j.apex, 63.85, 0.6);
    CHECK_NEAR(j.airTicks, 103, 1);
    CHECK_NEAR(j.distance / 30.0, 4.46, 0.06);
    auto j2 = measureJump(Mode::Cube, 2, false);
    CHECK_NEAR(j2.apex, 66.96, 0.6);
    CHECK_NEAR(j2.airTicks, 106, 1);
    auto jm = measureJump(Mode::Cube, 1, true);
    CHECK_NEAR(jm.apex, 40.66, 0.6);
    CHECK_NEAR(jm.airTicks, 82, 1);
}

TEST(physics_cube_held_rejump_no_gravity_tick) {
    // Holding through a landing re-jumps with no gravity on the jump tick: apex 66.36 (§16.1)
    Level L = emptyLevel();
    World W(L);
    GameState g = initialState(W);
    for (int i = 0; i < 5; ++i) step(W, g, false);
    int lands = 0;
    float y0 = g.p[0].y, apex = 0;
    int ticksSecond = 0;
    for (int t = 0; t < 600 && lands < 2; ++t) {
        step(W, g, true);
        if (lands == 1) { apex = std::max(apex, g.p[0].y - y0); ++ticksSecond; }
        if (g.p[0].events & EvLand) ++lands;
    }
    CHECK_NEAR(apex, 66.36, 0.6);
    CHECK_NEAR(ticksSecond, 105, 1);
}

TEST(physics_ship_acceleration_and_caps) {
    Level L = emptyLevel(Mode::Ship);
    World W(L);
    GameState g = initialState(W);
    step(W, g, false);
    int ticks = 0;
    while (g.p[0].v < 7.999 && ticks < 400) { step(W, g, true); ++ticks; }
    CHECK_NEAR(ticks, 89, 3);
    for (int i = 0; i < 50; ++i) step(W, g, true);
    CHECK_NEAR(g.p[0].v, 8.0, 1e-9);
    for (int i = 0; i < 400; ++i) step(W, g, false);
    // after hitting the floor velocity is reset, so test the cap mid-air with a high start
    Level L2 = emptyLevel(Mode::Ship);
    World W2(L2);
    GameState h = initialState(W2);
    for (int i = 0; i < 150; ++i) step(W2, h, true); // climb to the ceiling area
    double minV = 0;
    for (int i = 0; i < 200; ++i) { step(W2, h, false); minV = std::min(minV, h.p[0].v); }
    CHECK_NEAR(minV, -6.4, 1e-9);
    CHECK(!h.dead);
}

TEST(physics_ufo_hop) {
    Level L = emptyLevel(Mode::Ufo);
    World W(L);
    GameState g = initialState(W);
    for (int i = 0; i < 3; ++i) step(W, g, false);
    float y0 = g.p[0].y;
    step(W, g, true);
    CHECK_NEAR(g.p[0].v, 6.871, 1e-6);
    float apex = 0;
    int ticks = 1;
    for (int t = 0; t < 400; ++t) {
        step(W, g, false);
        ++ticks;
        apex = std::max(apex, g.p[0].y - y0);
        if (g.p[0].events & EvLand) break;
    }
    CHECK_NEAR(apex, 43.42, 1.5);
    CHECK_NEAR(ticks, 128, 3);
    // holding does not re-jump (one jump per press)
    GameState h = initialState(W);
    for (int i = 0; i < 3; ++i) step(W, h, false);
    int jumps = 0;
    for (int t = 0; t < 300; ++t) { step(W, h, true); if (h.p[0].events & EvUfoJump) ++jumps; }
    CHECK(jumps == 0 || jumps == 1);
}

TEST(physics_ball_switch_time) {
    Level L = emptyLevel(Mode::Ball);
    World W(L);
    GameState g = initialState(W);
    for (int i = 0; i < 3; ++i) step(W, g, false);
    REQUIRE(g.p[0].bounded);
    CHECK_NEAR(g.p[0].ceilY, 240, 1e-3);
    int ticks = 0;
    step(W, g, true);
    ++ticks;
    CHECK(g.p[0].upsideDown);
    for (int t = 0; t < 400; ++t) {
        step(W, g, true); // holding does not re-switch
        ++ticks;
        if (g.p[0].events & EvLand) break;
    }
    CHECK(g.p[0].upsideDown);
    CHECK_NEAR(ticks, 99, 3); // spec: 98 ticks (+1 press-tick convention)
    CHECK_NEAR(g.p[0].y, 240 - 15, 0.01);
}

TEST(physics_robot_min_max_jump) {
    auto tap = measureJump(Mode::Robot, 1, false, 1);
    CHECK_NEAR(tap.apex, 18.75, 1.5);
    auto hold = measureJump(Mode::Robot, 1, false, 400);
    CHECK_NEAR(hold.apex, 103.02, 2.5);
    CHECK(hold.airTicks > tap.airTicks * 2);
}

TEST(physics_wave_diagonal) {
    Level L = emptyLevel(Mode::Wave);
    World W(L);
    GameState g = initialState(W);
    step(W, g, false);
    float y0 = g.p[0].y, x0 = g.p[0].x;
    for (int i = 0; i < 40; ++i) step(W, g, true);
    CHECK_NEAR(g.p[0].y - y0, g.p[0].x - x0, 0.05); // 45 degrees
    Level Lm = emptyLevel(Mode::Wave, 1, true);
    World Wm(Lm);
    GameState m = initialState(Wm);
    step(Wm, m, false);
    float my0 = m.p[0].y, mx0 = m.p[0].x;
    for (int i = 0; i < 20; ++i) step(Wm, m, true);
    CHECK_NEAR(m.p[0].y - my0, 2 * (m.p[0].x - mx0), 0.05); // mini wave is twice as steep
}

TEST(physics_cube_dies_on_wall_and_spike) {
    Level L = emptyLevel();
    L.objects.push_back(obj(level::id::Block, 165, 15));
    L.objects.push_back(obj(level::id::Block, 165, 45));
    World W(L);
    GameState g = initialState(W);
    for (int i = 0; i < 400 && !g.dead; ++i) step(W, g, false);
    CHECK(g.dead);
    CHECK(g.p[0].deathCause == DeathCause::Wall);
    Level S = emptyLevel();
    S.objects.push_back(obj(level::id::Spike, 165, 15));
    World WS(S);
    GameState h = initialState(WS);
    for (int i = 0; i < 400 && !h.dead; ++i) step(WS, h, false);
    CHECK(h.dead);
    CHECK(h.p[0].deathCause == DeathCause::Hazard);
}

TEST(physics_cube_jumps_spike_and_lands_on_block) {
    // spike at x=165; pressing ~0.25 s before clears it. Then a one-block step at x=345.
    Level L = emptyLevel();
    L.objects.push_back(obj(level::id::Spike, 165, 15));
    for (int i = 0; i < 6; ++i) L.objects.push_back(obj(level::id::Block, 435 + 30 * i, 15));
    World W(L);
    GameState g = initialState(W);
    int jumpTick = int((165 - 30) / 1.29825); // start jump ~1 block before the spike
    bool landedOnBlock = false;
    for (int t = 0; t < 600 && !g.dead && g.p[0].x < 620; ++t) {
        bool press = (t >= jumpTick - 20 && t < jumpTick - 18) || (t >= 270 && t < 272);
        step(W, g, press);
        if ((g.p[0].events & EvLand) && std::fabs(g.p[0].y - 45) < 0.01f) landedOnBlock = true;
    }
    CHECK(!g.dead);
    CHECK(landedOnBlock);
}

TEST(physics_yellow_orb_and_pad) {
    // pad at x=150 launches with v = 16 (cube); orb in the air: press while overlapping -> 11.18
    Level L = emptyLevel();
    L.objects.push_back(obj(level::id::PadYellow, 150, 2));
    World W(L);
    GameState g = initialState(W);
    double maxV = 0;
    for (int t = 0; t < 200; ++t) { step(W, g, false); if (g.p[0].events & EvPad) maxV = g.p[0].v; }
    CHECK_NEAR(maxV, 16.0, 1e-9);
    Level O = emptyLevel();
    O.objects.push_back(obj(level::id::OrbYellow, 300, 15));
    World WO(O);
    GameState h = initialState(WO);
    bool hit = false;
    for (int t = 0; t < 400 && !hit; ++t) {
        bool press = h.p[0].x > 285 && h.p[0].x < 290;
        step(WO, h, press);
        if (h.p[0].events & EvOrb) { hit = true; CHECK_NEAR(h.p[0].v, 11.18, 0.25); }
    }
    CHECK(hit);
}

TEST(physics_ship_portal_bounds_and_gravity_portal) {
    Level L = emptyLevel();
    L.objects.push_back(obj(level::id::PortalShip, 150, 45));
    L.objects.push_back(obj(level::id::PortalGravFlip, 600, 45));
    World W(L);
    GameState g = initialState(W);
    bool ship = false, flipped = false;
    for (int t = 0; t < 800 && !g.dead; ++t) {
        step(W, g, g.p[0].mode == Mode::Ship && g.p[0].y < 60);
        if (g.p[0].mode == Mode::Ship && !ship) {
            ship = true;
            CHECK_NEAR(g.p[0].floorY, 0, 1e-3);
            CHECK_NEAR(g.p[0].ceilY, 300, 1e-3);
        }
        if (g.p[0].upsideDown) flipped = true;
    }
    CHECK(ship);
    CHECK(flipped);
    CHECK(!g.dead);
}

TEST(physics_deterministic) {
    Level L = emptyLevel();
    for (int i = 0; i < 40; ++i) L.objects.push_back(obj(i % 3 ? level::id::Block : level::id::Spike, 300 + 90 * i, 15 + (i % 4) * 30));
    World W(L);
    auto run = [&]() {
        GameState g = initialState(W);
        uint64_t h = 1469598103934665603ull;
        for (int t = 0; t < 2000 && !g.dead; ++t) {
            step(W, g, (t / 37) % 3 == 0);
            uint32_t bits;
            std::memcpy(&bits, &g.p[0].y, 4);
            h = (h ^ bits) * 1099511628211ull;
        }
        return h;
    };
    CHECK(run() == run());
}

TEST(level_string_roundtrip) {
    Level L = emptyLevel(Mode::Ship, 2, true);
    L.settings.colors.push_back(level::makeChannel(1000, {20, 30, 60}));
    L.objects.push_back(obj(level::id::Spike, 165, 15, 180));
    auto o = obj(level::id::TrigMove, 300, 600);
    o.setProp(51, 5.0);
    o.setProp(28, 30.0);
    o.groups = {5, 7};
    L.objects.push_back(o);
    std::string s = L.toLevelString();
    auto back = Level::fromLevelString(s);
    REQUIRE(back.has_value());
    CHECK(back->settings.startMode == Mode::Ship);
    CHECK(back->settings.startSpeed == 2);
    CHECK(back->settings.startMini);
    CHECK(back->settings.colors.size() == 1 && back->settings.colors[0].color.b == 60);
    REQUIRE(back->objects.size() == L.objects.size());
    CHECK(back->objects[1].rotation == 180);
    CHECK(back->objects[2].groups.size() == 2 && back->objects[2].prop(51) == "5");
    CHECK(back->toLevelString() == s);
    auto gmd = L.toGmd();
    auto fromGmd = Level::fromGmd(gmd);
    REQUIRE(fromGmd.has_value());
    CHECK(fromGmd->toLevelString() == s);
}
