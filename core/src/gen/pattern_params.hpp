#pragma once
// Parameter codes shared by the pattern library (style.cpp) and the gamemode builders.

namespace al::gen::pp {

// Ground modes (cube, robot): PatternDef::a = obstacle, b = terrain, c = special
enum Obstacle { ObNone = 0, ObSpike1, ObSpike2, ObSpike3, ObHalf1, ObHalf2, ObMix2, ObBlock1, ObSlab1, ObSaw, ObCount };
enum Terrain { TrFlat = 0, TrUp1, TrUp2, TrDown1, TrDown2, TrGap2, TrGap3, TrGap4, TrCount };
enum Special { SpNone = 0, SpOrbYellow, SpOrbPink, SpPadYellow, SpCount };

// Flying (ship, ufo, wave, swing): a = family, b = density (0 sparse, 1 dense)
enum Flying { FlOpen = 0, FlTight, FlSpiked, FlSaw, FlStairs, FlCount };

// Surface modes (ball, spider): a = family, b = density
enum Surface { SuSpikes = 0, SuBlocks, SuGaps, SuCount };

} // namespace al::gen::pp
