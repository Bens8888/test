#include <al/gen/profile.hpp>

#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace al::gen {

namespace oid = level::id;
using level::Kind;
using level::Mode;

namespace {

// Actions a gamemode can take at a click (for the "more or less often than usual" factor).
int actionCount(int mode) {
    switch (Mode(mode)) {
        case Mode::Cube: case Mode::Robot: return 8; // jump + 7 orbs
        case Mode::Ball: case Mode::Spider: return 4;
        case Mode::Ufo: return 4;
        default: return 2; // tap / hold
    }
}

} // namespace

std::string actionForObject(int id) {
    switch (id) {
        case oid::OrbYellow: return "orb-yellow";
        case oid::OrbPink: return "orb-pink";
        case oid::OrbRed: return "orb-red";
        case oid::OrbBlue: return "orb-blue";
        case oid::OrbGreen: return "orb-green";
        case oid::OrbBlack: return "orb-black";
        case oid::OrbDash: return "orb-dash";
        case oid::OrbDashPink: return "orb-dashpink";
        case oid::OrbSpider: return "orb-spider";
        case oid::PadYellow: return "pad-yellow";
        case oid::PadPink: return "pad-pink";
        case oid::PadRed: return "pad-red";
        case oid::PadBlue: return "pad-blue";
        case oid::PadSpider: return "pad-spider";
        default: return "";
    }
}

void StyleProfile::add(const StyleProfile& o) {
    levels += o.levels;
    for (int m = 0; m < 8; ++m) {
        for (auto& [k, v] : o.actions[size_t(m)]) actions[size_t(m)][k] += v;
        modeSeconds[size_t(m)] += o.modeSeconds[size_t(m)];
        for (int n = 0; n < 8; ++n) transitions[size_t(m)][size_t(n)] += o.transitions[size_t(m)][size_t(n)];
    }
    clicks += o.clicks;
    segments += o.segments;
    seconds += o.seconds;
    hazards += o.hazards;
    saws += o.saws;
    solids += o.solids;
}

double StyleProfile::actionFactor(int mode, const std::string& action) const {
    if (mode < 0 || mode >= 8) return 1.0;
    const auto& a = actions[size_t(mode)];
    double total = 0;
    for (auto& [k, v] : a) if (k.rfind("pad-", 0) != 0 && k != "gravity") total += v;
    if (total < 5) return 1.0; // too little data for this gamemode
    const int K = actionCount(mode);
    auto it = a.find(action);
    const double c = it == a.end() ? 0.0 : it->second;
    // relative frequency against an even use of the gamemode's actions, with a prior of one use
    const double f = (c + 1.0) / (total + double(K)) * double(K);
    return std::clamp(f, 0.15, 4.0);
}

float StyleProfile::switchRate() const {
    if (segments < 3 || clicks < 20) return -1;
    const double len = clicks / segments; // clicks per gamemode stretch
    return float(std::clamp((20.0 - len) / 18.4, 0.0, 1.0));
}

float StyleProfile::modeFactor(int mode) const {
    double total = 0;
    for (double s : modeSeconds) total += s;
    if (total < 20 || mode < 0 || mode >= 8) return 1.0f;
    const double share = modeSeconds[size_t(mode)] / total;
    return float(std::clamp(std::sqrt(share * 8.0), 0.2, 3.0));
}

float StyleProfile::density() const {
    if (seconds < 20) return -1;
    const double perSecond = (hazards + saws + solids / 10.0) / seconds;
    return float(std::clamp(0.2 + 0.05 * perSecond, 0.2, 0.95));
}

Json StyleProfile::toJson() const {
    Json j = Json::object();
    j["version"] = kVersion;
    j["levels"] = levels;
    Json acts = Json::object();
    for (int m = 0; m < 8; ++m) {
        Json a = Json::object();
        for (auto& [k, v] : actions[size_t(m)]) a[k] = v;
        acts[level::modeName(Mode(m))] = a;
    }
    j["actions"] = acts;
    Json ms = Json::array(), tr = Json::array();
    for (int m = 0; m < 8; ++m) {
        ms.push(Json(modeSeconds[size_t(m)]));
        Json row = Json::array();
        for (int n = 0; n < 8; ++n) row.push(Json(transitions[size_t(m)][size_t(n)]));
        tr.push(row);
    }
    j["modeSeconds"] = ms;
    j["transitions"] = tr;
    j["clicks"] = clicks;
    j["segments"] = segments;
    j["seconds"] = seconds;
    j["hazards"] = hazards;
    j["saws"] = saws;
    j["solids"] = solids;
    return j;
}

std::optional<StyleProfile> StyleProfile::fromJson(const Json& j) {
    if (j.get("version", 0) != kVersion) return std::nullopt;
    StyleProfile p;
    p.levels = j.get("levels", 0);
    for (int m = 0; m < 8; ++m)
        for (auto& [k, v] : j["actions"][level::modeName(Mode(m))].members()) p.actions[size_t(m)][k] = v.asNumber();
    for (int m = 0; m < 8; ++m) {
        p.modeSeconds[size_t(m)] = j["modeSeconds"][size_t(m)].asNumber();
        for (int n = 0; n < 8; ++n) p.transitions[size_t(m)][size_t(n)] = j["transitions"][size_t(m)][size_t(n)].asNumber();
    }
    p.clicks = j.get("clicks", 0.0);
    p.segments = j.get("segments", 0.0);
    p.seconds = j.get("seconds", 0.0);
    p.hazards = j.get("hazards", 0.0);
    p.saws = j.get("saws", 0.0);
    p.solids = j.get("solids", 0.0);
    return p;
}

std::string StyleProfile::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d level(s), %.0f clicks, %.1f clicks per gamemode stretch, switch rate %.2f, density %.2f", levels, clicks,
                  segments > 0 ? clicks / segments : 0.0, double(switchRate()), double(density()));
    return buf;
}

StyleProfile profileFromRun(const level::Level& lvl, const bot::InputTrack& solution) {
    StyleProfile p;
    p.levels = 1;
    phys::World W(lvl);
    bot::RunOptions ro;
    ro.recordTrace = true;
    auto run = bot::run(W, solution, ro);
    const auto& tr = run.trace;
    if (tr.empty()) return p;
    auto objectId = [&](int simIdx) {
        if (simIdx < 0 || simIdx >= int(W.size())) return 0;
        const int li = W.objects()[size_t(simIdx)].levelIndex;
        return li >= 0 && li < int(lvl.objects.size()) ? lvl.objects[size_t(li)].id : 0;
    };
    // clicks: what each press does
    const auto& ev = solution.events;
    for (size_t i = 0; i < ev.size(); ++i) {
        if (!ev[i].press) continue;
        const int t = ev[i].tick;
        const int rel = i + 1 < ev.size() ? ev[i + 1].tick : t + 3;
        if (t - 1 < 0 || t - 1 >= int(tr.size())) continue;
        const auto& st = tr[size_t(t - 1)];
        const int mode = int(st.mode);
        std::string act;
        for (int k = t; k <= t + 3 && k - 1 < int(tr.size()); ++k) {
            const uint32_t e = run.traceEvents[size_t(k - 1)];
            if (e & phys::EvOrb) { act = actionForObject(objectId(tr[size_t(k - 1)].eventObj)); break; }
            if (e & (phys::EvJump | phys::EvTeleport)) { act = "jump"; break; }
        }
        if (act.empty()) {
            if (st.isFlying()) act = rel - t > 8 ? "hold" : "tap";
            else continue; // a press that did nothing (buffered or spare)
        }
        p.actions[size_t(mode)][act] += 1;
        p.clicks += 1;
    }
    // between clicks: pads, gravity portals, gamemode stretches
    Mode cur = tr.front().mode;
    p.segments = 1;
    for (size_t k = 0; k < tr.size(); ++k) {
        const auto& st = tr[k];
        p.modeSeconds[size_t(int(st.mode))] += phys::kStepSec;
        const uint32_t e = run.traceEvents[k];
        if (e & phys::EvPad) {
            std::string a = actionForObject(objectId(st.eventObj));
            if (!a.empty()) p.actions[size_t(int(st.mode))][a] += 1;
        }
        if ((e & phys::EvPortal) && level::objectInfo(objectId(st.eventObj)).kind == Kind::GravityPortal) p.actions[size_t(int(st.mode))]["gravity"] += 1;
        if (st.mode != cur) {
            p.transitions[size_t(int(cur))][size_t(int(st.mode))] += 1;
            p.segments += 1;
            cur = st.mode;
        }
    }
    p.seconds = double(tr.size()) / phys::kTPS;
    for (auto& o : lvl.objects) {
        auto k = o.info().kind;
        if (k == Kind::Hazard || k == Kind::HazardSlope) p.hazards += 1;
        else if (k == Kind::Saw) p.saws += 1;
        else if (k == Kind::Solid || k == Kind::Slope) p.solids += 1;
    }
    return p;
}

bool profileFromLevel(const level::Level& lvl, StyleProfile& out, std::string& why, int solveBeam, int maxSeconds) {
    if (lvl.settings.platformer) { why = "platformer level"; return false; }
    if (lvl.settings.twoPlayer) { why = "2-player level"; return false; }
    int gameplay = 0;
    for (auto& o : lvl.objects) {
        auto k = o.info().kind;
        if (k != Kind::Deco && k != Kind::Trigger) ++gameplay;
    }
    if (gameplay < 30) { why = "too few gameplay objects"; return false; }
    phys::World W(lvl);
    bot::SolveOptions so;
    so.beamWidth = solveBeam;
    so.maxTicks = maxSeconds * phys::kTPS;
    auto res = bot::autoSolve(W, so);
    if (!res.solved) { why = "the bot could not beat the level (stuck at x " + std::to_string(int(res.bestX)) + ")"; return false; }
    out = profileFromRun(lvl, res.inputs);
    if (out.clicks < 10) { why = "fewer than 10 clicks"; return false; }
    return true;
}

} // namespace al::gen
