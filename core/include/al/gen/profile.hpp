#pragma once
// Style profile for generator v2, learned from real levels: what each click does per gamemode
// (jump, which orb, tap or hold), what keeps the player moving between clicks (pads, gravity
// portals), how long gamemode stretches last and which mode follows which, and how dense the
// structures are. Counted on a run that beats the level (the bot's or a recording), so only the
// layout matters: decoration, colours and triggers are ignored.
#include <al/bot/bot.hpp>
#include <al/level/level.hpp>
#include <al/util/json.hpp>

#include <array>
#include <map>
#include <optional>
#include <string>

namespace al::gen {

struct StyleProfile {
    static constexpr int kVersion = 1;
    int levels = 0;
    std::array<std::map<std::string, double>, 8> actions;      // per gamemode: action -> count
    std::array<double, 8> modeSeconds{};
    std::array<std::array<double, 8>, 8> transitions{};       // gamemode -> next gamemode
    double clicks = 0, segments = 0, seconds = 0;
    double hazards = 0, saws = 0, solids = 0;                   // gameplay objects of the levels

    void add(const StyleProfile& o);
    bool empty() const { return levels == 0; }

    // How much more (or less) often than the built-in default an action is used in `mode`
    // (1 = no information). Actions: "jump", "tap", "hold", "orb-yellow" ... "pad-red", "gravity".
    double actionFactor(int mode, const std::string& action) const;
    float switchRate() const;      // 0..1 (<0 = no information)
    float modeFactor(int mode) const;
    float density() const;         // 0..1 (<0 = no information)

    Json toJson() const;
    static std::optional<StyleProfile> fromJson(const Json& j);
    std::string summary() const;
};

// The action name of an orb / pad object id ("orb-yellow", "pad-pink", ...), "" for others.
std::string actionForObject(int id);

// Profile of one level from a run that beats it.
StyleProfile profileFromRun(const level::Level& lvl, const bot::InputTrack& solution);

// Profile of one level: filtered (platformer / 2-player / too short are rejected with a reason),
// solved with the bot.
bool profileFromLevel(const level::Level& lvl, StyleProfile& out, std::string& why, int solveBeam = 256, int maxSeconds = 600);

} // namespace al::gen
