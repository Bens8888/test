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
    const int t = m_inputs.events[size_t(ev)].tick;
    const int lo = ev > 0 ? m_inputs.events[size_t(ev - 1)].tick + 1 : m_cursor.tick + 1;
    const int hi = ev + 1 < int(m_inputs.events.size()) ? m_inputs.events[size_t(ev + 1)].tick - 1 : t + maxShift;
    auto survives = [&](int shift) {
        int nt = t + shift;
        if (nt < lo || nt > hi || nt <= m_cursor.tick) return false;
        bot::InputTrack mod;
        mod.events = m_inputs.events;
        mod.events[size_t(ev)].tick = nt;
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

// ---------------------------------------------------------------- transitions

void Builder::placeTransition(const Part& prev, const Part& next, bool first) {
    if (first) return;
    const int k0 = tickOf(next.t0);
    // Predict where the player is around the transition (no inputs after the previous part).
    phys::GameState s = m_cursor;
    runTo(s, std::max(s.tick, k0 - 24));
    const auto& p = s.p[0];
    float x = p.x;
    auto yAt = [&](float px) {
        phys::GameState q = s;
        while (q.p[0].x < px - 30 && !q.dead && q.tick < k0 + 240) phys::step(m_world, q, false);
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
    for (auto [id, isMode] : portals) {
        float py = yAt(px);
        // keep bounded-mode floors on the ground when the player is low
        float y = std::round(py / 15.0f) * 15.0f;
        if (isMode && y < 45) y = 45;
        LevelObject o = obj(id, px, y);
        if (isMode) o.setProp(13, std::string("1"));
        place(o);
        px += 30.0f;
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
            phys::GameState s = m_partStart[size_t(i)];
            int until = i + 1 < int(m_plan.parts.size()) ? kEnd - 30 : kEnd;
            if (runTo(s, until)) {
                m_cursor = s;
                res.ok = true;
                break;
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
    // No hazards at all: the player passes through the part without input. The part's clicks are
    // reported as dropped so the report shows exactly where the generator gave up.
    for (int ci : part.clicks) {
        ClickResult r;
        r.click = ci;
        r.dropped = true;
        r.pattern = "fallback";
        r.x = 0;
        m_clicks.push_back(r);
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
    float endX = std::max(s.p[0].x + 30.0f, float(m_level.lastObjectX()) + 120.0f);
    auto it = std::find_if(m_level.objects.begin(), m_level.objects.end(), [](const LevelObject& o) { return o.id == oid::TrigEnd; });
    if (it != m_level.objects.end()) m_level.objects.erase(it);
    LevelObject end = obj(oid::TrigEnd, std::round(endX / 30.0f) * 30.0f + 15, 600);
    end.setProp(36, std::string("1"));
    placeDeco(end);
    m_world.setEndX(float(end.x));
}

} // namespace al::gen
