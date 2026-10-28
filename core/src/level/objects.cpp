#include <al/level/objects.hpp>

#include <array>
#include <cstddef>
#include <initializer_list>
using std::size_t;
#include <utility>

namespace al::level {

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Cube: return "cube";
        case Mode::Ship: return "ship";
        case Mode::Ball: return "ball";
        case Mode::Ufo: return "ufo";
        case Mode::Wave: return "wave";
        case Mode::Robot: return "robot";
        case Mode::Spider: return "spider";
        case Mode::Swing: return "swing";
        default: return "?";
    }
}

namespace {

constexpr int kMaxId = 5000;

struct Table {
    std::array<ObjectInfo, kMaxId> info{};

    void set(int id, Kind k, float w, float h, uint8_t sub = 0, bool wide = false) {
        if (id <= 0 || id >= kMaxId) return;
        info[size_t(id)] = ObjectInfo{id, k, sub, w, h, wide};
    }
    void range(std::initializer_list<std::pair<int, int>> rs, Kind k, float w, float h) {
        for (auto [a, b] : rs) for (int i = a; i <= b; ++i) set(i, k, w, h);
    }

    Table() {
        for (int i = 0; i < kMaxId; ++i) info[size_t(i)].id = i;
        // ---- solids (gd-sim table, physics_spec §8.4)
        range({{1, 4}, {6, 7}, {63, 63}, {69, 72}, {74, 78}, {81, 83}, {90, 96}, {116, 119}, {121, 122}, {146, 146},
               {160, 163}, {165, 169}, {173, 173}, {175, 175}, {207, 210}, {212, 213}, {247, 250}, {252, 258},
               {260, 261}, {263, 265}, {267, 272}, {274, 275}, {467, 467}, {469, 471}, {1203, 1204}, {1209, 1210},
               {1221, 1222}, {1226, 1226}},
              Kind::Solid, 30, 30);
        range({{64, 64}, {195, 195}, {206, 206}, {220, 220}, {661, 661}, {1155, 1157}, {1208, 1208}, {1910, 1910}}, Kind::Solid, 15, 15);
        range({{40, 40}, {147, 147}, {215, 215}, {369, 370}, {1903, 1905}}, Kind::Solid, 30, 14);
        range({{170, 172}, {174, 174}, {192, 192}}, Kind::Solid, 30, 21);
        range({{468, 468}, {475, 475}, {1260, 1260}}, Kind::Solid, 30, 1.5f);
        range({{62, 62}, {65, 66}, {68, 68}}, Kind::Solid, 30, 16);
        range({{1202, 1202}, {1262, 1262}}, Kind::Solid, 30, 3);
        range({{1220, 1220}, {1264, 1264}}, Kind::Solid, 30, 6);
        range({{196, 196}, {219, 219}, {1911, 1911}}, Kind::Solid, 15, 8);
        set(204, Kind::Solid, 8, 15);
        range({{662, 664}}, Kind::Solid, 30, 15);
        set(1561, Kind::Solid, 30, 10);
        set(1567, Kind::Solid, 15, 10);
        set(1566, Kind::Solid, 12, 12);
        set(1565, Kind::Solid, 17, 17);
        set(1227, Kind::Solid, 30, 7);
        set(328, Kind::Solid, 22, 22);
        set(197, Kind::Solid, 22, 21);
        set(194, Kind::Solid, 21, 21);
        set(176, Kind::Solid, 14, 21);
        set(1562, Kind::Solid, 30, 2);
        set(1343, Kind::Solid, 25, 3);
        set(1340, Kind::Solid, 27, 2);
        set(143, Kind::Breakable, 30, 30);

        // ---- hazards
        range({{720, 720}, {991, 991}, {1731, 1731}, {1733, 1733}}, Kind::Hazard, 2.40039063f, 3.20001221f);
        range({{61, 61}, {446, 446}, {1719, 1719}, {1728, 1728}}, Kind::Hazard, 9, 7.2f);
        range({{365, 365}, {667, 667}, {1716, 1716}, {1730, 1730}}, Kind::Hazard, 9, 6);
        range({{392, 392}, {458, 459}}, Kind::Hazard, 2.6f, 4.8f);
        range({{8, 8}, {144, 144}, {177, 177}, {216, 216}}, Kind::Hazard, 6, 12);
        range({{103, 103}, {145, 145}, {218, 218}}, Kind::Hazard, 4, 7.6f);
        range({{39, 39}, {205, 205}, {217, 217}}, Kind::Hazard, 6, 5.6f);
        range({{768, 768}, {1727, 1727}}, Kind::Hazard, 4.5f, 5.2f);
        range({{447, 447}, {1729, 1729}}, Kind::Hazard, 5.2f, 7.2f);
        range({{135, 135}, {1711, 1711}}, Kind::Hazard, 14.1f, 20);
        range({{422, 422}, {1726, 1726}}, Kind::Hazard, 6, 4.4f);
        range({{244, 244}, {1721, 1721}}, Kind::Hazard, 6, 6.8f);
        range({{243, 243}, {1720, 1720}}, Kind::Hazard, 6, 7.2f);
        range({{421, 421}, {1725, 1725}}, Kind::Hazard, 9, 5.2f);
        range({{9, 9}, {1715, 1715}}, Kind::Hazard, 9, 10.8f);
        range({{989, 989}, {1732, 1732}}, Kind::Hazard, 9, 12);
        set(1714, Kind::Hazard, 11.4f, 16.4f);
        set(1712, Kind::Hazard, 13.5f, 22.4f);
        range({{368, 368}, {1722, 1722}}, Kind::Hazard, 9, 4);
        set(1713, Kind::Hazard, 11.7f, 20);
        set(178, Kind::Hazard, 6, 6.4f);
        set(919, Kind::Hazard, 25, 6);
        set(179, Kind::Hazard, 4, 8);

        // ---- saws (w = radius)
        auto saw = [&](std::initializer_list<int> ids, float r) { for (int i : ids) set(i, Kind::Saw, r, r); };
        saw({88, 186, 740, 1705}, 32.3f);
        saw({89, 1706}, 21.6f);
        saw({98, 1707}, 12);
        saw({183}, 15.66f);
        saw({184}, 20.4f);
        saw({185}, 2.85f);
        saw({187, 741}, 21.96f);
        saw({188, 742}, 12.6f);
        saw({397, 1708}, 28.9f);
        saw({398, 1709}, 17.44f);
        saw({399, 1710}, 12.9f);
        saw({675, 1734}, 32);
        saw({676, 1735}, 17.51f);
        saw({677, 1736}, 12.48f);
        saw({678}, 30.4f);
        saw({679}, 18.54f);
        saw({680}, 10.8f);
        saw({918}, 24);
        saw({1582, 1583}, 4);
        saw({1619}, 25);
        saw({1620}, 15);
        saw({1701, 1702, 1703}, 6);

        // ---- orbs (36x36)
        set(36, Kind::Orb, 36, 36, uint8_t(OrbType::Yellow));
        set(141, Kind::Orb, 36, 36, uint8_t(OrbType::Pink));
        set(1333, Kind::Orb, 36, 36, uint8_t(OrbType::Red));
        set(84, Kind::Orb, 36, 36, uint8_t(OrbType::Blue));
        set(1022, Kind::Orb, 36, 36, uint8_t(OrbType::Green));
        set(1330, Kind::Orb, 36, 36, uint8_t(OrbType::Black));
        set(1704, Kind::Orb, 36, 36, uint8_t(OrbType::DashGreen));
        set(1751, Kind::Orb, 36, 36, uint8_t(OrbType::DashPink));
        set(3004, Kind::Orb, 36, 36, uint8_t(OrbType::Spider));
        set(3027, Kind::Orb, 36, 36, uint8_t(OrbType::Teleport));
        set(1594, Kind::Orb, 36, 36, uint8_t(OrbType::Toggle));

        // ---- pads
        set(35, Kind::Pad, 25, 4, uint8_t(PadType::Yellow));
        set(140, Kind::Pad, 25, 5, uint8_t(PadType::Pink));
        set(1332, Kind::Pad, 25, 6, uint8_t(PadType::Red));
        set(67, Kind::Pad, 25, 6, uint8_t(PadType::Blue));
        set(3005, Kind::Pad, 25, 6, uint8_t(PadType::Spider));

        // ---- portals
        set(12, Kind::ModePortal, 34, 86, uint8_t(Mode::Cube));
        set(13, Kind::ModePortal, 34, 86, uint8_t(Mode::Ship));
        set(47, Kind::ModePortal, 34, 86, uint8_t(Mode::Ball));
        set(111, Kind::ModePortal, 34, 86, uint8_t(Mode::Ufo));
        set(660, Kind::ModePortal, 34, 86, uint8_t(Mode::Wave));
        set(745, Kind::ModePortal, 34, 86, uint8_t(Mode::Robot));
        set(1331, Kind::ModePortal, 34, 86, uint8_t(Mode::Spider));
        set(1933, Kind::ModePortal, 34, 86, uint8_t(Mode::Swing));
        set(10, Kind::GravityPortal, 25, 75, uint8_t(GravityPortalType::Normal));
        set(11, Kind::GravityPortal, 25, 75, uint8_t(GravityPortalType::Flip));
        set(2926, Kind::GravityPortal, 25, 75, uint8_t(GravityPortalType::Toggle));
        set(99, Kind::SizePortal, 31, 90, 0);
        set(101, Kind::SizePortal, 31, 90, 1);
        set(286, Kind::DualPortal, 34, 86, 1);
        set(287, Kind::DualPortal, 34, 86, 0);
        set(45, Kind::MirrorPortal, 34, 86, 1);
        set(46, Kind::MirrorPortal, 34, 86, 0);
        set(747, Kind::TeleportPortal, 34, 86, 0);
        set(200, Kind::SpeedPortal, 35, 44, 0);
        set(201, Kind::SpeedPortal, 33, 56, 1);
        set(202, Kind::SpeedPortal, 51, 56, 2);
        set(203, Kind::SpeedPortal, 65, 56, 3);
        set(1334, Kind::SpeedPortal, 69, 56, 4);

        // ---- slopes
        for (int i : {289, 294, 299, 305, 309, 315, 321, 326, 331, 337, 343, 349, 353, 371, 483, 492, 651, 665, 673, 709, 711,
                      726, 728, 886, 1338, 1341, 1344, 1723, 1743, 1745, 1747, 1749, 1906})
            set(i, Kind::Slope, 30, 30);
        for (int i : {291, 295, 301, 307, 311, 317, 323, 327, 333, 339, 345, 351, 355, 367, 372, 484, 493, 652, 666, 674, 710,
                      712, 727, 729, 887, 1339, 1342, 1345, 1724, 1744, 1746, 1748, 1750, 1907})
            set(i, Kind::Slope, 60, 30, 0, true);
        set(363, Kind::HazardSlope, 30, 30);
        set(1717, Kind::HazardSlope, 30, 30);
        set(364, Kind::HazardSlope, 60, 30, 0, true);
        set(366, Kind::HazardSlope, 60, 30, 0, true);
        set(1718, Kind::HazardSlope, 60, 30, 0, true);

        // ---- misc
        set(31, Kind::StartPos, 0, 0);
        set(1329, Kind::Coin, 0, 0);
        set(3600, Kind::EndTrigger, 0, 0);
        for (int t : {899, 901, 1006, 1007, 1049, 1268, 1346, 1347, 1520, 1585, 1814, 1595, 1611, 1616, 1912, 1913, 1914, 1916,
                      2015, 2062, 2925, 2901, 2900, 1932, 1934, 2066, 2899, 2903, 1935})
            set(t, Kind::Trigger, 0, 0);
    }
};

const Table& table() {
    static const Table t;
    return t;
}

} // namespace

const ObjectInfo& objectInfo(int id) {
    static const ObjectInfo unknown{};
    if (id <= 0 || id >= kMaxId) return unknown;
    return table().info[size_t(id)];
}

} // namespace al::level
