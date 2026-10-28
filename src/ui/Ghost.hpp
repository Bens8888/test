#pragma once
// Ghost playback in the editor: the bot's path through the generated level drawn in the level
// (world space, on the editor's object layer), clicks marked, and an animated player icon that
// runs along the path in real time.
#include <Geode/Geode.hpp>

#include <al/bot/bot.hpp>
#include <al/level/level.hpp>

namespace al::mod::ui {

// GD stores object y in the level string 90 units below the node position (ground offset).
constexpr float kEditorYOffset = 90.f;

bool showGhost(LevelEditorLayer* lel, level::Level const& lvl, bot::InputTrack const& inputs, float xOffset = 0.f);
void hideGhost(LevelEditorLayer* lel);
bool ghostVisible(LevelEditorLayer* lel);
// Animate a player icon along the drawn path (real time). Stops at the end or when hidden.
void playGhost(LevelEditorLayer* lel);

// "Watch it build": the camera follows a player icon along the designed path while the layout is
// drawn around it (orbs, pads and portals pop in just ahead, then the structures). An overlay
// only: nothing is added to the level until Apply.
bool watchBuild(LevelEditorLayer* lel, level::Level const& lvl, bot::InputTrack const& inputs);

} // namespace al::mod::ui
