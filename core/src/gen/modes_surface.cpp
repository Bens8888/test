// Surface gamemodes (ball, spider): the player switches between a floor and a ceiling on every
// click. A corridor of floor/ceiling blocks sized so the switches keep up with the rhythm, then
// hazards everywhere the intended path does not touch (so late/early/missing clicks die).
#include <al/gen/builder.hpp>

#include "pattern_params.hpp"

#include <algorithm>
#include <cmath>

namespace al::gen {

using level::LevelObject;
namespace oid = level::id;
using namespace pp;

bool buildSurfacePart(Builder& b, const Part& part, int attempt) {
    const auto& plan = b.plan();
    const auto& prof = b.profileFor(part);
    const int k0 = Builder::tickOf(part.t0), k1 = Builder::tickOf(part.t1);
    for (int i = 0; i < attempt * 7; ++i) b.rng().next();
    {
        phys::GameState s = b.cursor();
        int limit = std::min(k1 - 10, k0 + 90);
        while (s.tick < limit && (s.p[0].mode != part.mode || !s.p[0].onGround))
            if (!b.runTo(s, s.tick + 1)) { debugLog("  surface: died while settling at tick %d x=%.0f y=%.0f (%s)", s.tick, s.p[0].x, s.p[0].y, phys::deathCauseName(s.p[0].deathCause)); return false; }
        if (s.p[0].mode != part.mode) { debugLog("  surface: player never entered the mode (mode %d at tick %d)", int(s.p[0].mode), s.tick); return false; }
        b.setCursor(s);
    }
    const phys::PlayerState p0 = b.cursor().p[0];
    const float F = p0.bounded ? p0.floorY : 0.0f;
    const float C = p0.bounded ? p0.ceilY : 240.0f;
    // family
    auto cands = patternsFor(part.mode, part.difficulty);
    PatternContext ctx;
    ctx.mode = part.mode;
    ctx.intensity = part.intensity;
    ctx.difficulty = part.difficulty;
    ctx.intent = part.intent;
    ctx.style = plan.style;
    ctx.speed = part.speed;
    std::vector<float> w;
    RuleStyleModel rules;
    (b.styleModel() ? b.styleModel() : &rules)->score(ctx, cands, w);
    int pick = b.rng().weighted(w);
    const PatternDef* fam = pick >= 0 ? cands[size_t(pick)] : nullptr;
    const int family = fam ? fam->a : SuSpikes;
    const bool dense = fam ? fam->b == 1 : false;

    // corridor height: the switch must finish before the next click
    double minGap = 10;
    for (size_t i = 1; i < part.clicks.size(); ++i)
        minGap = std::min(minGap, plan.clicks[size_t(part.clicks[i])].time - plan.clicks[size_t(part.clicks[i - 1])].time);
    const float h = p0.height();
    float H = C - F;
    if (part.mode == Mode::Ball) {
        // ball switch: ~98 ticks for 210 units at 1x (travel ~ sqrt(distance)); pick the tallest that fits
        for (float cand : {240.0f, 180.0f, 150.0f, 120.0f, 90.0f}) {
            double travel = (98.0 / 240.0) * std::sqrt((cand - h) / 210.0);
            H = cand;
            if (travel < minGap * 0.8) break;
        }
    } else {
        H = std::min(H, 180.0f);
    }
    H = std::min(H, C - F);
    // anchor the corridor on the surface the player stands on
    const float feet = p0.y - p0.flip() * h * 0.5f;
    const float surf = std::round(feet / 30.0f) * 30.0f;
    float floorY, ceilY;
    if (p0.upsideDown) { ceilY = std::clamp(surf, F + H, C); floorY = ceilY - H; }
    else { floorY = std::clamp(surf, F, C - H); ceilY = floorY + H; }
    const bool needFloor = floorY > F + 0.5f;
    const bool needCeil = ceilY < C - 0.5f || !p0.bounded;

    // ---- inputs: taps on the clicks
    const int startTick = b.cursor().tick;
    std::vector<int> clickEv;
    for (int ci : part.clicks) {
        int k = Builder::tickOf(plan.clicks[size_t(ci)].time);
        if (k <= b.lastInputTick() + 1 || k <= startTick + 2) { clickEv.push_back(-1); continue; }
        if (b.heldAfterInputs()) b.release(k - 1);
        b.press(k);
        clickEv.push_back(int(b.inputs().events.size()) - 1);
        b.release(k + 5);
    }
    // ---- corridor blocks (floor and ceiling rows) for the part
    phys::GameState s = b.cursor();
    const int endTick = std::max(startTick + 2, k1 - 30);
    phys::GameState probe = s;
    b.runTo(probe, endTick);
    const float xStart = std::floor((s.p[0].x + 45.0f) / 30.0f) * 30.0f;
    const bool lastPart = part.index + 1 == int(plan.parts.size());
    const float xEnd = probe.p[0].x + (lastPart ? 900.0f : 0.0f); // the last part runs on to the level end
    for (float x = xStart + 15; x < xEnd; x += 30) {
        if (needFloor) b.place(Builder::obj(oid::Block, x, floorY - 15));
        if (needCeil) b.place(Builder::obj(oid::Block, x, ceilY + 15));
    }
    // ---- intended path plus timing variants; hazards go wherever none of them passes
    const int minWin = std::max(4, int(std::ceil(prof.minWindowMs * phys::kTPS / 1000.0)) - attempt / 3);
    const int halfWin = minWin / 2 + 1;
    std::vector<PathSample> env;
    if (!b.pathEnvelope(startTick, endTick, halfWin, part.mode, env)) { debugLog("  surface: intended path died"); return false; }
    const float margin = std::max(3.0f, prof.clearance * 0.25f);
    const float step = dense ? 30.0f : 60.0f;
    int hazards = 0;
    auto tryPlace = [&](const LevelObject& o) {
        if (!clearOf(o, env, margin)) return false;
        b.place(o);
        ++hazards;
        return true;
    };
    for (float x = xStart + 75; x < probe.p[0].x - 30; x += step) {
        if (family == SuGaps && int(x / 30) % 4 == 0) continue;
        if (family == SuBlocks) {
            // bumps: a half block with a spike behind it, on whichever surface is clear
            LevelObject f1 = Builder::obj(195, x - 7.5, floorY + 7.5), f2 = Builder::obj(oid::Spike, x + 7.5, floorY + 15.0f);
            if (clearOf(f1, env, margin) && clearOf(f2, env, margin)) { tryPlace(f1); tryPlace(f2); }
            LevelObject c1 = Builder::obj(195, x - 7.5, ceilY - 7.5), c2 = Builder::obj(oid::Spike, x + 7.5, ceilY - 15.0f, 180);
            if (clearOf(c1, env, margin) && clearOf(c2, env, margin)) { tryPlace(c1); tryPlace(c2); }
        } else {
            tryPlace(Builder::obj(oid::Spike, x, floorY + 15.0f));
            tryPlace(Builder::obj(oid::Spike, x, ceilY - 15.0f, 180));
        }
    }
    debugLog("  surface: %d hazards, corridor %.0f-%.0f", hazards, floorY, ceilY);
    // ---- verify and measure
    {
        phys::GameState v = b.cursor();
        if (!b.runTo(v, endTick)) { debugLog("  surface: verification died at tick %d x=%.0f y=%.0f (%s)", v.tick, v.p[0].x, v.p[0].y, phys::deathCauseName(v.p[0].deathCause)); return false; }
    }
    int tooTight = 0;
    for (size_t i = 0; i < part.clicks.size(); ++i) {
        ClickResult r;
        r.click = part.clicks[i];
        r.pattern = fam ? fam->id : "surface";
        int ev = i < clickEv.size() ? clickEv[i] : -1;
        if (ev < 0) { r.dropped = true; b.addClickResult(r); continue; }
        r.pressEvent = ev;
        int tick = b.inputs().events[size_t(ev)].tick;
        phys::GameState saved = b.cursor();
        phys::GameState pre = saved;
        b.runTo(pre, std::max(saved.tick, tick - 40));
        b.setCursor(pre);
        b.window(ev, std::min(endTick, b.nextPressAfter(ev) + 12), 36, r.windowEarly, r.windowLate);
        r.forced = b.necessary(ev, std::min(endTick, tick + 120));
        b.setCursor(saved);
        phys::GameState at = pre;
        b.runTo(at, tick);
        r.x = at.p[0].x;
        if (r.windowEarly + r.windowLate + 1 < minWin) ++tooTight;
        b.addClickResult(r);
        b.memory().uses.push_back({r.pattern, part.index, 0, plan.clicks[size_t(r.click)].time});
    }
    if (tooTight > 0 && attempt + 2 < b.options().maxRetriesPerSection) { debugLog("  surface: %d clicks with a too tight window", tooTight); return false; }
    return b.advanceCursor(endTick);
}

} // namespace al::gen
