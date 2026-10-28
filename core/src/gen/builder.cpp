#include <al/gen/builder.hpp>

#include <algorithm>
#include <cmath>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace al::gen {

bool debugEnabled() {
    static const bool on = std::getenv("AL_DEBUG") != nullptr;
    return on;
}

void debugLog(const char* fmt, ...) {
    if (!debugEnabled()) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

using level::LevelObject;
namespace oid = level::id;

Builder::Builder(const audio::MusicAnalysis& A, const GenOptions& opt, DesignPlan& plan, const StyleModel* style, Rng rng)
    : m_A(A), m_opt(opt), m_plan(plan), m_style(style), m_rng(rng), m_baseRng(rng) {
    m_level.name = "AutoLayout";
    auto& st = m_level.settings;
    st.fixGravityBug = true;
    st.fixRobotJump = true;
    st.fixRadius = true;
    if (!plan.parts.empty()) {
        const auto& p0 = plan.parts.front();
        st.startMode = p0.mode;
        st.startSpeed = p0.speed;
        st.startMini = p0.mini;
        st.startFlipped = p0.flipped;
        st.startDual = p0.dual;
    }
    m_world = phys::World(m_level);
    m_world.setEndX(1e9f);
    m_cursor = phys::initialState(m_world);
    m_endTime = plan.duration;
    m_nextGroup = opt.groupBase > 0 ? opt.groupBase : 1000;
    m_profiles.resize(plan.parts.size());
    for (size_t i = 0; i < plan.parts.size(); ++i) m_profiles[i] = profileAt(plan.parts[i].difficulty);
}

int PatternMemory::countRecent(const std::string& id, size_t lookback) const {
    int n = 0;
    for (size_t i = uses.size(); i-- > 0 && uses.size() - i <= lookback;) if (uses[i].id == id) ++n;
    return n;
}

const PatternMemory::Use* PatternMemory::lastForContext(uint32_t ctx) const {
    for (size_t i = uses.size(); i-- > 0;) if (uses[i].context == ctx) return &uses[i];
    return nullptr;
}

const DifficultyProfile& Builder::profileFor(const Part& p) const { return m_profiles[size_t(std::clamp(p.index, 0, int(m_profiles.size()) - 1))]; }

Builder::Mark Builder::mark() const { return {m_level.objects.size(), m_world.size(), m_inputs.events.size(), m_clicks.size()}; }

void Builder::rollback(const Mark& m) {
    m_level.objects.resize(m.levelObjects);
    m_world.truncate(m.worldObjects);
    m_inputs.events.resize(m.inputEvents);
    m_clicks.resize(m.clickResults);
}

LevelObject Builder::obj(int id, double x, double y, double rot) {
    LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

int Builder::place(LevelObject o) {
    int idx = int(m_level.objects.size());
    m_level.objects.push_back(o);
    m_world.addObject(m_level.objects.back(), idx);
    return idx;
}

int Builder::placeDeco(LevelObject o) {
    m_level.objects.push_back(std::move(o));
    return int(m_level.objects.size() - 1);
}

void Builder::press(int tick) {
    if (!m_inputs.events.empty() && m_inputs.events.back().tick >= tick) tick = m_inputs.events.back().tick + 1;
    if (heldAfterInputs()) return;
    m_inputs.events.push_back({tick, true});
}

void Builder::release(int tick) {
    if (!m_inputs.events.empty() && m_inputs.events.back().tick >= tick) tick = m_inputs.events.back().tick + 1;
    if (!heldAfterInputs()) return;
    m_inputs.events.push_back({tick, false});
}

bool Builder::runToWithInputs(phys::GameState& s, int tick, const bot::InputTrack& in) const {
    auto it = std::upper_bound(in.events.begin(), in.events.end(), s.tick, [](int t, const bot::InputEvent& e) { return t < e.tick; });
    size_t ei = size_t(it - in.events.begin());
    bool held = ei > 0 ? in.events[ei - 1].press : false;
    while (s.tick < tick && !s.dead) {
        int next = s.tick + 1;
        while (ei < in.events.size() && in.events[ei].tick <= next) { held = in.events[ei].press; ++ei; }
        phys::step(m_world, s, held);
    }
    return !s.dead;
}

bool Builder::runTo(phys::GameState& s, int tick) const { return runToWithInputs(s, tick, m_inputs); }

bool Builder::advanceCursor(int tick) {
    phys::GameState s = m_cursor;
    if (!runTo(s, tick)) return false;
    m_cursor = s;
    return true;
}

void Builder::window(int ev, int horizonTick, int maxShift, int& early, int& late) const {
    early = late = 0;
    if (ev < 0 || ev >= int(m_inputs.events.size())) return;
    const auto& E = m_inputs.events;
    const int t = E[size_t(ev)].tick;
    // a tap (press + release within a few ticks) moves as a whole
    const bool tap = E[size_t(ev)].press && ev + 1 < int(E.size()) && !E[size_t(ev + 1)].press && E[size_t(ev + 1)].tick - t <= 8;
    const int last = tap ? ev + 1 : ev;
    const int lo = ev > 0 ? E[size_t(ev - 1)].tick + 1 : m_cursor.tick + 1;
    const int hi = last + 1 < int(E.size()) ? E[size_t(last + 1)].tick - 1 - (E[size_t(last)].tick - t) : t + maxShift;
    auto survives = [&](int shift) {
        int nt = t + shift;
        if (nt < lo || nt > hi || nt <= m_cursor.tick) return false;
        bot::InputTrack mod;
        mod.events = E;
        for (int j = ev; j <= last; ++j) mod.events[size_t(j)].tick += shift;
        phys::GameState s = m_cursor;
        return runToWithInputs(s, horizonTick + std::max(0, shift), mod);
    };
    auto search = [&](int sign) {
        int good = 0, bad = -1;
        for (int s = 1; s <= maxShift; s *= 2) {
            if (survives(sign * s)) good = s;
            else { bad = s; break; }
        }
        if (bad < 0) {
            if (good < maxShift && survives(sign * maxShift)) return maxShift;
            if (good < maxShift) bad = maxShift;
            else return good;
        }
        while (bad - good > 1) {
            int mid = (good + bad) / 2;
            if (survives(sign * mid)) good = mid;
            else bad = mid;
        }
        return good;
    };
    early = search(-1);
    late = search(+1);
}

bool Builder::necessary(int pressEv, int horizonTick) const {
    if (pressEv < 0 || pressEv >= int(m_inputs.events.size())) return false;
    bot::InputTrack mod;
    mod.events = m_inputs.events;
    size_t n = 1;
    if (size_t(pressEv) + 1 < mod.events.size() && !mod.events[size_t(pressEv) + 1].press) n = 2;
    mod.events.erase(mod.events.begin() + pressEv, mod.events.begin() + pressEv + long(n));
    phys::GameState s = m_cursor;
    return !runToWithInputs(s, horizonTick, mod);
}

int Builder::nextPressAfter(int ev) const {
    const auto& E = m_inputs.events;
    for (size_t j = size_t(ev) + 1; j < E.size(); ++j) if (E[j].press) return E[j].tick;
    return E[size_t(ev)].tick + 120;
}

bool Builder::pathEnvelope(int startTick, int endTick, int halfWin, level::Mode mode, std::vector<PathSample>& out) const {
    std::vector<phys::GameState> states;
    phys::GameState s = m_cursor;
    states.push_back(s);
    auto add = [&](const phys::GameState& st) {
        int n = st.dual ? 2 : 1;
        for (int i = 0; i < n; ++i) {
            const auto& p = st.p[size_t(i)];
            out.push_back({p.x, p.y, p.height() * 0.5f, i});
        }
    };
    while (s.tick < endTick) {
        if (!runTo(s, s.tick + 1)) return false;
        add(s);
        states.push_back(s);
        if (s.p[0].mode != mode) break;
    }
    const auto& E = m_inputs.events;
    for (size_t ev = 0; ev < E.size(); ++ev) {
        const int t = E[ev].tick;
        if (t <= startTick + 1 || t >= endTick) continue;
        if (!E[ev].press && ev > 0 && E[ev - 1].press && t - E[ev - 1].tick <= 8) continue; // tap release: moves with its press
        const bool tap = E[ev].press && ev + 1 < E.size() && !E[ev + 1].press && E[ev + 1].tick - t <= 8;
        const size_t last = tap ? ev + 1 : ev;
        const int lo = ev > 0 ? E[ev - 1].tick + 1 : startTick + 1;
        const int hi = last + 1 < E.size() ? E[last + 1].tick - 1 - (E[last].tick - t) : t + halfWin;
        const int until = std::min(endTick, nextPressAfter(int(ev)) + 12);
        for (int shift : {-halfWin, -(halfWin + 1) / 2, (halfWin + 1) / 2, halfWin}) {
            int nt = std::clamp(t + shift, std::max(lo, startTick + 1), std::max(hi, startTick + 1));
            if (nt == t) continue;
            bot::InputTrack mod;
            mod.events = E;
            for (size_t j = ev; j <= last; ++j) mod.events[j].tick += nt - t;
            int from = std::min(t, nt) - 1 - m_cursor.tick;
            if (from < 0 || size_t(from) >= states.size()) continue;
            phys::GameState v = states[size_t(from)];
            while (v.tick < until) {
                if (!runToWithInputs(v, v.tick + 1, mod)) break;
                if (v.p[0].mode != mode) break;
                add(v);
            }
        }
    }
    return true;
}

bool clearOf(const level::LevelObject& o, const std::vector<PathSample>& samples, float margin) {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return true;
    float hw = so.hw, hh = so.kind == level::Kind::Saw ? so.hw : so.hh;
    if (so.obb) { float r = std::hypot(so.hw, so.hh); hw = hh = r; }
    for (const auto& p : samples) {
        if (std::fabs(p.x - so.x) >= p.half + hw + margin) continue;
        if (std::fabs(p.y - so.y) >= p.half + hh + margin) continue;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- transitions

void Builder::placeTransition(const Part& prev, const Part& next, bool first) {
    if (first) return;
    const int k0 = tickOf(next.t0);
    // Inputs never carry over into the next part (a held button would make a cube jump on its own).
    if (heldAfterInputs()) release(m_cursor.tick + 1);
    // Predict where the player is around the transition (no inputs after the previous part).
    phys::GameState s = m_cursor;
    runTo(s, std::max(s.tick, k0 - 24));
    const auto& p = s.p[0];
    float x = p.x;
    auto yAt = [&](float px) {
        phys::GameState q = s;
        while (q.p[0].x < px - 30 && !q.dead && q.tick < k0 + 240) runTo(q, q.tick + 1);
        return q.p[0].y;
    };
    std::vector<std::pair<int, int>> portals; // (id, extra)
    if (next.speed != int(p.speed)) {
        static const int speedIds[5] = {oid::Speed05, oid::Speed1, oid::Speed2, oid::Speed3, oid::Speed4};
        portals.push_back({speedIds[std::clamp(next.speed, 0, 4)], 0});
    }
    if (next.mini != p.mini) portals.push_back({next.mini ? oid::PortalSizeMini : oid::PortalSizeNormal, 0});
    bool wantFlip = next.flipped;
    if (next.mode == Mode::Ball || next.mode == Mode::Spider || next.mode == Mode::Swing) wantFlip = p.upsideDown && false;
    if (wantFlip != p.upsideDown) portals.push_back({wantFlip ? oid::PortalGravFlip : oid::PortalGravNormal, 0});
    static const int modeIds[8] = {oid::PortalCube, oid::PortalShip, oid::PortalBall, oid::PortalUfo, oid::PortalWave, oid::PortalRobot, oid::PortalSpider, oid::PortalSwing};
    bool bounded = next.mode == Mode::Ship || next.mode == Mode::Ufo || next.mode == Mode::Wave || next.mode == Mode::Swing || next.mode == Mode::Ball || next.mode == Mode::Spider;
    if (next.mode != prev.mode || bounded) portals.push_back({modeIds[int(next.mode)], 1});
    if (next.dual != s.dual) portals.push_back({next.dual ? oid::PortalDualOn : oid::PortalDualOff, 0});
    float px = std::round((x + 60.0f) / 15.0f) * 15.0f;
    auto stateAt = [&](float px) {
        phys::GameState q = s;
        while (q.p[0].x < px - 30 && !q.dead && q.tick < k0 + 240) runTo(q, q.tick + 1);
        return q;
    };
    auto placeAll = [&](float px) {
        for (auto [id, isMode] : portals) {
            if (id == oid::PortalGravFlip || id == oid::PortalGravNormal) {
                // flip only where the player is grounded or well clear of the floor: flipping while
                // falling onto the floor would carry the player into it head first
                for (int tries = 0; tries < 20; ++tries) {
                    phys::GameState q = stateAt(px);
                    const auto& qp = q.p[0];
                    const bool groundMode = qp.mode == Mode::Cube || qp.mode == Mode::Robot;
                    if (q.dead || !groundMode || qp.onGround || qp.y - qp.height() * 0.5f > 45.0f) break;
                    px += 15.0f;
                }
            }
            float py = yAt(px);
            // keep bounded-mode floors on the ground when the player is low
            float y = std::round(py / 15.0f) * 15.0f;
            if (isMode && y < 45) y = 45;
            LevelObject o = obj(id, px, y);
            if (isMode) o.setProp(13, std::string("1"));
            debugLog("  transition: portal %d at (%.0f, %.0f), predicted player y %.0f", id, px, y, py);
            place(o);
            px += 30.0f;
        }
        return px;
    };
    // The player must survive passing the portals (a size or gravity change next to the previous
    // part's last obstacles can kill): otherwise move them further on.
    const Mark m = mark();
    for (int shift = 0; shift < 12; ++shift) {
        float lastX = placeAll(px + 30.0f * float(shift));
        phys::GameState q = m_cursor;
        bool alive = true;
        while (alive && q.p[0].x < lastX + 30.0f && q.tick < k0 + 400) alive = runTo(q, q.tick + 1);
        if (alive || shift == 11) break;
        debugLog("  transition: the player dies passing the portals (tick %d x=%.0f %s), moving them", q.tick, q.p[0].x, phys::deathCauseName(q.p[0].deathCause));
        rollback(m);
        if (shift == 10) { placeAll(px); break; }
    }
}

// ---------------------------------------------------------------- part building

bool Builder::buildPart(int i, const Progress* progress) {
    const Part& part = m_plan.parts[size_t(i)];
    if (m_partStart.size() <= size_t(i)) { m_partStart.resize(size_t(i) + 1); m_partMark.resize(size_t(i) + 1); }
    m_partStart[size_t(i)] = m_cursor;
    m_partMark[size_t(i)] = mark();
    const size_t memMark = m_memory.uses.size();
    PartResult res;
    res.part = i;
    const int maxAttempts = std::max(1, m_opt.maxRetriesPerSection);
    const int kEnd = tickOf(part.t1);
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        checkCancel(progress);
        rollback(m_partMark[size_t(i)]);
        m_memory.uses.resize(memMark);
        m_cursor = m_partStart[size_t(i)];
        // independent random stream per part and attempt: re-rolling one part leaves the others intact
        m_rng = m_baseRng.fork(combineSeed(uint64_t(i) * 0x9E3779B97F4A7C15ull + part.salt, uint64_t(attempt)));
        placeTransition(i > 0 ? m_plan.parts[size_t(i - 1)] : part, part, i == 0);
        bool ok = false;
        switch (part.mode) {
            case Mode::Cube: case Mode::Robot: ok = buildGroundPart(*this, part, attempt); break;
            case Mode::Ship: case Mode::Ufo: case Mode::Wave: case Mode::Swing: ok = buildFlyingPart(*this, part, attempt); break;
            case Mode::Ball: case Mode::Spider: ok = buildSurfacePart(*this, part, attempt); break;
            default: break;
        }
        res.attempts = attempt + 1;
        debugLog("part %d (%s) attempt %d: builder %s", i, level::modeName(part.mode), attempt, ok ? "ok" : "FAILED");
        if (ok) {
            // verify the whole part from its start state
            // (a little past the part end too: the next part's portals come ~10-40 ticks after
            // it, and nothing of this part may kill the player before them)
            phys::GameState s = m_partStart[size_t(i)];
            int until = i + 1 < int(m_plan.parts.size()) ? kEnd - 30 : kEnd;
            if (runTo(s, until)) {
                phys::GameState beyond = s;
                if (i + 1 >= int(m_plan.parts.size()) || runTo(beyond, kEnd + 10)) {
                    m_cursor = s;
                    res.ok = true;
                    break;
                }
                s = beyond;
            }
            res.note = "part failed final verification";
            debugLog("part %d: final verification died at tick %d (x=%.0f cause=%s)", i, s.tick, s.p[0].x, phys::deathCauseName(s.p[0].deathCause));
        }
    }
    if (!res.ok) {
        rollback(m_partMark[size_t(i)]);
        m_memory.uses.resize(memMark);
        m_cursor = m_partStart[size_t(i)];
        placeTransition(i > 0 ? m_plan.parts[size_t(i - 1)] : part, part, i == 0);
        res.fallback = true;
        res.ok = buildSafeFallback(part);
        res.note = res.ok ? "rebuilt with the safe fallback" : "fallback failed";
        phys::GameState s = m_partStart[size_t(i)];
        int until = i + 1 < int(m_plan.parts.size()) ? kEnd - 30 : kEnd;
        if (runTo(s, until)) m_cursor = s;
        else res.ok = false;
    }
    if (m_parts.size() <= size_t(i)) m_parts.resize(size_t(i) + 1);
    m_parts[size_t(i)] = res;
    return res.ok;
}

bool Builder::buildSafeFallback(const Part& part) {
    // The part's terrain without any of its clicks (no click-forcing hazards): a plain floor or
    // corridor the player passes without input. The clicks are reported as dropped so the report
    // shows exactly where the generator gave up.
    for (int ci : part.clicks) {
        ClickResult r;
        r.click = ci;
        r.dropped = true;
        r.pattern = "fallback";
        r.x = 0;
        m_clicks.push_back(r);
    }
    Part bare = part;
    bare.clicks.clear();
    const Mark m = mark();
    const phys::GameState start = m_cursor;
    bool ok = false;
    const int attempt = std::max(1, m_opt.maxRetriesPerSection);
    switch (part.mode) {
        case Mode::Cube: case Mode::Robot: ok = buildGroundPart(*this, bare, attempt); break;
        case Mode::Ship: case Mode::Ufo: case Mode::Wave: case Mode::Swing: ok = buildFlyingPart(*this, bare, attempt); break;
        case Mode::Ball: case Mode::Spider: ok = buildSurfacePart(*this, bare, attempt); break;
        default: break;
    }
    if (!ok) {
        // keep the clicks' results, drop whatever the failed builder added
        Mark keep = m;
        keep.clickResults = m_clicks.size();
        rollback(keep);
        m_cursor = start;
    }
    return true;
}

bool Builder::build(const Progress* progress) {
    return rebuildFrom(0, progress);
}

bool Builder::rebuildFrom(int index, const Progress* progress) {
    index = std::clamp(index, 0, int(m_plan.parts.size()));
    if (index > 0 && size_t(index) < m_partMark.size()) {
        rollback(m_partMark[size_t(index)]);
        m_cursor = m_partStart[size_t(index)];
        m_parts.resize(size_t(index));
        // drop memory uses of rebuilt parts
        m_memory.uses.erase(std::remove_if(m_memory.uses.begin(), m_memory.uses.end(), [&](const PatternMemory::Use& u) { return u.part >= index; }), m_memory.uses.end());
    } else if (index == 0) {
        rollback({0, 0, 0, 0});
        m_cursor = phys::initialState(m_world);
        m_parts.clear();
        m_memory.uses.clear();
    }
    try {
        for (int i = index; i < int(m_plan.parts.size()); ++i) {
            if (progress) progress->report("build", float(i) / std::max<size_t>(1, m_plan.parts.size()));
            buildPart(i, progress);
        }
        finish();
    } catch (const Cancelled&) {
        return false;
    }
    return true;
}

void Builder::finish() {
    // Level end: an end trigger where the song ends (or just after the last gameplay object).
    phys::GameState s = m_cursor;
    int endTick = tickOf(m_endTime);
    runTo(s, std::max(s.tick, endTick - 1));
    // past the song end and every interactive object (the run-out terrain of the last part does
    // not count: it only carries the player to the end)
    float lastInteractive = 0;
    for (const auto& o : m_level.objects) {
        auto k = o.info().kind;
        if (k != level::Kind::Solid && k != level::Kind::Deco && k != level::Kind::Trigger) lastInteractive = std::max(lastInteractive, float(o.x));
    }
    float endX = std::max(s.p[0].x + 30.0f, lastInteractive + 120.0f);
    auto it = std::find_if(m_level.objects.begin(), m_level.objects.end(), [](const LevelObject& o) { return o.id == oid::TrigEnd; });
    if (it != m_level.objects.end()) m_level.objects.erase(it);
    LevelObject end = obj(oid::TrigEnd, std::round(endX / 30.0f) * 30.0f + 15, 600);
    end.setProp(36, std::string("1"));
    placeDeco(end);
    m_world.setEndX(float(end.x));
}

} // namespace al::gen
