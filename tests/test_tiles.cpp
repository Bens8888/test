// Tiles: the outline silhouette of a structure must play exactly like the solid structure
// (landing on top, dying on the side, ceilings), and the tiler picks the right piece per cell.
#include "test.hpp"

#include <al/gen/tiles.hpp>
#include <al/level/level.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>

using namespace al;
using namespace al::gen;
using level::Level;
using level::LevelObject;

namespace {

Level levelWith(const CellGrid& g, TileLook look) {
    Level L;
    L.settings.startMode = level::Mode::Cube;
    L.settings.startSpeed = 1;
    LevelObject end;
    end.id = level::id::TrigEnd;
    end.x = 3000;
    end.y = 300;
    L.objects.push_back(end);
    tileCells(g, look, L.objects);
    return L;
}

struct Run { bool dead; int tick; float x, y; };

// a press at `pressTick` (held for 6 ticks), run until `ticks`
Run play(const Level& L, int pressTick, int ticks) {
    phys::World W(L);
    phys::GameState s = phys::initialState(W);
    for (int t = 0; t < ticks && !s.dead && !s.won; ++t) phys::step(W, s, pressTick >= 0 && s.tick >= pressTick && s.tick < pressTick + 6);
    return {s.dead, s.tick, s.p[0].x, s.p[0].y};
}

} // namespace

TEST(tiles_piece_choice) {
    CellGrid g;
    g.fill(10, 0, 11, 2); // a 2x3 pillar
    CHECK(g.exposed(10, 2) == (EdgeT | EdgeL));
    CHECK(g.exposed(11, 2) == (EdgeT | EdgeR));
    CHECK(g.exposed(10, 1) == EdgeL);
    CHECK(g.exposed(11, 0) == (EdgeR | EdgeB));
    std::vector<LevelObject> out;
    tileCells(g, TileLook::Outline, out);
    CHECK(out.size() == 6); // every cell of a 2-wide pillar is on the outside
    for (auto& o : out) {
        // the drawn edges of each piece are exactly the exposed edges of its cell
        int cx = int(std::floor(o.x / 30.0)), cy = int(std::floor(o.y / 30.0));
        CHECK_MSG(drawnEdges(o) == g.exposed(cx, cy), "cell %d,%d id %d rot %.0f", cx, cy, o.id, o.rotation);
    }
    // a 3x3 block has an empty middle
    CellGrid b;
    b.fill(0, 0, 2, 2);
    out.clear();
    tileCells(b, TileLook::Outline, out);
    CHECK(out.size() == 8);
    // slope neighbours cover their full sides
    CellGrid s;
    s.set(5, 0, CellKind::Solid);
    s.set(6, 0, CellKind::SlopeFloorDown); // "\" right of the block: its left side is full
    CHECK((s.exposed(5, 0) & EdgeR) == 0);
    CHECK(rotateEdges(EdgeT, 1) == EdgeR && rotateEdges(EdgeL, 1) == EdgeT && rotateEdges(EdgeT | EdgeL, 3) == (EdgeL | EdgeB));
}

TEST(tiles_outline_plays_like_solid) {
    // pillar 2 wide, 2 high at x = 300..360
    CellGrid g;
    g.fill(10, 0, 11, 1);
    const Level solid = levelWith(g, TileLook::Blocks), outline = levelWith(g, TileLook::Outline);
    // running into the side
    Run a = play(solid, -1, 600), b = play(outline, -1, 600);
    CHECK(a.dead && b.dead);
    CHECK_MSG(std::abs(a.tick - b.tick) <= 1, "side death tick %d vs %d", a.tick, b.tick);
    // jumping onto the top (press so that the cube lands on it) and running off the other side
    int landed = 0;
    for (int press = 100; press < 260; ++press) {
        Run s1 = play(solid, press, 700), s2 = play(outline, press, 700);
        CHECK_MSG(s1.dead == s2.dead, "press %d: solid dead %d outline dead %d", press, s1.dead, s2.dead);
        if (!s1.dead) {
            ++landed;
            CHECK_MSG(std::fabs(s1.y - s2.y) < 0.01f && std::fabs(s1.x - s2.x) < 0.01f, "press %d: (%.1f %.1f) vs (%.1f %.1f)", press, s1.x, s1.y, s2.x, s2.y);
        }
    }
    CHECK(landed > 5);
}

TEST(tiles_outline_ceiling_plays_like_solid) {
    // a ceiling bar at y = 90..120 over the run: a jump hits it head first
    CellGrid g;
    g.fill(8, 3, 20, 3);
    const Level solid = levelWith(g, TileLook::Blocks), outline = levelWith(g, TileLook::Outline);
    for (int press = 0; press < 160; press += 7) {
        Run a = play(solid, press, 500), b = play(outline, press, 500);
        CHECK_MSG(a.dead == b.dead && std::fabs(a.y - b.y) < 0.01f, "press %d: dead %d/%d y %.1f/%.1f", press, a.dead, b.dead, a.y, b.y);
    }
}
