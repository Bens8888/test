// Flying gamemodes (ship, ufo, wave, swing): inputs from the clicks, the intended path simulated,
// then a tube carved around it (blocks outside the path with difficulty-dependent clearance, plus
// spikes/saws by pattern family). Dual parts carve around both players' paths.
#include <al/gen/builder.hpp>

#include "pattern_params.hpp"

#include <algorithm>
#include <cmath>

namespace al::gen {

using level::LevelObject;
namespace oid = level::id;
using namespace pp;

namespace {

const PatternDef* pickFamily(Builder& b, const Part& part, const std::vector<int>& clicks) {
    auto cands = patternsFor(part.mode, part.difficulty);
    if (cands.empty()) return nullptr;
    PatternContext ctx;
    ctx.mode = part.mode;
    ctx.intensity = part.intensity;
    ctx.difficulty = part.difficulty;
    ctx.intent = part.intent;
    ctx.speed = part.speed;
    ctx.mini = part.mini;
    ctx.style = b.plan().style;
    float strength = 0;
    for (int ci : clicks) strength += b.plan().clicks[size_t(ci)].strength;
    ctx.strength = clicks.empty() ? 0.5f : strength / float(clicks.size());
    std::vector<float> w;
    RuleStyleModel rules;
    (b.styleModel() ? b.styleModel() : &rules)->score(ctx, cands, w);
    int pick = b.rng().weighted(w);
    return pick >= 0 ? cands[size_t(pick)] : cands.front();
}

} // namespace

bool buildFlyingPart(Builder& b, const Part& part, int attempt) {
    const auto& plan = b.plan();
    const auto& prof = b.profileFor(part);
    const int k0 = Builder::tickOf(part.t0), k1 = Builder::tickOf(part.t1);
    for (int i = 0; i < attempt * 5; ++i) b.rng().next();
    // settle: until the player is in this mode
    {
        phys::GameState s = b.cursor();
        int limit = std::min(k1 - 10, k0 + 90);
        while (s.tick < limit && s.p[0].mode != part.mode) if (!b.runTo(s, s.tick + 1)) return false;
        if (s.p[0].mode != part.mode) { debugLog("  flying: player never entered the mode (mode %d at tick %d)", int(s.p[0].mode), s.tick); return false; }
        b.setCursor(s);
    }
    const phys::PlayerState p0 = b.cursor().p[0];
    const float F = p0.bounded ? p0.floorY : 0.0f;
    const float C = p0.bounded ? p0.ceilY : 300.0f;
    const PatternDef* fam = pickFamily(b, part, part.clicks);
    const int family = fam ? fam->a : FlOpen;
    const bool dense = fam ? fam->b == 1 : false;
    const float h = p0.height();

    // ---- inputs from clicks
    const int startTick = b.cursor().tick;
    std::vector<int> clickEv;
    const bool toggleMode = part.mode == Mode::Wave || part.mode == Mode::Swing;
    const float margin = std::max(25.0f, prof.clearance + h);
    const float bandLo = F + margin, bandHi = C - margin;
    // Timing: every input needs a window of minWin ticks, i.e. room of about half of it to both
    // neighbouring inputs.
    const int minWin = std::max(4, int(std::ceil(prof.minWindowMs * phys::kTPS / 1000.0)) - attempt / 3);
    const int halfWin = minWin / 2 + 1;
    const bool taps = part.mode == Mode::Ufo || part.mode == Mode::Swing; // press + release 5 ticks later
    const int gapT = halfWin + 2 + (taps ? 5 : 0);
    // Forward pass: lay the click inputs down in time order. For wave/swing, also insert a toggle
    // whenever the path would leave the band [bandLo, bandHi] (snapped to the 16th-note grid when
    // that is close), so the path never needs the floor/ceiling. Toggles keep gapT away from the
    // musical clicks: a toggle needed right after a click means the click is cancelled at once, so
    // that click is dropped instead; a toggle needed right before a click is moved earlier.
    phys::GameState sim = b.cursor();
    std::vector<phys::GameState> history; // recent states for rewinding to a toggle tick
    int lastPressTick = -1000;
    struct LastClick { bool valid = false; int tick = 0; size_t slot = 0; size_t events = 0; phys::GameState state; } lastClick;
    auto movingUp = [&](const phys::PlayerState& p) {
        if (p.mode == Mode::Wave) return p.holding ? !p.upsideDown : p.upsideDown;
        if (p.mode == Mode::Swing) return p.upsideDown; // gravity flips at once, the velocity follows later
        return p.v * p.flip() > 0;
    };
    auto rewindTo = [&](int t) {
        // latest remembered state strictly before tick t
        for (auto it = history.rbegin(); it != history.rend(); ++it)
            if (it->tick < t) { sim = *it; history.erase(it.base(), history.end()); return true; }
        return false;
    };
    auto toggleAt = [&](int t) {
        if (part.mode == Mode::Wave) {
            if (b.heldAfterInputs()) b.release(t); else b.press(t);
        } else { // swing: a tap flips
            if (b.heldAfterInputs()) b.release(t - 1);
            b.press(t);
            b.release(t + 5);
        }
        lastPressTick = t;
        lastClick.valid = false;
    };
    auto advanceBand = [&](int untilTick, int nextClickTick) -> bool {
        while (sim.tick < untilTick) {
            phys::GameState before = sim;
            if (!b.runTo(sim, sim.tick + 1)) return false;
            history.push_back(before);
            if (history.size() > 96) history.erase(history.begin());
            if (!toggleMode || sim.p[0].mode != part.mode) continue;
            if (sim.tick <= b.lastInputTick() + 1) continue; // a scheduled input (e.g. a tap release) is still pending
            float y = sim.p[0].y;
            bool up = movingUp(sim.p[0]);
            if (!((y > bandHi && up) || (y < bandLo && !up))) continue;
            int t = sim.tick;
            if (lastClick.valid && t - lastClick.tick < gapT) {
                // the click just placed sends the player out of the band. The band is soft: if the
                // player stays clear of the hard bounds, the corrective toggle simply waits.
                const int tw = lastClick.tick + gapT;
                phys::GameState q = sim;
                bool clear = true;
                const float hardLo = F + h * 0.5f + 4.0f, hardHi = C - h * 0.5f - 4.0f;
                while (q.tick < tw - 1 && clear) {
                    if (!b.runTo(q, q.tick + 1)) clear = false;
                    else if (q.p[0].y < hardLo || q.p[0].y > hardHi) clear = false;
                }
                if (clear && (nextClickTick < 0 || nextClickTick - tw >= gapT)) {
                    while (sim.tick < tw - 1) {
                        history.push_back(sim);
                        if (history.size() > 96) history.erase(history.begin());
                        b.runTo(sim, sim.tick + 1);
                    }
                    toggleAt(tw);
                    continue;
                }
                // otherwise the click is cancelled at once: drop it
                auto m = b.mark();
                m.inputEvents = lastClick.events;
                b.rollback(m);
                clickEv[lastClick.slot] = -1;
                sim = lastClick.state;
                history.clear();
                lastClick.valid = false;
                lastPressTick = b.inputs().events.empty() ? -1000 : b.lastInputTick();
                continue;
            }
            double snapped = b.music().snapToGrid(Builder::timeOf(t), 4);
            int st = Builder::tickOf(snapped);
            if (st < t && st > t - 14) t = st;
            if (nextClickTick > 0 && nextClickTick - t < gapT) t = std::min(t, nextClickTick - gapT);
            t = std::max(t, std::max(lastPressTick + gapT, b.lastInputTick() + 1));
            if (!rewindTo(t)) t = std::max(sim.tick, b.lastInputTick() + 1);
            toggleAt(t);
            if (sim.tick >= t) rewindTo(t);
        }
        return true;
    };
    if (part.mode == Mode::Wave) {
        bool wantUp = p0.y < (F + C) * 0.5f;
        if (wantUp != b.heldAfterInputs()) { if (wantUp) b.press(startTick + 2); else b.release(startTick + 2); }
    }
    for (int ci : part.clicks) {
        const Click& ck = plan.clicks[size_t(ci)];
        int k = Builder::tickOf(ck.time);
        if (!advanceBand(k - 1, k)) return false;
        if (k <= b.lastInputTick() + 1 || k <= startTick + 2 || k < lastPressTick + gapT) { clickEv.push_back(-1); continue; }
        lastClick = {true, k, clickEv.size(), b.inputs().events.size(), sim};
        lastPressTick = k;
        switch (part.mode) {
            case Mode::Wave:
                if (b.heldAfterInputs()) b.release(k); else b.press(k);
                clickEv.push_back(int(b.inputs().events.size()) - 1);
                break;
            case Mode::Ship: {
                if (b.heldAfterInputs()) b.release(k - 1);
                b.press(k);
                clickEv.push_back(int(b.inputs().events.size()) - 1);
                int rel = std::max(k + 4, Builder::tickOf(ck.release));
                b.release(rel);
                break;
            }
            default: // ufo, swing: taps
                if (b.heldAfterInputs()) b.release(k - 1);
                b.press(k);
                clickEv.push_back(int(b.inputs().events.size()) - 1);
                b.release(k + 5);
                break;
        }
    }
    if (!advanceBand(std::max(startTick + 2, k1 - 30), -1)) return false;

    // ---- the intended path plus its timing variants (the tube is carved around all of them, so
    // every input keeps at least the required window by construction)
    std::vector<PathSample> path;
    const int endTick = std::max(startTick + 2, k1 - 30);
    if (!b.pathEnvelope(startTick, endTick, halfWin, part.mode, path)) { debugLog("  flying: intended path died"); return false; }
    if (path.empty()) { debugLog("  %s: empty path", "flying"); return false; }
    auto nextPressAfter = [&](size_t ev) { return b.nextPressAfter(int(ev)); };

    // ---- carve the tube (30-unit columns)
    const float widen = 1.0f + 0.2f * float(attempt);
    float clearance = prof.clearance * widen * (family == FlTight ? 0.75f : (family == FlOpen ? 1.35f : 1.0f));
    clearance = std::max(clearance, 4.0f);
    const float q = part.difficulty >= 3 ? 15.0f : 30.0f;
    float xMin = path.front().x, xMax = path.front().x;
    for (auto& sm : path) { xMin = std::min(xMin, sm.x); xMax = std::max(xMax, sm.x); }
    const float c0 = std::floor((xMin + 60.0f) / 30.0f) * 30.0f;  // leave the first 2 blocks after the portal open
    const int ncols = std::max(0, int((xMax - 30.0f - c0) / 30.0f));
    struct Col { float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f}; };
    std::vector<Col> cols(static_cast<size_t>(ncols));
    for (auto& sm : path) {
        float hw = sm.half; // square hitbox
        int a = int(std::floor((sm.x - hw - c0) / 30.0f)), bcol = int(std::floor((sm.x + hw - c0) / 30.0f));
        for (int c = std::max(0, a); c <= std::min(ncols - 1, bcol); ++c) {
            cols[size_t(c)].lo[sm.player] = std::min(cols[size_t(c)].lo[sm.player], sm.y - sm.half);
            cols[size_t(c)].hi[sm.player] = std::max(cols[size_t(c)].hi[sm.player], sm.y + sm.half);
        }
    }
    // smooth: widen each column's range to its neighbours (prevents single-block notches)
    std::vector<Col> sm = cols;
    for (int c = 0; c < ncols; ++c)
        for (int d = -1; d <= 1; ++d) {
            int j = c + d;
            if (j < 0 || j >= ncols) continue;
            for (int pl = 0; pl < 2; ++pl) {
                sm[size_t(c)].lo[pl] = std::min(sm[size_t(c)].lo[pl], cols[size_t(j)].lo[pl]);
                sm[size_t(c)].hi[pl] = std::max(sm[size_t(c)].hi[pl], cols[size_t(j)].hi[pl]);
            }
        }
    const bool dual = b.cursor().dual || part.dual;
    int placedBlocks = 0;
    for (int c = 0; c < ncols; ++c) {
        float x = c0 + 15.0f + 30.0f * float(c);
        // collect tube intervals in this column
        std::vector<std::pair<float, float>> tubes;
        for (int pl = 0; pl < (dual ? 2 : 1); ++pl) {
            if (sm[size_t(c)].lo[pl] > sm[size_t(c)].hi[pl]) continue;
            float lo = std::floor((sm[size_t(c)].lo[pl] - clearance) / q) * q;
            float hi = std::ceil((sm[size_t(c)].hi[pl] + clearance) / q) * q;
            tubes.push_back({lo, hi});
        }
        if (tubes.empty()) continue;
        std::sort(tubes.begin(), tubes.end());
        // merge overlapping tubes
        std::vector<std::pair<float, float>> merged;
        for (auto& t : tubes) {
            if (!merged.empty() && t.first <= merged.back().second + 2 * q) merged.back().second = std::max(merged.back().second, t.second);
            else merged.push_back(t);
        }
        // solid regions: [F, first.lo], between tubes, [last.hi, C]; two rows deep at most
        std::vector<std::pair<float, float>> solids;
        if (merged.front().first > F + 0.5f) solids.push_back({std::max(F, merged.front().first - 60.0f), merged.front().first});
        for (size_t i = 0; i + 1 < merged.size(); ++i) solids.push_back({merged[i].second, merged[i + 1].first});
        if (merged.back().second < C - 0.5f) solids.push_back({merged.back().second, std::min(C, merged.back().second + 60.0f)});
        for (auto [y0, y1] : solids) {
            if (y1 - y0 < 14.0f) continue;
            // 30-tall blocks aligned to the tube edge side, 15x15 squares to fill the rest
            bool bottomSolid = y1 <= merged.front().first + 0.5f; // below the (first) tube
            float yy0 = y0, yy1 = y1;
            if (bottomSolid) {
                float top = yy1;
                while (top - yy0 >= 29.9f) { b.place(Builder::obj(oid::Block, x, top - 15.0f)); top -= 30.0f; ++placedBlocks; }
                if (top - yy0 >= 14.9f) {
                    b.place(Builder::obj(195, x - 7.5, top - 7.5));
                    b.place(Builder::obj(195, x + 7.5, top - 7.5));
                    placedBlocks += 2;
                }
            } else {
                float bot = yy0;
                while (yy1 - bot >= 29.9f) { b.place(Builder::obj(oid::Block, x, bot + 15.0f)); bot += 30.0f; ++placedBlocks; }
                if (yy1 - bot >= 14.9f) {
                    b.place(Builder::obj(195, x - 7.5, bot + 7.5));
                    b.place(Builder::obj(195, x + 7.5, bot + 7.5));
                    placedBlocks += 2;
                }
            }
        }
        // hazards on the tube edges (spiked / saw families), only where safely clear of the path
        bool wantHazard = (family == FlSpiked && (dense || c % 3 == 0)) || (family == FlSaw && c % (dense ? 4 : 6) == 2) ||
                          (family == FlTight && dense && c % 4 == 1);
        if (wantHazard && part.mode != Mode::Wave) {
            float pathLo = 1e9f, pathHi = -1e9f;
            for (int pl = 0; pl < (dual ? 2 : 1); ++pl) { pathLo = std::min(pathLo, sm[size_t(c)].lo[pl]); pathHi = std::max(pathHi, sm[size_t(c)].hi[pl]); }
            float floorTop = merged.front().first, ceilBottom = merged.back().second;
            if (family == FlSaw) {
                float r = 12.0f;
                if (floorTop - F > 20 && floorTop + r < pathLo - 6) b.place(Builder::obj(oid::SawSmall, x, floorTop));
                else if (C - ceilBottom > 20 && ceilBottom - r > pathHi + 6) b.place(Builder::obj(oid::SawSmall, x, ceilBottom));
            } else {
                if (floorTop + 12.0f < pathLo - 3.0f) b.place(Builder::obj(oid::Spike, x, floorTop + 15.0f));
                if (ceilBottom - 12.0f > pathHi + 3.0f) b.place(Builder::obj(oid::Spike, x, ceilBottom - 15.0f, 180));
            }
        }
    }
    (void)placedBlocks;

    // ---- verify the part and measure windows
    {
        phys::GameState v = b.cursor();
        if (!b.runTo(v, endTick)) { debugLog("  flying: verification died at tick %d x=%.0f y=%.0f (%s)", v.tick, v.p[0].x, v.p[0].y, phys::deathCauseName(v.p[0].deathCause)); return false; }
    }
    int tooTight = 0;
    for (size_t i = 0; i < part.clicks.size(); ++i) {
        ClickResult r;
        r.click = part.clicks[i];
        r.pattern = fam ? fam->id : "open";
        int ev = i < clickEv.size() ? clickEv[i] : -1;
        if (ev < 0) {
            r.dropped = true;
            b.addClickResult(r);
            continue;
        }
        r.pressEvent = ev;
        int tick = b.inputs().events[size_t(ev)].tick;
        // windows need a cursor before the event: use a state ~40 ticks before it
        phys::GameState saved = b.cursor();
        phys::GameState pre = saved;
        b.runTo(pre, std::max(saved.tick, tick - 40));
        b.setCursor(pre);
        b.window(ev, std::min(endTick, nextPressAfter(size_t(ev)) + 12), 36, r.windowEarly, r.windowLate);
        r.forced = b.necessary(ev, std::min(endTick, tick + 120));
        b.setCursor(saved);
        phys::GameState at = pre;
        b.runTo(at, tick);
        r.x = at.p[0].x;
        if (r.windowEarly + r.windowLate + 1 < minWin) {
            ++tooTight;
            if (debugEnabled()) {
                // why: try the first failing shift and report the death
                for (int sign : {-1, 1}) {
                    int sh = sign * ((sign < 0 ? r.windowEarly : r.windowLate) + 1);
                    bot::InputTrack mod;
                    mod.events = b.inputs().events;
                    mod.events[size_t(ev)].tick += sh;
                    if (size_t(ev) + 1 < mod.events.size() && !mod.events[size_t(ev) + 1].press && mod.events[size_t(ev) + 1].tick - tick <= 8) mod.events[size_t(ev) + 1].tick += sh;
                    phys::GameState q = pre;
                    bool alive = b.runToWithInputs(q, std::min(endTick, nextPressAfter(size_t(ev)) + 12), mod);
                    debugLog("    tight: click tick %d early %d late %d | shift %d: %s at tick %d x=%.0f y=%.0f (%s) prev ev %d next ev %d", tick, r.windowEarly, r.windowLate, sh, alive ? "alive" : "dies", q.tick, q.p[0].x, q.p[0].y, phys::deathCauseName(q.p[0].deathCause), ev > 0 ? b.inputs().events[size_t(ev - 1)].tick : -1, size_t(ev) + 1 < b.inputs().events.size() ? b.inputs().events[size_t(ev) + 1].tick : -1);
                }
            }
        }
        b.addClickResult(r);
        b.memory().uses.push_back({r.pattern, part.index, 0, plan.clicks[size_t(r.click)].time});
    }
    if (tooTight > 0 && attempt + 2 < b.options().maxRetriesPerSection) { debugLog("  flying: %d clicks with a too tight window", tooTight); return false; }
    return b.advanceCursor(endTick);
}

} // namespace al::gen
