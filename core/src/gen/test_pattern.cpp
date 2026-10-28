#include <al/gen/test_pattern.hpp>
#include <al/gen/tiles.hpp>
#include <al/util/codec.hpp>

namespace al::gen {

using level::LevelObject;
namespace id = level::id;

static LevelObject mk(int oid, double x, double y, double rot = 0) {
    LevelObject o;
    o.id = oid;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

std::vector<LevelObject> makeTestPattern(double x0) {
    std::vector<LevelObject> v;
    auto X = [&](double blocks) { return x0 + 15 + 30 * blocks; };
    // cube: spike, a block step and a yellow pad
    v.push_back(mk(id::Spike, X(4), 15));
    v.push_back(mk(id::Block, X(10), 15));
    v.push_back(mk(id::Block, X(11), 15));
    v.push_back(mk(id::PadYellow, X(15), 2));
    // orb over a spike pit
    for (int i = 0; i < 3; ++i) v.push_back(mk(id::Spike, X(22 + i), 15));
    v.push_back(mk(id::OrbYellow, X(22), 75));
    // a gamemode portal parade (each followed by a few blocks of flat ground)
    const int portals[] = {id::PortalShip, id::PortalBall, id::PortalUfo, id::PortalWave, id::PortalRobot, id::PortalSpider, id::PortalSwing, id::PortalCube};
    double b = 30;
    for (int p : portals) {
        v.push_back(mk(p, X(b), 45));
        b += 10;
    }
    // speed portals and size portals
    v.push_back(mk(id::Speed2, X(b), 45));
    v.push_back(mk(id::PortalSizeMini, X(b + 6), 45));
    v.push_back(mk(id::PortalSizeNormal, X(b + 14), 45));
    v.push_back(mk(id::Speed1, X(b + 20), 45));
    return v;
}

static LevelObject label(const std::string& text, double x, double y) {
    LevelObject o = mk(914, x, y);
    o.setProp(31, base64Encode(text, true));
    o.scale = 0.5;
    return o;
}

std::vector<LevelObject> makePieceTest(double x0) {
    std::vector<LevelObject> v;
    // rows (bottom to top): the outline pieces at 0 / 90 / 180 / 270 degrees, then the outline
    // slope in the four orientations the generator uses
    const int pieces[] = {id::OutlineTop, id::OutlineCorner, id::OutlineTopPillar, id::OutlinePillar};
    const char* names[] = {"468 edge", "469 corner", "470 cap", "471 pillar"};
    double y = 135;
    for (int i = 0; i < 4; ++i, y += 75) {
        v.push_back(label(names[i], x0 + 45, y));
        for (int q = 0; q < 4; ++q) {
            double x = x0 + 165 + q * 90;
            v.push_back(mk(pieces[i], x, y, q * 90.0));
            v.push_back(label(std::to_string(q * 90), x, y - 30));
        }
    }
    v.push_back(label("1338 slope", x0 + 45, y));
    const char* slopeNames[] = {"floor /", "floor \\", "ceil /", "ceil \\"};
    const CellKind kinds[] = {CellKind::SlopeFloorUp, CellKind::SlopeFloorDown, CellKind::SlopeCeilUp, CellKind::SlopeCeilDown};
    for (int q = 0; q < 4; ++q) {
        CellGrid g;
        int cx = int((x0 + 150 + q * 90) / 30), cy = int(y / 30);
        g.set(cx, cy, kinds[q]);
        std::vector<LevelObject> one;
        tileCells(g, TileLook::Outline, one);
        for (auto& o : one) if (o.id != id::OutlineTop) v.push_back(o);
        v.push_back(label(slopeNames[q], cx * 30 + 15, y - 30));
    }
    // a tiled structure (a pillar with a slope corner and a hanging pillar) to see the silhouette
    CellGrid g;
    const int sx = int((x0 + 600) / 30);
    g.fill(sx, 0, sx + 2, 3);
    g.set(sx + 2, 3, CellKind::SlopeFloorDown);
    g.fill(sx + 5, 7, sx + 6, 12);
    g.set(sx + 4, 12, CellKind::SlopeCeilDown);
    tileCells(g, TileLook::Outline, v);
    v.push_back(label("expected: one clean silhouette, no inner lines", x0 + 690, 420));
    return v;
}

} // namespace al::gen

