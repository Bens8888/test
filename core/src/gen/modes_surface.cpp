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
        while (s.tick < limit && (s.p[0].mode != part.mode || !s.p[0].onGround)) if (!b.runTo(s, s.tick + 1)) return false;
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
    const float floorY = F + std::floor((C - F - H) / 2.0f / 30.0f) * 30.0f;
    const float ceilY = floorY + H;

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
    const float xEnd = probe.p[0].x;
    for (float x = xStart + 15; x < xEnd; x += 30) {
        if (floorY > F + 0.5f) b.place(Builder::obj(oid::Block, x, floorY - 15));
        if (ceilY < C - 0.5f) b.place(Builder::obj(oid::Block, x, ceilY + 15));
    }
    // ---- intended path
    std::vector<std::pair<float, float>> floorContact, ceilContact; // x intervals where the player touches
    std::vector<float> xs, ys;
    s = b.cursor();
    while (s.tick < endTick) {
        if (!b.runTo(s, s.tick + 1)) return false;
        xs.push_back(s.p[0].x);
        ys.push_back(s.p[0].y);
    }
    const float half = h * 0.5f;
    // hazards on the surfaces wherever the path is far from them
    const float step = dense ? 30.0f : 60.0f;
    auto clearOfPath = [&](float x, float yLo, float yHi) {
        for (size_t i = 0; i < xs.size(); ++i) {
            if (std::fabs(xs[i] - x) > half + 6.0f) continue;
            if (ys[i] + half > yLo - 3.0f && ys[i] - half < yHi + 3.0f) return false;
        }
        return true;
    };
    for (float x = xStart + 75; x < xEnd - 30; x += step) {
        if (family == SuGaps && int(x / 30) % 4 == 0) continue;
        int spikeId = family == SuBlocks ? oid::Block : oid::Spike;
        if (spikeId == oid::Spike) {
            if (clearOfPath(x, floorY, floorY + 12.0f)) b.place(Builder::obj(oid::Spike, x, floorY + 15.0f));
            if (clearOfPath(x, ceilY - 12.0f, ceilY)) b.place(Builder::obj(oid::Spike, x, ceilY - 15.0f, 180));
        } else {
            // bumps of half blocks the player must avoid by being on the other side
            if (clearOfPath(x, floorY, floorY + 15.0f)) { b.place(Builder::obj(195, x - 7.5, floorY + 7.5)); b.place(Builder::obj(oid::Spike, x + 7.5, floorY + 15.0f + 0.0f)); }
            if (clearOfPath(x, ceilY - 15.0f, ceilY)) { b.place(Builder::obj(195, x - 7.5, ceilY - 7.5)); b.place(Builder::obj(oid::Spike, x + 7.5, ceilY - 15.0f, 180)); }
        }
    }
    // ---- verify and measure
    {
        phys::GameState v = b.cursor();
        if (!b.runTo(v, endTick)) { debugLog("  surface: verification died at tick %d x=%.0f y=%.0f (%s)", v.tick, v.p[0].x, v.p[0].y, phys::deathCauseName(v.p[0].deathCause)); return false; }
    }
    const int minWin = std::max(4, int(std::ceil(prof.minWindowMs * phys::kTPS / 1000.0)) - attempt / 3);
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
        b.window(ev, std::min(endTick, tick + 120), 36, r.windowEarly, r.windowLate);
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
