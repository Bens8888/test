// Ground gamemodes (cube, robot): terrain runs + obstacles placed around the simulated jump arc
// of every click, each placement verified with the simulator (survival, timing window, necessity).
#include <al/gen/builder.hpp>

#include "pattern_params.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace al::gen {

using level::LevelObject;
namespace oid = level::id;
using namespace pp;

namespace {

constexpr float kCell = 30.0f;

inline float snap(float x, float step) { return std::round(x / step) * step; }
inline float cellCenter(float x) { return std::floor(x / kCell) * kCell + 15.0f; }

// A terrain run: the surface the player walks on. Surface = feet height (gravity relative).
struct Run {
    float surf = 0;      // y of the walking surface
    int g = 1;           // gravity sign the run was made for
    float builtTo = 0;   // blocks exist for cells with centre < builtTo
    bool floor = true;   // surface is the level floor (no blocks needed)
};

struct GroundCtx {
    Builder& b;
    const Part& part;
    const DifficultyProfile& prof;
    Run run;
    int maxShift = 36;
    int minWindowTicks = 20;
    std::string prev1, prev2;
    float lastPitch = 0;
    double lastClickTime = -1;
};

// Blocks for the run between builtTo and x (cell centres), filling down to the floor (or up for
// ceilings) for low runs, 2 rows for high runs.
void extendRun(GroundCtx& c, float x) {
    auto& r = c.run;
    if (r.floor) { r.builtTo = std::max(r.builtTo, x); return; }
    float start = cellCenter(r.builtTo + 0.01f);
    if (start < r.builtTo) start += kCell;
    for (float cx = start; cx < x; cx += kCell) {
        int rows;
        if (r.g > 0) {
            int height = int(std::lround(r.surf / kCell));
            rows = height <= 4 ? height : 2;
            for (int k = 0; k < rows; ++k) c.b.place(Builder::obj(oid::Block, cx, r.surf - 15.0f - kCell * k));
        } else {
            for (int k = 0; k < 2; ++k) c.b.place(Builder::obj(oid::Block, cx, r.surf + 15.0f + kCell * k));
        }
        r.builtTo = cx + 15.0f;
    }
}

float hazardY(const Run& r, int id) {
    // centre y of a hazard sitting on the run surface
    float off = id == oid::HalfSpike ? 6.0f : (id == oid::SawSmall ? 0.0f : 15.0f);
    return r.surf + r.g * off;
}

void placeHazard(GroundCtx& c, int id, float x, const Run& r) {
    LevelObject o = Builder::obj(id, x, hazardY(r, id), r.g > 0 ? 0 : 180);
    c.b.place(o);
}

int spikeIdFor(const GenOptions& opt, int obstacle, int idx, Rng& rng);

// A spike pit of `width` cells starting at gapStart in the current run. On the floor the spikes
// sit on the ground; on raised (or ceiling) runs the pit is one row deep with a solid bottom so
// falling in kills quickly instead of after a long fall.
void placePit(GroundCtx& c, float gapStart, int width) {
    auto& r = c.run;
    if (r.floor) {
        extendRun(c, gapStart + width * kCell);
        for (int i = 0; i < width; ++i) placeHazard(c, spikeIdFor(c.b.options(), ObSpike1, i, c.b.rng()), gapStart + 15 + i * kCell, r);
        return;
    }
    extendRun(c, gapStart);
    Run bottom = r;
    bottom.surf = r.surf - r.g * kCell;
    for (int i = 0; i < width; ++i) {
        float cx = gapStart + 15 + i * kCell;
        placeHazard(c, spikeIdFor(c.b.options(), ObSpike1, i, c.b.rng()), cx, bottom);
        int rows = 1;
        if (r.g > 0) {
            int height = int(std::lround(bottom.surf / kCell));
            rows = height <= 4 ? height : 1;
        }
        for (int k = 0; k < rows; ++k) c.b.place(Builder::obj(oid::Block, cx, bottom.surf - r.g * (15.0f + kCell * k)));
    }
    r.builtTo = gapStart + width * kCell;
}

int spikeIdFor(const GenOptions& opt, int obstacle, int idx, Rng& rng) {
    if (obstacle == ObHalf1 || obstacle == ObHalf2) return oid::HalfSpike;
    if (obstacle == ObMix2) return idx == 0 ? oid::Spike : oid::HalfSpike;
    int id = oid::Spike;
    if (opt.spikes == SpikeSet::Colored) id = 216;
    else if (opt.spikes == SpikeSet::Mixed && rng.chance(0.3)) id = 216;
    return id;
}

int obstacleCount(int ob) {
    switch (ob) {
        case ObSpike1: case ObHalf1: case ObBlock1: case ObSlab1: case ObSaw: return 1;
        case ObSpike2: case ObHalf2: case ObMix2: return 2;
        case ObSpike3: return 3;
        default: return 0;
    }
}

// Free arc of a jump pressed at `pressTick` (released after holdTicks) on the current run.
struct Arc {
    bool ok = false;
    int landTick = -1;
    float takeoffX = 0, apexX = 0, landX = 0, apexY = 0;
};

Arc probeArc(GroundCtx& c, int pressTick, int holdTicks, bool orbPress) {
    Arc a;
    auto m = c.b.mark();
    Run saved = c.run;
    phys::GameState s = c.b.cursor();
    c.b.runTo(s, pressTick - 1);
    extendRun(c, s.p[0].x + 420.0f);
    (void)orbPress;
    c.b.press(pressTick);
    c.b.release(pressTick + holdTicks);
    phys::GameState q = c.b.cursor();
    bool alive = c.b.runTo(q, pressTick - 1);
    if (alive) {
        a.takeoffX = q.p[0].x;
        float best = -1e9f;
        bool left = false;
        for (int t = pressTick; t < pressTick + 360; ++t) {
            phys::GameState prev = q;
            if (!c.b.runTo(q, t)) break;
            float feet = (q.p[0].y - c.run.surf) * float(c.run.g);
            if (feet > best) { best = feet; a.apexX = q.p[0].x; a.apexY = feet; }
            if (feet > 2.0f) left = true;
            if (left && (q.p[0].events & phys::EvLand)) { a.landTick = t; a.landX = q.p[0].x; a.ok = true; break; }
            (void)prev;
        }
    }
    c.b.rollback(m);
    c.run = saved;
    return a;
}

struct Attempt {
    bool ok = false;
    int deathTick = -1;
    float deathX = 0, deathY = 0;
    phys::DeathCause cause = phys::DeathCause::None;
    int early = 0, late = 0;
    bool forced = false;
    int landTick = -1;
};

// Evaluate the current placement: survival to the landing, window and necessity.
Attempt evaluateImpl(GroundCtx& c, int pressEv, int pressTick, int nextClickTick);

// Evaluate with temporary look-ahead terrain (the run continues flat after the pattern); the
// temporary blocks are removed again, only the pattern's own geometry is kept.
Attempt evaluate(GroundCtx& c, int pressEv, int pressTick, int nextClickTick) {
    auto m = c.b.mark();
    Run saved = c.run;
    extendRun(c, c.b.cursor().p[0].x + 1000.0f);
    Attempt r = evaluateImpl(c, pressEv, pressTick, nextClickTick);
    c.b.rollback(m);
    c.run = saved;
    return r;
}

Attempt evaluateImpl(GroundCtx& c, int pressEv, int pressTick, int nextClickTick) {
    Attempt r;
    phys::GameState s = c.b.cursor();
    // Invariant: every placement is safe if the player stops clicking after it (survive until the
    // landing), so a later click that cannot be built never leaves the player in a death trap.
    (void)nextClickTick;
    int horizon = pressTick + 360;
    // find landing
    int land = -1;
    bool left = false;
    auto died = [&]() { r.deathTick = s.tick; r.deathX = s.p[0].x; r.deathY = s.p[0].y; r.cause = s.p[0].deathCause; return r; };
    while (s.tick < horizon) {
        if (!c.b.runTo(s, s.tick + 1)) return died();
        if (s.tick < pressTick) continue;
        if (!s.p[0].onGround) left = true;
        if (left && (s.p[0].events & phys::EvLand)) { land = s.tick; break; }
    }
    int check = land > 0 ? std::min(horizon, land + 20) : horizon;
    if (!c.b.runTo(s, check)) return died();
    r.landTick = land;
    c.b.window(pressEv, check, c.maxShift, r.early, r.late);
    // necessity: without the press the player must die soon, judged independently of the next
    // click (its geometry does not exist yet; the look-ahead terrain is plain)
    r.forced = c.b.necessary(pressEv, std::max(check, pressTick + 150));
    r.ok = true;
    return r;
}

bool placeOne(GroundCtx& c, const PatternDef& d, float off, int k, int hold, bool airborne, const Arc& arc) {
    Builder& b = c.b;
    float span = obstacleCount(d.a) * kCell;
    const float h = 30.0f;
    if (!airborne) {
        float apexX = arc.apexX;
        switch (d.b) {
            case TrFlat: {
                float xs = snap(apexX - span / 2 + 15 + off, 15.0f);
                extendRun(c, xs + span - 15);
                for (int i = 0; i < obstacleCount(d.a); ++i) {
                    float x = xs + i * kCell;
                    if (d.a == ObBlock1) b.place(Builder::obj(oid::Block, x, c.run.surf + c.run.g * 15.0f));
                    else if (d.a == ObSlab1) b.place(Builder::obj(oid::Slab, x, c.run.surf + c.run.g * 7.0f, c.run.g > 0 ? 0 : 180));
                    else if (d.a == ObSaw) b.place(Builder::obj(oid::SawSmall, x, c.run.surf));
                    else placeHazard(c, spikeIdFor(b.options(), d.a, i, b.rng()), x, c.run);
                }
                return true;
            }
            case TrUp1: case TrUp2: {
                int steps = d.b == TrUp1 ? 1 : 2;
                float wallX = snap(apexX - 15 + off, 15.0f) - 15.0f;
                float obsEnd = wallX - 15.0f;
                extendRun(c, obsEnd - span);
                for (int i = 0; i < obstacleCount(d.a); ++i) {
                    if (d.a == ObBlock1 || d.a == ObSlab1 || d.a == ObSaw) return false;
                    placeHazard(c, spikeIdFor(b.options(), d.a, i, b.rng()), obsEnd - span + 15 + i * kCell, c.run);
                }
                Run nr;
                nr.g = c.run.g;
                nr.surf = c.run.surf + c.run.g * h * steps;
                nr.floor = false;
                nr.builtTo = wallX;
                c.run = nr;
                extendRun(c, wallX + 30); // the wall itself (the rest of the new run is built lazily)
                return true;
            }
            case TrDown1: case TrDown2: {
                int steps = d.b == TrDown1 ? 1 : 2;
                float ledge = snap(arc.takeoffX + 30 + off, 15.0f);
                extendRun(c, ledge);
                Run nr;
                nr.g = c.run.g;
                nr.surf = c.run.surf - c.run.g * h * steps;
                nr.floor = nr.surf <= 0.5f && nr.g > 0;
                if (nr.floor) nr.surf = 0;
                nr.builtTo = ledge;
                c.run = nr;
                if (d.a == ObBlock1 || d.a == ObSlab1 || d.a == ObSaw) return false;
                // where would the player land if it just ran off the ledge? (temporary terrain)
                float fallX = -1;
                {
                    auto tm = b.mark();
                    Run tr = c.run;
                    extendRun(c, ledge + 420);
                    phys::GameState q = b.cursor();
                    for (int t = q.tick + 1; t < k + 300; ++t) {
                        if (!b.runTo(q, t)) break;
                        if ((q.p[0].events & phys::EvLand) && q.p[0].x > ledge) { fallX = q.p[0].x; break; }
                    }
                    b.rollback(tm);
                    c.run = tr;
                }
                if (fallX < 0) return false;
                int n = std::max(obstacleCount(d.a), int(std::ceil((fallX + 20 - ledge) / kCell)));
                if (n > c.prof.maxSpikes + 2) return false;
                for (int i = 0; i < n; ++i) placeHazard(c, spikeIdFor(b.options(), i < obstacleCount(d.a) ? d.a : ObSpike1, i, b.rng()), ledge + 15 + i * kCell, c.run);
                return true;
            }
            case TrGap2: case TrGap3: case TrGap4: {
                if (d.a == ObBlock1 || d.a == ObSlab1 || d.a == ObSaw) return false;
                int width = d.b == TrGap2 ? 2 : (d.b == TrGap3 ? 3 : 4);
                width += obstacleCount(d.a) > 0 ? 0 : 0;
                float gapStart = std::floor(snap(apexX - width * kCell / 2 + off, 15.0f) / kCell) * kCell;
                if (c.run.builtTo > gapStart + 1) return false; // terrain already exists there
                placePit(c, gapStart, width);
                return true;
            }
            default: return false;
        }
    }
    phys::GameState s = b.cursor();
    b.runTo(s, k);
    // the orb sits on the player's path at the click (offset: the click lands earlier/later in
    // the time the player overlaps it)
    float ox = snap(s.p[0].x + 4 + off * 0.5f, 7.5f), oy = snap(s.p[0].y, 15.0f);
    if (std::fabs(oy - s.p[0].y) > 10) oy = s.p[0].y;
    float lx = 0;
    {
        auto tm = b.mark();
        Run tr = c.run;
        extendRun(c, s.p[0].x + 420);
        phys::GameState q = s;
        int landTick = -1;
        for (int t = k; t < k + 300; ++t) { if (!b.runTo(q, t)) break; if (q.p[0].events & phys::EvLand) { landTick = t; break; } }
        b.rollback(tm);
        c.run = tr;
        if (landTick < 0) return false;
        lx = q.p[0].x;
    }
    int width = d.b == TrGap4 ? 4 : (d.b == TrGap3 ? 3 : 2);
    float gs = std::floor((lx - 45) / kCell) * kCell;
    if (c.run.builtTo <= gs + 1) {
        placePit(c, gs, width);
    } else {
        // the landing area already exists (the previous pattern built it): spikes on the run
        // where the player would land without the orb
        float first = cellCenter(lx - 15.0f);
        extendRun(c, first + width * kCell);
        std::vector<int> near;
        int placed = 0;
        for (int i = 0; i < width; ++i) {
            float cx = first + i * kCell;
            float cy = hazardY(c.run, oid::Spike);
            near.clear();
            b.world().query(cx - 10, cx + 10, near);
            bool occupied = false;
            for (int si : near) {
                const auto& so = b.world().objects()[size_t(si)];
                if (std::fabs(so.y - cy) < 14.0f) occupied = true;
            }
            if (occupied) continue;
            placeHazard(c, spikeIdFor(b.options(), ObSpike1, i, b.rng()), cx, c.run);
            ++placed;
        }
        if (placed == 0) return false;
    }
    b.place(Builder::obj(d.c == SpOrbPink ? oid::OrbPink : oid::OrbYellow, ox, oy));
    (void)hold;
    return true;
}

struct Choice {
    const PatternDef* def = nullptr;
    float offset = 0;
};

PatternContext makeContext(GroundCtx& c, const Click& ck, int nextClickTick) {
    PatternContext ctx;
    ctx.mode = c.part.mode;
    ctx.intensity = c.part.intensity;
    ctx.difficulty = c.part.difficulty;
    ctx.intent = c.part.intent;
    ctx.role = ck.role;
    ctx.strength = ck.strength;
    ctx.gapPrev = c.lastClickTime >= 0 ? ck.time - c.lastClickTime : 2.0;
    ctx.gapNext = nextClickTick > 0 ? Builder::timeOf(nextClickTick) - ck.time : 2.0;
    ctx.speed = c.part.speed;
    ctx.mini = c.part.mini;
    ctx.style = c.b.plan().style;
    ctx.pitchDelta = (ck.pitch > 0 && c.lastPitch > 0) ? ck.pitch - c.lastPitch : 0;
    ctx.hold = ck.hold;
    ctx.motifKey = motifKey(ctx.gapPrev, ctx.gapNext, ck.role, ctx.pitchDelta, ck.role == Role::Downbeat);
    ctx.prevPattern = c.prev1;
    ctx.prevPattern2 = c.prev2;
    return ctx;
}

// Choose a pattern + placement for a click: candidates in the style model's preference order,
// each tried at a few grid offsets; the first candidate with any fair placement wins, and among
// its placements the one whose window is best centred on the musical click is kept.
Choice choose(GroundCtx& c, const PatternContext& ctx, int k, int hold, int nextClickTick, bool airborne, const Arc& arc) {
    Builder& b = c.b;
    auto cands = patternsFor(c.part.mode, c.part.difficulty);
    std::vector<const PatternDef*> filtered;
    for (auto* d : cands) {
        bool orb = d->c == SpOrbYellow || d->c == SpOrbPink;
        if (d->c == SpPadYellow) continue;            // pads are fillers, not click patterns
        if (airborne != orb) continue;                // airborne clicks need an orb
        if (orb && !c.prof.orbs) continue;
        if (d->a == ObSpike3 && !c.prof.tripleSpikes) continue;
        if (d->a <= ObMix2 && obstacleCount(d->a) > c.prof.maxSpikes) continue;
        if (c.run.g < 0 && (d->b == TrUp1 || d->b == TrUp2)) continue; // ceilings: no steps up
        if (c.run.floor && (d->b == TrDown1 || d->b == TrDown2)) continue;
        if (d->b == TrDown2 && c.run.surf < 60) continue;
        if (c.run.surf >= 120 && (d->b == TrUp1 || d->b == TrUp2)) continue; // keep terrain low
        filtered.push_back(d);
    }
    Choice none;
    if (filtered.empty()) { debugLog("    tick %d: no candidates pass the filter (of %zu, airborne %d, surf %.0f g %d)", k, cands.size(), airborne, c.run.surf, c.run.g); return none; }
    std::vector<float> w;
    RuleStyleModel rules;
    const StyleModel* model = b.styleModel() ? b.styleModel() : &rules;
    model->score(ctx, filtered, w);
    if (auto* last = b.memory().lastForContext(ctx.motifKey)) {
        for (size_t i = 0; i < filtered.size(); ++i) if (filtered[i]->id == last->id) w[i] *= ctx.style == Style::Chaotic ? 1.5f : 3.0f;
    }
    std::vector<size_t> order;
    std::vector<float> ww = w;
    for (size_t n = 0; n < std::min<size_t>(filtered.size(), 16); ++n) {
        int pick = b.rng().weighted(ww);
        if (pick < 0) break;
        order.push_back(size_t(pick));
        ww[size_t(pick)] = 0;
    }
    const float offsets[] = {0, -15, 15, -30, 30, -45};
    for (size_t oi : order) {
        const PatternDef& d = *filtered[oi];
        float bestScore = -1e9f, bestOff = 0;
        bool any = false;
        for (float off : offsets) {
            auto m = b.mark();
            Run saved = c.run;
            uint64_t rngState = b.rng().state();
            (void)rngState;
            if (placeOne(c, d, off, k, hold, airborne, arc)) {
                b.press(k);
                int pressEv = int(b.inputs().events.size()) - 1;
                b.release(k + hold);
                Attempt at = evaluate(c, pressEv, k, nextClickTick);
                int window = at.early + at.late + 1;
                debugLog("    tick %d cand %s off %.0f: alive=%d window=%d/%d forced=%d | cursor %d x=%.0f takeoff=%.0f apex=%.0f land=%.0f surf=%.0f | death %d (%.0f,%.0f) %s", k, d.id.c_str(), off, at.ok, window, c.minWindowTicks, at.forced, b.cursor().tick, b.cursor().p[0].x, arc.takeoffX, arc.apexX, arc.landX, c.run.surf, at.deathTick, at.deathX, at.deathY, phys::deathCauseName(at.cause));
                if (at.ok && window >= c.minWindowTicks && at.forced) {
                    float centre = float(at.late - at.early) * 0.5f;
                    float score = -std::fabs(centre) + 0.05f * float(std::min(window, 60));
                    if (score > bestScore) { bestScore = score; bestOff = off; }
                    any = true;
                }
            }
            b.rollback(m);
            c.run = saved;
            if (any && std::fabs(bestScore) < 2.0f) break; // well centred already
        }
        if (any) return {&d, bestOff};
    }
    return none;
}

} // namespace

bool buildGroundPart(Builder& b, const Part& part, int attempt) {
    const auto& plan = b.plan();
    const auto& prof = b.profileFor(part);
    GroundCtx c{b, part, prof};
    c.minWindowTicks = std::max(4, int(std::ceil(prof.minWindowMs * phys::kTPS / 1000.0)) - attempt / 3);
    c.minWindowTicks = std::min(c.minWindowTicks, 2 * c.maxShift - 8);
    const int k0 = Builder::tickOf(part.t0), k1 = Builder::tickOf(part.t1);
    if (attempt > 0) for (int i = 0; i < attempt * 3; ++i) b.rng().next(); // different choices per attempt

    // settle into the part: run until the player is grounded in this mode
    {
        const phys::GameState s0 = b.cursor();
        int firstClick = part.clicks.empty() ? k1 : Builder::tickOf(plan.clicks[size_t(part.clicks.front())].time);
        int settleLimit = std::max(s0.tick + 1, std::min(firstClick - c.maxShift - 4, k0 + 120));
        auto settle = [&](phys::GameState& s) {
            while (s.tick < settleLimit) {
                if (!b.runTo(s, s.tick + 1)) { debugLog("  ground: died while settling at tick %d x=%.0f (%s)", s.tick, s.p[0].x, phys::deathCauseName(s.p[0].deathCause)); return false; }
                if (s.p[0].mode == part.mode && s.p[0].onGround && s.p[0].upsideDown == part.flipped && s.tick > k0 &&
                    s.time - s.p[0].lastFlipTime > 0.05) break; // (the on-ground flag is stale right after a flip)
            }
            return true;
        };
        phys::GameState s = s0;
        if (!settle(s)) return false;
        while (s.p[0].mode != part.mode && s.tick < k0 + 240)
            if (!b.runTo(s, s.tick + 1)) { debugLog("  ground: died before entering the mode at tick %d", s.tick); return false; }
        if (s.p[0].mode != part.mode) { debugLog("  ground: the player never entered the mode"); return false; }
        if (debugEnabled() && std::getenv("AL_TRACE_SETTLE")) {
            phys::GameState q = s0;
            while (q.tick < s.tick) {
                b.runTo(q, q.tick + 1);
                debugLog("      settle t %d x %.1f y %.1f v %.2f g %d ground %d ev %x", q.tick, q.p[0].x, q.p[0].y, q.p[0].v, q.p[0].flip(), q.p[0].onGround, q.p[0].events);
            }
            std::vector<int> near;
            b.world().query(s.p[0].x - 40, s.p[0].x + 40, near);
            for (int si : near) { const auto& so = b.world().objects()[size_t(si)]; debugLog("      obj kind %d at (%.0f, %.0f) %.0fx%.0f", int(so.kind), so.x, so.y, so.hw * 2, so.hh * 2); }
        }
        if (!s.p[0].onGround) {
            // still airborne (coming from a flying part or a gravity flip with nothing to land on):
            // build the surface it is heading for from just ahead of the settle start, settle again
            const auto& p = s.p[0];
            c.run.g = p.flip();
            c.run.floor = c.run.g > 0;
            if (c.run.g > 0) {
                c.run.surf = 0.0f;
                c.run.builtTo = std::floor((s0.p[0].x + 30.0f) / kCell) * kCell;
            } else {
                // a ceiling over the free rise: the lowest height (from 150) the player reaches
                // after passing below the start of the blocks with room to spare
                std::vector<std::pair<float, float>> rise; // (x, top of the hitbox)
                phys::GameState q = s0;
                for (int t = 0; t < 360 && !q.dead; ++t) {
                    b.runTo(q, q.tick + 1);
                    rise.push_back({q.p[0].x, q.p[0].y + q.p[0].height() * 0.5f});
                }
                c.run.surf = 300.0f;
                c.run.builtTo = std::floor((s0.p[0].x + 30.0f) / kCell) * kCell;
                for (float cand = 150.0f; cand <= 600.0f; cand += kCell) {
                    float xs = -1;
                    for (auto& [x, top] : rise) if (top >= cand - 25.0f) { xs = x; break; }
                    if (xs < 0) continue;
                    float start = std::floor((xs - 15.0f) / kCell) * kCell;
                    if (start < s0.p[0].x + 30.0f) continue;
                    c.run.surf = cand;
                    c.run.builtTo = start;
                    break;
                }
            }
            extendRun(c, p.x + 240.0f);
            debugLog("  ground: airborne after settling (tick %d y %.0f g %d): surface %.0f from x %.0f", s.tick, p.y, c.run.g, c.run.surf, s0.p[0].x);
            s = s0;
            if (!settle(s)) return false;
            while (!s.p[0].onGround && s.tick < k0 + 240) {
                extendRun(c, s.p[0].x + 240.0f);
                if (!b.runTo(s, s.tick + 1)) { debugLog("  ground: died while landing at tick %d x=%.0f y=%.0f (%s)", s.tick, s.p[0].x, s.p[0].y, phys::deathCauseName(s.p[0].deathCause)); return false; }
            }
        }
        b.setCursor(s);
        debugLog("  ground: settled at tick %d (part %d..%d) x=%.0f y=%.1f v=%.2f g %d ground %d mode %d", s.tick, k0, k1, s.p[0].x, s.p[0].y, s.p[0].v, s.p[0].flip(), s.p[0].onGround, int(s.p[0].mode));
        const auto& p = s.p[0];
        const Run pre = c.run;
        c.run.g = p.flip();
        float feet = p.y - c.run.g * p.height() * 0.5f;
        c.run.surf = std::round(feet / 15.0f) * 15.0f;
        c.run.floor = c.run.g > 0 && c.run.surf <= 0.5f;
        if (c.run.floor) c.run.surf = 0;
        // keep the surface built while settling only if the player actually landed on it
        const bool same = pre.builtTo > 0 && pre.g == c.run.g && std::fabs(pre.surf - c.run.surf) < 1.0f;
        c.run.builtTo = same ? std::max(pre.builtTo, p.x + 1) : p.x + 1;
    }
    int dropped = 0;
    for (size_t ci = 0; ci < part.clicks.size(); ++ci) {
        int clickIndex = part.clicks[ci];
        const Click& ck = plan.clicks[size_t(clickIndex)];
        int k = Builder::tickOf(ck.time);
        int nextK = ci + 1 < part.clicks.size() ? Builder::tickOf(plan.clicks[size_t(part.clicks[ci + 1])].time) : -1;
        if (k <= b.cursor().tick + 2) { debugLog("  ground: click at tick %d is behind the cursor (%d)", k, b.cursor().tick); ++dropped; continue; }
        // move the cursor close to the click (but before its window)
        int target = k - c.maxShift - 2;
        if (target > b.cursor().tick) {
            const auto& cp = b.cursor().p[0];
            float ups = float(phys::speedParams(cp.speed).unitsPerStep());
            extendRun(c, cp.x + float(target - b.cursor().tick) * ups + 20.0f);
            if (!b.advanceCursor(target)) {
                phys::GameState d = b.cursor();
                b.runTo(d, target);
                debugLog("  ground: died advancing to tick %d (click %d): tick %d x=%.0f y=%.0f %s, run surf %.0f builtTo %.0f", target, clickIndex, d.tick, d.p[0].x, d.p[0].y, phys::deathCauseName(d.p[0].deathCause), c.run.surf, c.run.builtTo);
                if (debugEnabled()) {
                    phys::GameState q = b.cursor();
                    while (!q.dead && q.tick < target) {
                        b.runTo(q, q.tick + 1);
                        if (q.tick > d.tick - 25) debugLog("      t %d x %.1f y %.1f v %.2f mode %d g %d ground %d", q.tick, q.p[0].x, q.p[0].y, q.p[0].v, int(q.p[0].mode), q.p[0].flip(), q.p[0].onGround);
                    }
                    for (auto& e : b.inputs().events) if (e.tick > d.tick - 200) debugLog("      input %d %d", e.tick, e.press);
                    if (d.p[0].deathObj >= 0) { const auto& so = b.world().objects()[size_t(d.p[0].deathObj)]; debugLog("      killer kind %d at (%.0f, %.0f) %.0fx%.0f", int(so.kind), so.x, so.y, so.hw * 2, so.hh * 2); }
                }
                return false;
            }
        }
        // is the player airborne at the click? (temporary look-ahead terrain)
        phys::GameState s = b.cursor();
        {
            auto mk = b.mark();
            Run sv = c.run;
            extendRun(c, s.p[0].x + 400);
            b.runTo(s, k - 1);
            b.rollback(mk);
            c.run = sv;
        }
        bool airborne = !s.p[0].onGround && !s.p[0].canJump;
        ClickResult r;
        r.click = clickIndex;
        r.x = s.p[0].x;
        PatternContext ctx = makeContext(c, ck, nextK);
        // hold length: cube taps; robot tries a few hold lengths (jump height) starting from the music's
        int hold = 5;
        Arc arc;
        Choice choice;
        std::vector<int> holds{5};
        if (airborne && part.mode == Mode::Cube) holds = {16, 10, 5}; // held into the orb: an early press still catches it
        else if (part.mode == Mode::Robot) {
            int musical = ck.hold ? std::max(6, Builder::tickOf(ck.release) - k) : 0;
            holds = musical ? std::vector<int>{musical, 67, 40, 20} : std::vector<int>{20, 40, 67, 10};
        }
        for (int hcand : holds) {
            hold = hcand;
            if (nextK > 0) hold = std::min(hold, std::max(2, nextK - k - 2));
            arc = probeArc(c, k, hold, airborne);
            if (!arc.ok && !airborne && debugEnabled()) {
                phys::GameState q = b.cursor();
                b.runTo(q, k - 1);
                debugLog("    no arc at tick %d: player y %.1f g %d ground %d mini %d, run surf %.0f g %d floor %d builtTo %.0f", k, q.p[0].y, q.p[0].flip(), q.p[0].onGround, q.p[0].mini, c.run.surf, c.run.g, c.run.floor, c.run.builtTo);
            }
            if (!airborne && !arc.ok) continue;
            choice = choose(c, ctx, k, hold, nextK, airborne, arc);
            if (choice.def) break;
        }
        auto m = b.mark();
        Run saved = c.run;
        if (!choice.def || !placeOne(c, *choice.def, choice.offset, k, hold, airborne, arc)) {
            debugLog("  ground: no fair pattern for the click at tick %d (airborne %d, arc %d)", k, airborne, arc.ok);
            b.rollback(m);
            c.run = saved;
            r.dropped = true;
            r.pattern = "dropped";
            b.addClickResult(r);
            ++dropped;
            continue;
        }
        const std::string pid = choice.def->id;
        b.press(k);
        int pressEv = int(b.inputs().events.size()) - 1;
        b.release(k + hold);
        Attempt ev = evaluate(c, pressEv, k, nextK);
        if (!ev.ok) { debugLog("  ground: chosen pattern died on re-evaluation at tick %d", k); b.rollback(m); c.run = saved; r.dropped = true; r.pattern = "dropped"; b.addClickResult(r); ++dropped; continue; }
        r.pattern = pid;
        r.pressEvent = pressEv;
        r.windowEarly = ev.early;
        r.windowLate = ev.late;
        r.forced = ev.forced;
        r.x = arc.takeoffX;
        b.addClickResult(r);
        b.memory().uses.push_back({pid, part.index, motifKey(c.lastClickTime >= 0 ? ck.time - c.lastClickTime : 2.0, nextK > 0 ? Builder::timeOf(nextK) - ck.time : 2.0, ck.role, (ck.pitch > 0 && c.lastPitch > 0) ? ck.pitch - c.lastPitch : 0, ck.role == Role::Downbeat), ck.time});
        c.prev2 = c.prev1;
        c.prev1 = pid;
        c.lastClickTime = ck.time;
        if (ck.pitch > 0) c.lastPitch = ck.pitch;
    }
    // extend the terrain through the end of the part and verify
    phys::GameState s = b.cursor();
    const bool lastPart = part.index + 1 == int(plan.parts.size());
    {
        // the terrain ends just before the next part's portals (placed ~60 units after the
        // player's position 24 ticks before the part end); the last part runs on to the level end
        phys::GameState q = s;
        const auto& qp0 = q.p[0];
        float ups = float(phys::speedParams(qp0.speed).unitsPerStep());
        float xEnd = qp0.x + float(std::max(0, k1 - 24 - q.tick)) * ups;
        const bool nextGround = !lastPart && (plan.parts[size_t(part.index + 1)].mode == Mode::Cube || plan.parts[size_t(part.index + 1)].mode == Mode::Robot);
        extendRun(c, lastPart ? xEnd + 900.0f : (nextGround ? xEnd + 120.0f : xEnd - 15.0f));
    }
    if (dropped * 3 > int(part.clicks.size()) + 1 && attempt + 2 < b.options().maxRetriesPerSection) {
        debugLog("  ground: %d of %zu clicks dropped", dropped, part.clicks.size());
        return false;
    }
    bool okEnd = b.advanceCursor(std::max(b.cursor().tick, k1 - 30));
    if (!okEnd) debugLog("  ground: died before the part end");
    return okEnd;
}

} // namespace al::gen
