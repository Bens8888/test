#pragma once
// Object database: what each GD 2.2 object ID is for gameplay purposes (kind, hitbox) and
// the IDs the generator uses. Sources: docs/research/level_format.md, physics_spec.md §8.
#include <cstdint>

namespace al::level {

enum class Kind : uint8_t {
    Deco,           // no gameplay effect (including triggers we don't simulate)
    Solid,
    Breakable,
    Hazard,         // rectangular hazard
    Saw,            // circular hazard (radius = w)
    Orb,
    Pad,
    ModePortal,
    GravityPortal,
    SizePortal,
    SpeedPortal,
    DualPortal,
    MirrorPortal,
    TeleportPortal,
    Slope,
    HazardSlope,
    StartPos,
    Trigger,
    Coin,
    EndTrigger,
};

enum class Mode : uint8_t { Cube = 0, Ship, Ball, Ufo, Wave, Robot, Spider, Swing, Count };
const char* modeName(Mode m);

enum class OrbType : uint8_t { Yellow, Pink, Red, Blue, Green, Black, DashGreen, DashPink, Spider, Teleport, Toggle };
enum class PadType : uint8_t { Yellow, Pink, Red, Blue, Spider };
enum class GravityPortalType : uint8_t { Normal, Flip, Toggle };

struct ObjectInfo {
    int id = 0;
    Kind kind = Kind::Deco;
    uint8_t sub = 0;       // OrbType / PadType / Mode / speed index / GravityPortalType / size(0 normal,1 mini) / dual(1 on, 0 off)
    float w = 0, h = 0;    // hitbox size before scale (Saw: w = radius)
    bool slopeWide = false; // 60x30 slope
};

// Returns the info for an object ID (Kind::Deco with zero hitbox for unknown IDs).
const ObjectInfo& objectInfo(int id);

// Speed index (0 = 0.5x, 1 = 1x, 2 = 2x, 3 = 3x, 4 = 4x) <-> header kA4 value (0 = 1x, 1 = 0.5x, ...).
inline int speedFromKA4(int v) { return v == 0 ? 1 : (v == 1 ? 0 : (v >= 2 && v <= 4 ? v : 1)); }
inline int speedToKA4(int s) { return s == 1 ? 0 : (s == 0 ? 1 : s); }

// IDs used by the generator.
namespace id {
constexpr int Block = 1, BlockTop = 2, BlockCorner = 3, BlockInnerCorner = 4, BlockTopPillar = 6, BlockPillar = 7;
constexpr int Spike = 8, HalfSpike = 39, MediumSpike = 103, GroundSpikes = 9, TinySpike = 392;
constexpr int Slab = 40;
constexpr int SawLarge = 1705, SawMedium = 1706, SawSmall = 1707;
constexpr int OrbYellow = 36, OrbPink = 141, OrbRed = 1333, OrbBlue = 84, OrbGreen = 1022, OrbBlack = 1330;
constexpr int OrbDash = 1704, OrbDashPink = 1751, OrbSpider = 3004, OrbTeleport = 3027;
constexpr int PadYellow = 35, PadPink = 140, PadRed = 1332, PadBlue = 67, PadSpider = 3005;
constexpr int PortalCube = 12, PortalShip = 13, PortalBall = 47, PortalUfo = 111, PortalWave = 660, PortalRobot = 745, PortalSpider = 1331, PortalSwing = 1933;
constexpr int PortalGravNormal = 10, PortalGravFlip = 11, PortalGravToggle = 2926;
constexpr int PortalSizeNormal = 99, PortalSizeMini = 101;
constexpr int PortalDualOn = 286, PortalDualOff = 287;
constexpr int Speed05 = 200, Speed1 = 201, Speed2 = 202, Speed3 = 203, Speed4 = 1334;
constexpr int StartPos = 31, Coin = 1329;
// Colored (channel-assignable) square set 207-213: outer pieces + inner fill
constexpr int ColBlock = 207, ColTop = 208, ColCorner = 209, ColInnerCorner = 210, ColInner = 211 /*deco*/, ColTopPillar = 212, ColPillar = 213;
constexpr int ColSlab = 215, ColSmallSlab = 219, ColSmallSquare = 220;
constexpr int ColGradientSlab = 1903;
// Outline set (2.0)
constexpr int OutlineSquare = 467, OutlineTop = 468, OutlineCorner = 469, OutlineTopPillar = 470, OutlinePillar = 471;
// Triggers
constexpr int TrigColor = 899, TrigMove = 901, TrigPulse = 1006, TrigAlpha = 1007, TrigToggle = 1049, TrigSpawn = 1268, TrigRotate = 1346;
constexpr int TrigShake = 1520, TrigCamZoom = 1913, TrigCamStatic = 1914, TrigCamOffset = 1916, TrigCamRotate = 2015, TrigEnd = 3600;
// Deco
constexpr int GlowStraight = 503, GlowCorner = 504, GlowInner = 505;
constexpr int DecoSpikeL = 18, DecoSpikeM = 19, DecoSpikeS = 20, DecoSpikeXS = 21;
} // namespace id

} // namespace al::level
