#include <al/gen/test_pattern.hpp>

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
    // a colour trigger for the background
    LevelObject col = mk(id::TrigColor, X(2), 600);
    col.setProp(36, std::string("1"));
    col.setProp(23, 1000.0);
    col.setProp(7, 40.0);
    col.setProp(8, 20.0);
    col.setProp(9, 90.0);
    col.setProp(10, 0.5);
    v.push_back(col);
    return v;
}

} // namespace al::gen
