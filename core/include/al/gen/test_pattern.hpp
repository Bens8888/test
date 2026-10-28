#pragma once
// A compact showcase of the object types AutoLayout writes (used to verify editor placement,
// undo and object IDs in-game). The pattern is itself playable: tests check it with the bot.
#include <al/level/level.hpp>

#include <vector>

namespace al::gen {

// Objects of the test pattern starting at x0 (level-string coordinates).
std::vector<level::LevelObject> makeTestPattern(double x0);

} // namespace al::gen
