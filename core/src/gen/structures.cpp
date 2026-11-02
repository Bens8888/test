// Generator v2: structures around the gameplay path (see structures.hpp).
#include <al/gen/structures.hpp>

#include <al/bot/bot.hpp>
#include <al/gen/modules.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace al::gen {

namespace oid = level::id;
using level::LevelObject;

namespace {

constexpr float kCell = 30.0f;
constexpr float kModuleGap = 12.0f; // free space between structures, so they stay separate shapes

struct Box {
    float x0, y0, x1, y1;
};
bool overlap(const Box& a, const Box& b, float m) { return !(a.x1 + m <= b.x0 || b.x1 + m <= a.x0 || a.y1 + m <= b.y0 || b.y1 + m <= a.y0); }
Box cellBox(int cx, int cy) { return {cx * kCell, cy * kCell, cx * kCell + kCell, cy * kCell + kCell}; }

// Reserved space: player rects of every run that must stay possible.
class Envelope {
public:
    void add(float x, float y, float half) { addBox({x - half, y - half, x + half, y + half}); }
    void addBox(const Box& b) {
        for (int c = col(b.x0); c <= col(b.x1); ++c) at(c).push_back(b);
    }
    bool hits(const Box& b, float m) const {
        for (int c = col(b.x0 - m); c <= col(b.x1 + m); ++c) {
            if (c < 0 || c >= int(m_cols.size())) continue;
            for (auto& r : m_cols[size_t(c)]) if (overlap(b, r, m)) return true;
        }
        return false;
    }
    // whether the reserved space in column c has something above y (dir > 0) or below it
    bool beyond(int c, float y, int dir) const {
        if (c < 0 || c >= int(m_cols.size())) return false;
        for (auto& r : m_cols[size_t(c)]) if (dir > 0 ? r.y0 >= y : r.y1 <= y) return true;
        return false;
    }
    // lowest / highest reserved y in a column (false if the column is empty)
    bool range(int c, float& lo, float& hi) const {
        if (c < 0 || c >= int(m_cols.size()) || m_cols[size_t(c)].empty()) return false;
        lo = 1e9f;
        hi = -1e9f;
        for (auto& r : m_cols[size_t(c)]) { lo = std::min(lo, r.y0); hi = std::max(hi, r.y1); }
        return true;
    }

private:
    static int col(float x) { return int(std::floor(x / kCell)); }
    std::vector<Box>& at(int c) {
        c = std::max(0, c);
        if (c >= int(m_cols.size())) m_cols.resize(size_t(c) + 1);
        return m_cols[size_t(c)];
    }
    std::vector<std::vector<Box>> m_cols;
};

struct Sample {
    float x, y, half;
    Mode mode;
    int g;
    bool onGround;
    bool bounded;
    float floorY, ceilY;
    double v;
    uint32_t events;
};

Sample sampleOf(const phys::GameState& s) {
    const auto& p = s.p[0];
    return {p.x, p.y, p.height() * 0.5f, p.mode, p.flip(), p.onGround, p.bounded, p.floorY, p.ceilY, p.v, p.events};
}

LevelObject make(int id, double x, double y, double rot = 0) {
    LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

bool isFlyingMode(Mode m) { return m == Mode::Ship || m == Mode::Ufo || m == Mode::Wave || m == Mode::Swing; }

class Structurer {
public:
    Structurer(const PathPlan& path, const DesignPlan& plan, const GenOptions& opt, Rng& rng, const StructureParams& prm)
        : m_path(path), m_plan(plan), m_opt(opt), m_rng(rng), m_prm(prm) {}
    StructureResult run();

private:
    const PathPlan& m_path;
    const DesignPlan& m_plan;
    const GenOptions& m_opt;
    Rng& m_rng;
    StructureParams m_prm;
    StructureResult m_res;

    level::Level m_base;              // path objects + platforms (their outline pieces)
    CellGrid m_platforms;             // the path's platform cells (structures keep off them)
    phys::World m_W;                  // grows with every placed structure
    const phys::World* m_simWorld = nullptr; // the world runFrom uses
    Envelope m_env;
    std::vector<Sample> m_nominal;    // tick by tick (index = tick - 1)
    std::vector<phys::GameState> m_pre; // state before each used click (index = PathClick index)
    std::vector<LevelObject> m_hazards;
    std::set<std::pair<int, int>> m_hazardCells;
    float m_margin = 4;               // extra distance kept from the reserved space
    int m_halfWin = 12;               // ticks each press may move either way
    float m_density = 0.5f;
    int m_style = 0;                  // structure style (modules.hpp)
    Envelope m_built;                 // everything placed so far
    CellGrid m_lines;                 // wave chains, always line slopes
    std::vector<LevelObject> m_objects; // the modules' pieces
    uint64_t m_seed = 0;
    // random choices tied to a place in the level (re-rolling one stretch leaves the others)
    Rng at(int cx, uint64_t what) const { return Rng(combineSeed(m_seed ^ what, uint64_t(int64_t(cx)))); }

    float intensityAt(double t) const {
        for (auto& p : m_plan.parts) if (t >= p.t0 && t < p.t1) return p.intensity;
        return 0.5f;
    }
    int nextPressAfter(int tick) const {
        for (auto& e : m_path.inputs.events) if (e.press && e.tick > tick) return e.tick;
        return m_path.endTick;
    }
    // the inputs with click i moved by `shift` ticks (or left out); a press takes its own release
    // along, a wave release click moves alone
    std::vector<bot::InputEvent> shifted(size_t i, int shift, bool remove) const;
    // ticks the press/release of click i may move to: (lo, hi) exclusive
    void moveRange(size_t i, int& lo, int& hi) const;
    std::set<int> m_releaseTicks;     // release events that are clicks of their own (wave)
    bool runFrom(const phys::GameState& from, const std::vector<bot::InputEvent>& ev, int until, std::vector<Sample>* out, int* deathTick = nullptr) const;
    void buildEnvelope();
    bool cellFree(int cx, int cy) const;
    bool hazardFree(const LevelObject& o) const;
    // tentative placement: add, re-check, keep or undo
    size_t mark() const { return m_W.size(); }
    void addCell(int cx, int cy, CellKind k = CellKind::Solid);
    void addHazard(const LevelObject& o);
    void necessity();
    bool tryKill(size_t clickIndex, const std::vector<Sample>& skip, const std::vector<bot::InputEvent>& ev, int horizon);
    void placeModules();
    bool placeModule(const Module& m, bool mirrorX, bool under, int cx, int row, int floorRow, int ceilRow);
    bool objectFits(const LevelObject& o, Box* out) const;
    void placeObject(const LevelObject& o);
    void waveChains();
    bool chevron(float x, float y, bool up, bool over, int left, int right, int floorRow, int ceilRow);
    void lineCell(int cx, int cy, CellKind k);
    void measureWindows();
};

std::vector<bot::InputEvent> Structurer::shifted(size_t ci, int shift, bool remove) const {
    const PathClick& c = m_path.clicks[ci];
    const bool release = c.act == Act::Release;
    std::vector<bot::InputEvent> ev;
    const auto& src = m_path.inputs.events;
    for (size_t i = 0; i < src.size(); ++i) {
        if (release) {
            if (!src[i].press && src[i].tick == c.tick) {
                if (!remove) ev.push_back({c.tick + shift, false});
                continue;
            }
            ev.push_back(src[i]);
            continue;
        }
        if (src[i].press && src[i].tick == c.tick) {
            const bool relIsClick = i + 1 < src.size() && m_releaseTicks.count(src[i + 1].tick);
            const int rel = i + 1 < src.size() ? src[i + 1].tick : c.tick + 3;
            if (!remove) ev.push_back({c.tick + shift, true});
            if (relIsClick) ev.push_back(src[i + 1]);
            else if (!remove) ev.push_back({rel + shift, false});
            ++i; // the release is handled
            continue;
        }
        ev.push_back(src[i]);
    }
    std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    return ev;
}

void Structurer::moveRange(size_t ci, int& lo, int& hi) const {
    const PathClick& c = m_path.clicks[ci];
    const auto& src = m_path.inputs.events;
    lo = 0;
    hi = m_path.endTick;
    if (c.act == Act::Release) {
        for (auto& e : src) if (e.press && e.tick < c.tick) lo = e.tick;
        hi = nextPressAfter(c.tick);
        return;
    }
    for (auto& e : src) if (!e.press && e.tick < c.tick) lo = e.tick;
    hi = nextPressAfter(c.tick);
    for (auto& e : src)
        if (!e.press && e.tick > c.tick) {
            if (m_releaseTicks.count(e.tick)) hi = std::min(hi, e.tick); // its release is the next click
            break;
        }
}

bool Structurer::runFrom(const phys::GameState& from, const std::vector<bot::InputEvent>& ev, int until, std::vector<Sample>* out, int* deathTick) const {
    phys::GameState s = from;
    bool held = s.pressed;
    size_t ei = 0;
    while (ei < ev.size() && ev[ei].tick <= s.tick) { held = ev[ei].press; ++ei; }
    while (s.tick < until && !s.dead && !s.won) {
        int next = s.tick + 1;
        while (ei < ev.size() && ev[ei].tick <= next) { held = ev[ei].press; ++ei; }
        phys::step(*m_simWorld, s, held);
        if (out) out->push_back(sampleOf(s));
    }
    if (deathTick) *deathTick = s.dead ? s.tick : -1;
    return !s.dead;
}

void Structurer::buildEnvelope() {
    // the designed run
    for (auto& q : m_nominal) m_env.add(q.x, q.y, q.half);
    // every click pressed early and late (within the window); a variant only counts while it does
    // what the click is meant to do (the orb is taken, the jump happens)
    const uint32_t kClickEvents = phys::EvJump | phys::EvOrb | phys::EvUfoJump | phys::EvSwingFlip | phys::EvTeleport;
    for (size_t i = 0; i < m_path.clicks.size(); ++i) {
        const PathClick& c = m_path.clicks[i];
        const int T = c.tick;
        uint32_t want = 0;
        for (int t = T; t <= T + 3 && t - 1 < int(m_nominal.size()); ++t) want |= m_nominal[size_t(t - 1)].events & kClickEvents;
        const int horizon = std::min(m_path.endTick, nextPressAfter(T) + 15);
        int lo, hi;
        moveRange(i, lo, hi);
        for (int s = -m_halfWin; s <= m_halfWin; s += (std::abs(s) <= 2 ? 1 : 2)) {
            if (s == 0 || T + s <= lo + 1) continue;
            if (s > 0 && T + s >= hi - 1) continue;
            std::vector<Sample> tr;
            auto ev = shifted(i, s, false);
            const int startTick = m_pre[i].tick;
            runFrom(m_pre[i], ev, horizon, &tr);
            if (want) {
                uint32_t got = 0;
                for (int t = T + s - 1; t <= T + s + 4; ++t) {
                    int k = t - startTick - 1;
                    if (k >= 0 && k < int(tr.size())) got |= tr[size_t(k)].events & kClickEvents;
                }
                if (!(got & want)) continue; // pressed too early/late to do anything: outside the window
            }
            for (auto& q : tr) m_env.add(q.x, q.y, q.half);
        }
    }
}

bool Structurer::cellFree(int cx, int cy) const {
    if (cy < 0) return false;
    if (m_res.cells.filled(cx, cy)) return false;
    // never on or next to a platform (its outline pieces must stay exactly as planned)
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (m_platforms.filled(cx + dx, cy + dy)) return false;
    return !m_env.hits(cellBox(cx, cy), m_margin) && !m_built.hits(cellBox(cx, cy), 2);
}

bool Structurer::hazardFree(const LevelObject& o) const {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return true;
    float hw = so.hw, hh = so.hh;
    if (so.kind == level::Kind::Saw) hh = hw;
    Box b{so.x - hw, so.y - hh, so.x + hw, so.y + hh};
    if (m_env.hits(b, m_margin + 1) || m_built.hits(b, 2)) return false;
    // a saw may cut into a wall, but its centre stays in the open (not buried in the blocks)
    if (so.kind == level::Kind::Saw && m_res.cells.filled(int(std::floor(so.x / kCell)), int(std::floor(so.y / kCell)))) return false;
    // keep the path's orbs, pads and portals readable
    for (auto& po : m_path.objects)
        if (std::fabs(po.o.x - o.x) < 36 && std::fabs(po.o.y - o.y) < 36) return false;
    return true;
}

void Structurer::addCell(int cx, int cy, CellKind k) {
    m_res.cells.set(cx, cy, k);
    if (k == CellKind::Solid) {
        LevelObject b = make(oid::Block, cx * kCell + 15, cy * kCell + 15);
        m_W.addObject(b, -1);
    } else {
        // the slope object exactly as the tiler places it (a 2-wide slope with its left half)
        CellGrid one;
        one.set(cx, cy, k);
        std::vector<LevelObject> objs;
        tileCells(one, TileLook::Outline, objs);
        for (auto& o : objs) if (o.info().kind == level::Kind::Slope) m_W.addObject(o, -1);
    }
}

void Structurer::addHazard(const LevelObject& o) {
    m_hazards.push_back(o);
    m_W.addObject(o, -1);
    m_hazardCells.insert({int(std::floor(o.x / kCell)), int(std::floor(o.y / kCell))});
}

bool Structurer::tryKill(size_t i, const std::vector<Sample>& skip, const std::vector<bot::InputEvent>& ev, int horizon) {
    const phys::GameState& pre = m_pre[i];
    auto dies = [&]() {
        int dt = -1;
        runFrom(pre, ev, horizon, nullptr, &dt);
        return dt >= 0;
    };
    for (size_t j = 0; j < skip.size(); j += 3) {
        const Sample& q = skip[j];
        if (q.mode == Mode::Wave) continue; // wave parts keep their clean triangles
        // (no test on the player box here: only the hazard has to keep off the reserved space,
        // e.g. a spike right under the arc of the jump)
        const int cx = int(std::floor((q.x + q.half + 2) / kCell));
        const int cyc = int(std::floor(q.y / kCell));
        const size_t mk = mark();
        // 1. a spike on the surface the player runs on
        if (q.onGround) {
            const float surf = q.y - float(q.g) * q.half;
            LevelObject sp = make(oid::Spike, cx * kCell + 15, surf + float(q.g) * 15.0f, q.g > 0 ? 0 : 180);
            if (hazardFree(sp)) {
                addHazard(sp);
                if (dies()) return true;
                m_hazards.pop_back();
                m_W.truncate(mk);
            }
        }
        // 2. a saw in the way
        for (int id : {oid::SawMedium, oid::SawSmall}) {
            LevelObject sw = make(id, std::round((q.x + q.half + 8) / 15.0f) * 15.0f, std::round(q.y / 15.0f) * 15.0f);
            if (!hazardFree(sw)) continue;
            addHazard(sw);
            if (dies()) { ++m_res.saws; return true; }
            m_hazards.pop_back();
            m_W.truncate(mk);
        }
        if (j > 200) break;
    }
    return false;
}

void Structurer::necessity() {
    for (size_t i = 0; i < m_path.clicks.size(); ++i) {
        const PathClick& c = m_path.clicks[i];
        ClickCheck ck;
        ck.click = c.click;
        const int horizon = std::min(m_path.endTick, std::max(nextPressAfter(c.tick) + 30, c.tick + 90));
        auto ev = shifted(i, 0, true);
        std::vector<Sample> skip;
        int dt = -1;
        runFrom(m_pre[i], ev, horizon, &skip, &dt);
        if (dt >= 0) ck.forced = true;
        else if (tryKill(i, skip, ev, horizon)) { ck.forced = true; ++m_res.hazards; }
        m_res.checks.push_back(ck);
    }
}

bool Structurer::objectFits(const LevelObject& o, Box* out) const {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return true;
    const float hw = so.hw, hh = so.kind == level::Kind::Saw ? so.hw : so.hh;
    const Box b{so.x - hw, so.y - hh, so.x + hw, so.y + hh};
    if (out) *out = b;
    const bool hazard = so.kind == level::Kind::Hazard || so.kind == level::Kind::Saw || so.kind == level::Kind::HazardSlope;
    if (m_env.hits(b, m_margin + (hazard ? 2.0f : 0.0f))) return false;
    if (m_built.hits(b, kModuleGap)) return false;
    for (auto& po : m_path.objects)
        if (std::fabs(po.o.x - so.x) < 40 + hw && std::fabs(po.o.y - so.y) < 40 + hh) return false;
    return true;
}

void Structurer::placeObject(const LevelObject& o) {
    m_objects.push_back(o);
    m_W.addObject(o, -1);
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return;
    const float hh = so.kind == level::Kind::Saw ? so.hw : so.hh;
    m_built.addBox({so.x - so.hw, so.y - hh, so.x + so.hw, so.y + hh});
}

// A module at cell (cx, row), its lower left corner. `under`: it is mirrored to stand under the
// path. Thin rails running out of the bounds are cut off; anything else out of them rejects it.
bool Structurer::placeModule(const Module& m, bool mirrorX, bool under, int cx, int row, int floorRow, int ceilRow) {
    const ModulePiece* pieces = modulePieces() + m.first;
    std::vector<LevelObject> objs;
    objs.reserve(size_t(m.count));
    for (int i = 0; i < m.count; ++i) {
        const ModulePiece& p = pieces[i];
        float x = p.x, y = p.y;
        int rot = p.rot;
        bool fx = p.fx != 0, fy = p.fy != 0;
        if (under) { y = float(m.h) * kCell - y; rot = -rot; fy = !fy; }
        if (mirrorX) { x = float(m.w) * kCell - x; rot = -rot; fx = !fx; }
        LevelObject o = make(p.id, double(cx) * kCell + x, double(row) * kCell + y, rot);
        o.flipX = fx;
        o.flipY = fy;
        Box b{};
        if (!objectFits(o, &b)) return false;
        const float lowest = float(floorRow) * kCell, highest = float(ceilRow + 1) * kCell;
        if (b.y0 < lowest - 0.5f || b.y1 > highest + 0.5f) {
            if (o.info().kind != level::Kind::Solid || std::min(b.x1 - b.x0, b.y1 - b.y0) > 4) return false;
            continue;
        }
        objs.push_back(o);
    }
    if (objs.empty()) return false;
    for (auto& o : objs) placeObject(o);
    ++m_res.pillars;
    return true;
}

// Every gamemode stretch uses a small set of shapes and repeats them, as real layouts do: that
// repetition is what makes a stretch look designed. A lane runs over the path and one under it,
// each structure as close to the reserved space as it fits.
void Structurer::placeModules() {
    if (m_density <= 0.01f || m_nominal.empty()) return;
    size_t moduleCount = 0;
    const Module* table = moduleTable(moduleCount);
    const StyleStats& st = structureStyle(m_style);
    const int n = int(m_nominal.size());
    for (int i = 0; i < n;) {
        const Sample& q = m_nominal[size_t(i)];
        int j = i;
        while (j < n && m_nominal[size_t(j)].mode == q.mode) ++j;
        const int c0 = int(std::floor(q.x / kCell)) + 2, c1 = int(std::floor(m_nominal[size_t(j - 1)].x / kCell)) - 1;
        if (q.mode == Mode::Wave || c1 - c0 < 3) { i = j; continue; }
        Rng r = at(c0, 0x90D1);
        const int bit = isFlyingMode(q.mode) ? 2 : 1;
        // the palette of this stretch
        std::vector<std::pair<const Module*, bool>> palette;
        const int size = 2 + int(r.below(3));
        for (int tries = 0; tries < 400 && int(palette.size()) < size; ++tries) {
            const Module& m = table[r.below(moduleCount)];
            if (!(m.modes & bit) || m.weight[m_style] <= 0) continue;
            if (!r.chance(std::min(1.0, 0.15 + double(m.weight[m_style]) / 12.0))) continue;
            bool have = false;
            for (auto& e : palette) have = have || e.first == &m;
            if (!have) palette.push_back({&m, r.chance(0.5)});
        }
        if (palette.empty()) { i = j; continue; }
        static const int rhythm[8] = {0, 1, 0, 2, 0, 1, 0, 3};
        size_t si = size_t(i);
        for (int lane = 0; lane < 2; ++lane) {
            const bool under = lane == 1;
            si = size_t(i);
            int cx = c0 + (under ? 1 + int(r.below(3)) : 0);
            int step = 0;
            while (cx < c1) {
                const auto& e = palette[size_t(rhythm[step % 8]) % palette.size()];
                const Module& m = *e.first;
                while (si + 1 < size_t(j) && m_nominal[si].x < (float(cx) + 0.5f) * kCell) ++si;
                const Sample& s = m_nominal[si];
                float lo = 1e9f, hi = -1e9f;
                for (int x = cx; x < cx + m.w; ++x) {
                    float a, b;
                    if (m_env.range(x, a, b)) { lo = std::min(lo, a); hi = std::max(hi, b); }
                }
                bool placed = false;
                const int floorRow = s.bounded ? int(std::lround(s.floorY / kCell)) : 0;
                const int ceilRow = s.bounded ? int(std::lround(s.ceilY / kCell)) - 1 : 90;
                if (hi > lo) {
                    const int base = under ? int(std::floor(lo / kCell)) - m.h + 2 : int(std::floor(hi / kCell)) - 1;
                    for (int d = 0; d < 7 && !placed; ++d) {
                        const int row = under ? base - d : base + d;
                        if (row < floorRow && under && row + m.h <= floorRow) break;
                        placed = placeModule(m, e.second, under, cx, row, floorRow, ceilRow);
                    }
                }
                if (placed) {
                    const float dens = std::clamp(m_density * (0.6f + 0.8f * intensityAt(double(si + 1) / phys::kTPS)), 0.05f, 1.0f);
                    const float pitch = st.pitchLo + (st.pitchHi - st.pitchLo) * (1.0f - dens) * 0.45f;
                    const int gap = std::max(1, int(std::lround(pitch))) + int(r.below(2));
                    if (gap >= 3 && r.chance(std::min(0.5, double(st.saws) * 0.3))) {
                        const float side = under ? lo - m_margin - 26 : hi + m_margin + 26;
                        LevelObject sw = make(oid::SawMedium, (float(cx + m.w) + float(gap) * 0.5f) * kCell, std::round(side / 15.0f) * 15.0f);
                        if (objectFits(sw, nullptr)) { placeObject(sw); ++m_res.saws; }
                    }
                    cx += m.w + gap;
                    ++step;
                } else {
                    cx += 1;
                }
            }
        }
        i = j;
    }
}

// One arm of a chain of line slopes.
void Structurer::lineCell(int cx, int cy, CellKind k) {
    m_lines.set(cx, cy, k);
    CellGrid one;
    one.set(cx, cy, k);
    std::vector<LevelObject> objs;
    tileCells(one, TileLook::Outline, objs);
    for (auto& o : objs) if (o.info().kind == level::Kind::Slope) m_W.addObject(o, -1);
    m_built.addBox({float(cx) * kCell, float(cy) * kCell, float(cx + 1) * kCell, float(cy + 1) * kCell});
    ++m_res.corridorCells;
}

// A chevron along a turn of the zig-zag. up: it points up (a peak); over: it runs over the path
// (else under it). Moved away from the path until every cell is clear of the reserved space, so
// the gap the player flies through comes from the difficulty's margin.
bool Structurer::chevron(float x, float y, bool up, bool over, int left, int right, int floorRow, int ceilRow) {
    const int c = int(std::lround(x / kCell));
    const int start = over ? int(std::ceil(y / kCell)) : int(std::floor(y / kCell)) - 1;
    const int dir = over ? 1 : -1;
    const CellKind leftKind = up ? (over ? CellKind::SlopeCeilUp : CellKind::SlopeFloorUp) : (over ? CellKind::SlopeCeilDown : CellKind::SlopeFloorDown);
    const CellKind rightKind = up ? (over ? CellKind::SlopeCeilDown : CellKind::SlopeFloorDown) : (over ? CellKind::SlopeCeilUp : CellKind::SlopeFloorUp);
    const int slope = up ? -1 : 1;
    for (int d = 0; d < 7; ++d) {
        const int row = start + dir * d;
        struct C { int x, y; CellKind k; };
        std::vector<C> cells;
        bool ok = true;
        auto add = [&](int cx, int cy, CellKind k) {
            if (cy < floorRow || cy > ceilRow) return;
            if (m_env.hits(cellBox(cx, cy), m_margin) || m_built.hits(cellBox(cx, cy), 1)) ok = false;
            cells.push_back({cx, cy, k});
        };
        for (int i = 0; i < left && ok; ++i) add(c - 1 - i, row + slope * i, leftKind);
        for (int i = 0; i < right && ok; ++i) add(c + i, row + slope * i, rightKind);
        if (!ok) continue;
        if (int(cells.size()) < 3) return false;
        for (auto& e : cells) lineCell(e.x, e.y, e.k);
        return true;
    }
    return false;
}

// Wave parts: line slopes built to the click pattern. Every turn of the zig-zag gets a chevron on
// the outside of the turn and one on the inside, so the player flies through a corridor of thin
// 45-degree chains with breaks in the middle of the strokes.
void Structurer::waveChains() {
    if (m_density <= 0.01f) return;
    const StyleStats& st = structureStyle(m_style);
    const int n = int(m_nominal.size());
    for (int i = 0; i < n;) {
        const Sample& q = m_nominal[size_t(i)];
        if (q.mode != Mode::Wave) { ++i; continue; }
        int j = i;
        while (j < n && m_nominal[size_t(j)].mode == Mode::Wave) ++j;
        struct Turn { float x, y; bool peak; };
        std::vector<Turn> turns;
        for (int k = i + 1; k + 1 < j; ++k) {
            const Sample& a = m_nominal[size_t(k - 1)];
            const Sample& b = m_nominal[size_t(k)];
            const Sample& c = m_nominal[size_t(k + 1)];
            const bool peak = b.y > a.y && b.y >= c.y, dip = b.y < a.y && b.y <= c.y;
            if ((peak || dip) && (turns.empty() || b.x - turns.back().x > 20)) turns.push_back({b.x, b.y, peak});
        }
        const float x0 = m_nominal[size_t(i)].x, x1 = m_nominal[size_t(j - 1)].x;
        for (size_t t = 0; t < turns.size(); ++t) {
            const Turn& u = turns[t];
            const float before = u.x - (t > 0 ? turns[t - 1].x : x0), after = (t + 1 < turns.size() ? turns[t + 1].x : x1) - u.x;
            const int left = std::clamp(int(std::lround(before / kCell * 0.7f)), 2, 12), right = std::clamp(int(std::lround(after / kCell * 0.7f)), 2, 12);
            const Sample& s = m_nominal[size_t(i)];
            const int floorRow = s.bounded ? int(std::lround(s.floorY / kCell)) : 0;
            const int ceilRow = s.bounded ? int(std::lround(s.ceilY / kCell)) - 1 : 90;
            chevron(u.x, u.y, u.peak, u.peak, left, right, floorRow, ceilRow);
            chevron(u.x, u.y, u.peak, !u.peak, std::max(2, left - 1), std::max(2, right - 1), floorRow, ceilRow);
            // saws in the pockets on the outside of the turn
            Rng r = at(int(std::floor(u.x / kCell)), 0x5A3E);
            if (r.chance(std::min(0.45, double(st.saws) * 0.25))) {
                const float sy = u.peak ? u.y - 60 : u.y + 60;
                LevelObject sw = make(oid::SawMedium, std::round((u.x + after * 0.5f) / 15.0f) * 15.0f, std::round(sy / 15.0f) * 15.0f);
                if (objectFits(sw, nullptr)) { placeObject(sw); ++m_res.saws; }
            }
        }
        i = j;
    }
}

void Structurer::measureWindows() {
    for (size_t i = 0; i < m_path.clicks.size() && i < m_res.checks.size(); ++i) {
        const PathClick& c = m_path.clicks[i];
        const int horizon = std::min(m_path.endTick, nextPressAfter(c.tick) + 15);
        int lo, hi;
        moveRange(i, lo, hi);
        auto ok = [&](int s) {
            if (c.tick + s <= lo + 1 || c.tick + s >= hi - 1) return false;
            return runFrom(m_pre[i], shifted(i, s, false), horizon, nullptr);
        };
        int e = 0, l = 0;
        while (e < m_halfWin * 2 && ok(-(e + 1))) ++e;
        while (l < m_halfWin * 2 && ok(l + 1)) ++l;
        m_res.checks[i].early = e;
        m_res.checks[i].late = l;
    }
}

StructureResult Structurer::run() {
    const DifficultyProfile prof = profileAt(m_plan.baseDifficulty);
    m_halfWin = std::clamp(int(std::lround(prof.minWindowMs * 0.5 / 1000.0 * phys::kTPS)), 4, 30);
    // 5 units on top: on runs logged in GD the simulator's path is off by a few units after some pads
    m_margin = std::clamp(prof.clearance * 0.3f + 2.0f, 2.0f, 16.0f) + 5.0f;
    m_seed = m_rng.next();
    if (m_opt.structureStyle > 0) {
        m_style = std::clamp(m_opt.structureStyle - 1, 0, kStructureStyles - 1);
    } else {
        // automatic: each variant its own style, mostly the rail style of Bidal Wave
        const uint64_t pick = m_seed % 20;
        m_style = pick < 9 ? 0 : pick < 15 ? 1 : pick < 18 ? 2 : 3;
    }
    m_density = m_prm.density >= 0 ? m_prm.density : (m_plan.style == Style::Minimal ? 0.3f : m_plan.style == Style::Chaotic ? 0.85f : 0.65f);
    // the path as planned: its objects and platform pieces (same objects, same order)
    m_base = pathLevel(m_path);
    for (auto& c : m_path.clicks) if (c.act == Act::Release) m_releaseTicks.insert(c.tick);
    for (auto& sf : m_path.surfaces) m_platforms.fill(sf.cx0, sf.cy0, sf.cx1, sf.cy1);
    m_W = phys::World();
    m_W.setSettings(m_path.start);
    m_W.setEndX(1e9f);
    for (size_t i = 0; i < m_base.objects.size(); ++i) m_W.addObject(m_base.objects[i], int(i));
    m_simWorld = &m_W;
    // the designed run, with the state before every click
    auto nominalRun = [&](const phys::World& W, std::vector<Sample>& samples, std::vector<phys::GameState>& pre) {
        samples.clear();
        pre.assign(m_path.clicks.size(), phys::GameState{});
        phys::GameState s = phys::initialState(W);
        const auto& ev = m_path.inputs.events;
        size_t ci = 0, ei = 0;
        bool held = false;
        while (s.tick < m_path.endTick && !s.dead) {
            // the state well before each click (early presses are simulated from there)
            while (ci < m_path.clicks.size() && std::max(0, m_path.clicks[ci].tick - 2 * m_halfWin - 3) <= s.tick) { pre[ci] = s; ++ci; }
            int next = s.tick + 1;
            while (ei < ev.size() && ev[ei].tick <= next) { held = ev[ei].press; ++ei; }
            phys::step(W, s, held);
            samples.push_back(sampleOf(s));
        }
        for (; ci < m_path.clicks.size(); ++ci) pre[ci] = s;
        return s.dead ? s.tick : -1;
    };
    if (int dt = nominalRun(m_W, m_nominal, m_pre); dt >= 0) m_res.notes.push_back("the path dies before the structures are built (tick " + std::to_string(dt) + ")");
    buildEnvelope();
    waveChains();
    placeModules();
    // what the structures do not force yet gets a hazard of its own
    if (m_prm.necessity) necessity();
    else for (auto& c : m_path.clicks) m_res.checks.push_back({c.click, false, 0, 0});
    // ---- the finished layout: path objects, platform pieces (as planned), structures, hazards
    level::Level& L = m_res.level;
    L = m_base;
    tileCells(m_res.cells, m_prm.look, L.objects);
    tileCells(m_lines, TileLook::Outline, L.objects);
    for (auto& o : m_objects) L.objects.push_back(o);
    for (auto& h : m_hazards) L.objects.push_back(h);
    // the end: a little before the last planned position (the run beyond it is not planned)
    LevelObject end = make(oid::TrigEnd, std::floor(((m_nominal.empty() ? 600.0f : m_nominal.back().x) - 45.0f) / 30.0f) * 30.0f + 15, 600);
    end.setProp(36, std::string("1"));
    L.objects.push_back(end);
    // ---- verified on the finished layout itself: the designed run, then the timing windows
    phys::World W(L);
    W.setEndX(1e9f);
    std::vector<Sample> finalRun;
    std::vector<phys::GameState> finalPre;
    if (int dt = nominalRun(W, finalRun, finalPre); dt >= 0) m_res.notes.push_back("the finished layout kills the designed run at tick " + std::to_string(dt));
    m_pre = std::move(finalPre);
    m_simWorld = &W;
    measureWindows();
    phys::World W2(L);
    m_res.beatable = bot::isBeatable(W2, m_path.inputs);
    for (auto [k, kind] : m_platforms.cells()) m_res.cells.set(int(int32_t(uint32_t(k >> 32))), int(int32_t(uint32_t(k))), kind);
    return m_res;
}

} // namespace

StructureResult buildStructures(const PathPlan& path, const DesignPlan& plan, const GenOptions& opt, Rng& rng, const StructureParams& prm) {
    Structurer s(path, plan, opt, rng, prm);
    return s.run();
}

} // namespace al::gen
