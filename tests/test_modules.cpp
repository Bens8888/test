// The structure library and the passes that place it.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/modules.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

using namespace al;
using namespace al::gen;

namespace {

const audio::MusicAnalysis& demoAnalysis() {
    static const audio::MusicAnalysis A = audio::analyze(audio::render(audio::demoSong()));
    return A;
}

level::LevelObject object(int id, double x, double y, double rot = 0) {
    level::LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

int count(const level::Level& L, int id) {
    int n = 0;
    for (const auto& o : L.objects) n += o.id == id;
    return n;
}

} // namespace

TEST(modules_library_is_well_formed) {
    size_t n = 0;
    const Module* table = moduleTable(n);
    CHECK(n >= 100);
    int used[kStructureStyles] = {};
    for (size_t i = 0; i < n; ++i) {
        const Module& m = table[i];
        CHECK(m.count >= 2 && m.w >= 1 && m.h >= 1 && m.modes != 0);
        for (int s = 0; s < kStructureStyles; ++s) used[s] += m.weight[s] > 0;
        for (int k = 0; k < m.count; ++k) {
            const ModulePiece& p = modulePieces()[m.first + k];
            CHECK_MSG(p.rot % 90 == 0, "module %d piece %d rotated by %d", int(i), k, p.rot);
            const auto kind = level::objectInfo(p.id).kind;
            CHECK_MSG(kind == level::Kind::Solid || kind == level::Kind::Slope || kind == level::Kind::Hazard, "module %d: object %d", int(i), p.id);
        }
    }
    for (int s = 0; s < kStructureStyles; ++s) CHECK_MSG(used[s] >= 5, "style %s: %d modules", structureStyle(s).name, used[s]);
}

// Bloodbath's two-pad corridors, tick 1078 of a run recorded in GD: the ceiling pad touched while
// the gravity is normal does nothing, and fires as soon as the floor pad has flipped the gravity.
TEST(blue_pad_facing_away_stays_armed) {
    level::Level L;
    L.objects.push_back(object(67, 60, 27, 180));
    L.objects.push_back(object(67, 60, 3));
    phys::World W(L);
    W.setEndX(1e9f);
    auto s = phys::initialState(W);
    int flips = 0;
    bool wasFlipped = false;
    for (int t = 0; t < 40 && !s.dead; ++t) {
        phys::step(W, s, false);
        if (s.p[0].upsideDown != wasFlipped) ++flips;
        wasFlipped = s.p[0].upsideDown;
    }
    CHECK_MSG(flips == 2, "%d gravity flips", flips);
    CHECK(!s.p[0].upsideDown);
}

// Run 82 of the real-game check: a spider standing on a floor hits a ship portal. Its step's fall is
// undone first, so the ship starts level, not 0.03 units and half a step of speed lower.
TEST(ground_mode_enters_a_flying_portal_level) {
    level::Level L;
    L.settings.startMode = Mode::Spider;
    L.objects.push_back(object(13, 60, 30));
    phys::World W(L);
    W.setEndX(1e9f);
    auto s = phys::initialState(W);
    while (s.p[0].mode == Mode::Spider && s.tick < 60) {
        phys::step(W, s, false);
        if (s.p[0].mode != Mode::Spider) break;
        CHECK(s.p[0].onGround);
    }
    REQUIRE(s.p[0].mode == Mode::Ship);
    CHECK(s.p[0].v == 0);
}

// Run 84: a swing resting on the floor flips with the press and only moves on the step after.
TEST(swing_press_on_the_floor_holds_for_a_step) {
    level::Level L;
    L.settings.startMode = Mode::Swing;
    phys::World W(L);
    W.setEndX(1e9f);
    auto s = phys::initialState(W);
    for (int t = 0; t < 10; ++t) phys::step(W, s, false);
    REQUIRE(s.p[0].onGround);
    const float y = s.p[0].y;
    phys::step(W, s, true);
    CHECK(s.p[0].upsideDown);
    CHECK(s.p[0].y == y);
    phys::step(W, s, true);
    CHECK(s.p[0].y > y);
}

TEST(structure_styles_beatable_and_built_from_the_library) {
    const auto& A = demoAnalysis();
    for (int style = 1; style <= kStructureStyles; ++style) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = uint64_t(40 + style);
        opt.difficulty = Difficulty::Insane;
        opt.structureStyle = style;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        phys::World W(res.level);
        CHECK_MSG(bot::isBeatable(W, res.inputs), "style %d", style);
        CHECK_MSG(res.report.intendedInputs, "style %d: the designed clicks do not win", style);
        const int rails = count(res.level, level::id::OutlineTop);
        CHECK_MSG(rails > 0, "style %d: no library pieces", style);
    }
}

TEST(wave_parts_are_line_slopes) {
    const auto& A = demoAnalysis();
    GenOptions opt;
    opt.engine = 2;
    opt.seed = 7;
    opt.difficulty = Difficulty::Insane;
    opt.variants = 1;
    opt.modeWeights.fill(0.0f);
    opt.modeWeights[size_t(Mode::Wave)] = 1.0f;
    auto res = generateVariant(A, opt, opt.seed, nullptr);
    phys::World W(res.level);
    CHECK(bot::isBeatable(W, res.inputs));
    CHECK_MSG(count(res.level, 1338) > 100, "%d line slopes", count(res.level, 1338));
}

// Wave parts are a sealed corridor: over and under the wave there is always a wall or the bound,
// and the wave rides on the floor or ceiling only now and then.
TEST(wave_corridors_are_sealed) {
    const auto& A = demoAnalysis();
    int ticks = 0, open = 0, sliding = 0;
    for (uint64_t seed : {7, 8, 9, 10}) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = seed;
        opt.difficulty = Difficulty(3 + int(seed % 5));
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        std::vector<const level::LevelObject*> walls;
        for (const auto& o : res.level.objects)
            if (o.id == 1338 || o.id == 1339 || o.id == level::id::OutlineTop) walls.push_back(&o);
        std::sort(walls.begin(), walls.end(), [](auto a, auto b) { return a->x < b->x; });
        phys::World W(res.level);
        auto s = phys::initialState(W);
        size_t next = 0;
        bool held = false;
        while (!s.dead && !s.won) {
            while (next < res.inputs.events.size() && res.inputs.events[next].tick <= s.tick + 1) held = res.inputs.events[next++].press;
            phys::step(W, s, held);
            const auto& p = s.p[0];
            if (p.mode != Mode::Wave || s.tick % 4) continue;
            ++ticks;
            sliding += p.onGround;
            bool over = p.bounded && p.ceilY - p.y < 90, under = p.bounded && p.y - p.floorY < 90;
            auto it = std::lower_bound(walls.begin(), walls.end(), p.x - 16.0, [](auto a, double v) { return a->x < v; });
            for (; it != walls.end() && (*it)->x <= p.x + 16; ++it) {
                const double dy = (*it)->y - p.y;
                if (dy > 0 && dy < 150) over = true;
                if (dy < 0 && dy > -150) under = true;
            }
            open += !(over && under);
        }
    }
    std::printf("    %d wave ticks: %.1f%% without a wall on a side, %.1f%% sliding\n", ticks, 100.0 * open / std::max(1, ticks), 100.0 * sliding / std::max(1, ticks));
    CHECK(ticks > 0);
}

TEST(mini_wave_parts_keep_their_walls) {
    const auto& A = demoAnalysis();
    GenOptions opt;
    opt.engine = 2;
    opt.seed = 7;
    opt.difficulty = Difficulty::ExtremeDemon;
    opt.variants = 1;
    opt.modeWeights.fill(0.0f);
    opt.modeWeights[size_t(Mode::Wave)] = 1.0f;
    auto res = generateVariant(A, opt, opt.seed, nullptr);
    phys::World W(res.level);
    CHECK(bot::isBeatable(W, res.inputs));
    CHECK_MSG(count(res.level, 1339) > 100, "%d steep line slopes", count(res.level, 1339));
}

TEST(structures_are_a_mass_with_lined_faces) {
    const auto& A = demoAnalysis();
    GenOptions opt;
    opt.engine = 2;
    opt.seed = 9;
    opt.difficulty = Difficulty::Insane;
    opt.variants = 1;
    auto res = generateVariant(A, opt, opt.seed, nullptr);
    phys::World W(res.level);
    CHECK(bot::isBeatable(W, res.inputs));
    double last = 1;
    int hazards = 0;
    for (const auto& o : res.level.objects) {
        last = std::max(last, o.x);
        const auto kind = o.info().kind;
        hazards += kind == level::Kind::Hazard || kind == level::Kind::Saw;
    }
    const double screens = last / 570.0;
    CHECK_MSG(double(res.level.objects.size()) / screens > 40, "%.0f objects per screen", double(res.level.objects.size()) / screens);
    CHECK_MSG(double(hazards) / screens > 15, "%.0f hazards per screen", double(hazards) / screens);
}

// Spikes stand straight on their block or wall, at quarter turns only.
TEST(spikes_stand_straight) {
    const auto& A = demoAnalysis();
    for (int style = 1; style <= kStructureStyles; ++style) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = uint64_t(80 + style);
        opt.difficulty = Difficulty::Insane;
        opt.structureStyle = style;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        for (const auto& o : res.level.objects)
            if (o.info().kind == level::Kind::Hazard) {
                CHECK_MSG(std::fmod(std::abs(o.rotation), 90.0) == 0, "style %d: hazard %d turned %.0f at (%.0f, %.0f)", style, o.id, o.rotation, o.x, o.y);
                // thorn and wavy spikes are drawn bent
                CHECK_MSG(o.id != 177 && o.id != 216 && o.id != 217 && o.id != 218, "style %d: bent spike %d at (%.0f, %.0f)", style, o.id, o.x, o.y);
            }
    }
}

namespace {

// What is drawn, roughly: a spike fills its 30 unit cell, a half spike the lower half of it, a saw
// a little more than its hitbox.
struct Drawn {
    float x0, y0, x1, y1;
    bool round;
    int id = 0;
};
bool drawnHazard(const level::LevelObject& o, Drawn& d) {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return false;
    if (so.kind == level::Kind::Saw) {
        const float r = so.hw * 1.1f;
        d = {so.x - r, so.y - r, so.x + r, so.y + r, true, o.id};
        return true;
    }
    if (so.kind != level::Kind::Hazard) return false;
    float w = 30, h = 30;
    if (o.id == 39 || o.id == 217) h = 14;
    else if (o.id == 103 || o.id == 218) w = h = 18;
    else if (o.id == 392) { w = 12; h = 10; }
    else if (o.id == 458 || o.id == 459) h = 10;
    const bool side = std::fmod(std::abs(o.rotation), 180.0) == 90;
    if (side) std::swap(w, h);
    d = {float(o.x) - w / 2, float(o.y) - h / 2, float(o.x) + w / 2, float(o.y) + h / 2, false, o.id};
    return true;
}
float common(const Drawn& a, const Drawn& b) {
    const float w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0), h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    return w > 0 && h > 0 ? w * h : 0;
}
float area(const Drawn& a) { return (a.x1 - a.x0) * (a.y1 - a.y0); }

struct Overlaps {
    int hazards = 0, piles = 0, buried = 0;
};
// Hazards on top of each other, and hazards drawn into a solid.
Overlaps overlapsOf(const level::Level& L) {
    Overlaps r;
    std::vector<Drawn> hz, solid;
    for (const auto& o : L.objects) {
        Drawn d{};
        if (drawnHazard(o, d)) { hz.push_back(d); continue; }
        phys::SimObject so;
        if (!phys::World::makeSimObject(o, 0, so) || so.kind != level::Kind::Solid) continue;
        solid.push_back({so.x - so.hw, so.y - so.hh, so.x + so.hw, so.y + so.hh, false, o.id});
    }
    r.hazards = int(hz.size());
    std::sort(hz.begin(), hz.end(), [](const Drawn& a, const Drawn& b) { return a.x0 < b.x0; });
    std::sort(solid.begin(), solid.end(), [](const Drawn& a, const Drawn& b) { return a.x0 < b.x0; });
    size_t s0 = 0;
    for (size_t i = 0; i < hz.size(); ++i) {
        for (size_t k = i + 1; k < hz.size() && hz[k].x0 < hz[i].x1; ++k)
            if (common(hz[i], hz[k]) > 0.2f * std::min(area(hz[i]), area(hz[k]))) {
                if (std::getenv("AL_OVERLAPS") && r.piles < 12)
                    std::printf("      pile %d (%.0f %.0f %.0f %.0f) %d (%.0f %.0f %.0f %.0f)\n", hz[i].id, hz[i].x0, hz[i].y0, hz[i].x1, hz[i].y1, hz[k].id, hz[k].x0, hz[k].y0, hz[k].x1, hz[k].y1);
                ++r.piles;
                break;
            }
        // a hazard stands on a face: only a sliver of it may be in the solid
        while (s0 < solid.size() && solid[s0].x0 < hz[i].x0 - 120) ++s0;
        Drawn in = hz[i];
        const float m = hz[i].round ? (hz[i].x1 - hz[i].x0) * 0.5f - 6.f : 3.f; // a saw bites into a wall, its hub stays out
        in.x0 += m; in.y0 += m; in.x1 -= m; in.y1 -= m;
        for (size_t k = s0; k < solid.size() && solid[k].x0 < in.x1; ++k)
            if (common(in, solid[k]) > 1) {
                if (std::getenv("AL_OVERLAPS") && r.buried < 12)
                    std::printf("      sunk %d (%.0f %.0f %.0f %.0f) in %d (%.0f %.0f %.0f %.0f)\n", hz[i].id, hz[i].x0, hz[i].y0, hz[i].x1, hz[i].y1, solid[k].id, solid[k].x0, solid[k].y0, solid[k].x1, solid[k].y1);
                ++r.buried;
                break;
            }
    }
    return r;
}

} // namespace

// Hazards stand side by side on the faces, not in piles or sunk into the blocks.
TEST(hazards_do_not_overlap) {
    const auto& A = demoAnalysis();
    for (int style = 1; style <= kStructureStyles; ++style) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = uint64_t(90 + style);
        opt.difficulty = Difficulty::Insane;
        opt.structureStyle = style;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        const Overlaps o = overlapsOf(res.level);
        std::printf("    style %d: %d hazards, %d in a pile, %d sunk into a block\n", style, o.hazards, o.piles, o.buried);
        CHECK_MSG(o.piles * 100 <= o.hazards * 2, "style %d: %d of %d hazards in a pile", style, o.piles, o.hazards);
        CHECK_MSG(o.buried * 100 <= o.hazards * 2, "style %d: %d of %d hazards sunk into a block", style, o.buried, o.hazards);
    }
}

// A portal only touched at the edge of the player is missed by anyone a little off the path.
TEST(portals_are_passed_through_not_grazed) {
    const auto& A = demoAnalysis();
    for (int style = 1; style <= kStructureStyles; ++style) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = uint64_t(60 + style);
        opt.difficulty = Difficulty::Insane;
        opt.structureStyle = style;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        phys::World W(res.level);
        std::vector<size_t> portals;
        for (size_t i = 0; i < res.level.objects.size(); ++i) {
            const auto kind = res.level.objects[i].info().kind;
            if (kind == level::Kind::ModePortal || kind == level::Kind::GravityPortal || kind == level::Kind::SizePortal || kind == level::Kind::SpeedPortal)
                portals.push_back(i);
        }
        std::vector<double> depth(portals.size(), -1e9);
        auto s = phys::initialState(W);
        size_t next = 0;
        bool held = false;
        while (!s.dead && !s.won) {
            while (next < res.inputs.events.size() && res.inputs.events[next].tick <= s.tick + 1) held = res.inputs.events[next++].press;
            phys::step(W, s, held);
            for (size_t k = 0; k < portals.size(); ++k) {
                const auto& o = res.level.objects[portals[k]];
                if (std::abs(o.x - s.p[0].x) > (o.info().w + s.p[0].width()) * 0.5) continue;
                // the path the player was on as it came in: the portal itself turns it away
                const auto& q = s.p[0];
                const double reach = (o.info().h + q.height()) * 0.5, inside = reach - std::abs(o.y - q.y);
                if (inside <= 0 || depth[k] > 0) { depth[k] = std::max(depth[k], inside); continue; }
                const double slope = q.x > q.lastX ? (q.y - q.lastY) / (q.x - q.lastX) : 0;
                depth[k] = std::max(inside, reach - std::abs(o.y - (q.y + slope * (o.x - q.x))));
            }
        }
        CHECK(s.won);
        for (size_t k = 0; k < portals.size(); ++k)
            if (depth[k] > -1e8) CHECK_MSG(depth[k] >= 12, "style %d: portal %d at (%.0f, %.0f) touched by only %.1f units", style, res.level.objects[portals[k]].id, res.level.objects[portals[k]].x, res.level.objects[portals[k]].y, depth[k]);
    }
}

TEST(pads_have_their_real_width) {
    CHECK(object(35, 0, 0).info().w == 25);
    CHECK(object(140, 0, 0).info().w == 25);
    CHECK(object(67, 0, 0).info().w == 25);
    CHECK(object(1332, 0, 0).info().w == 29);
}

TEST(speed_style_steers_the_speed) {
    const auto& A = demoAnalysis();
    auto mean = [&](int speedStyle) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = 3;
        opt.difficulty = Difficulty::Harder;
        opt.speedStyle = speedStyle;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        double sum = 0;
        for (const auto& p : res.plan.parts) sum += p.speed;
        return sum / double(std::max<size_t>(1, res.plan.parts.size()));
    };
    CHECK(mean(1) < mean(3));
}
