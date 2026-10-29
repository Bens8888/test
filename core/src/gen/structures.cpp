// Generator v2: structures around the gameplay path (see structures.hpp).
#include <al/gen/structures.hpp>

#include <al/bot/bot.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace al::gen {

namespace oid = level::id;
using level::LevelObject;

namespace {

constexpr float kCell = 30.0f;
constexpr int kTopRow = 17;

struct Box {
    float x0, y0, x1, y1;
};
bool overlap(const Box& a, const Box& b, float m) { return !(a.x1 + m <= b.x0 || b.x1 + m <= a.x0 || a.y1 + m <= b.y0 || b.y1 + m <= a.y0); }
Box cellBox(int cx, int cy) { return {cx * kCell, cy * kCell, cx * kCell + kCell, cy * kCell + kCell}; }

// Reserved space: player rects of every run that must stay possible.
class Envelope {
public:
    void add(float x, float y, float half) {
        Box b{x - half, y - half, x + half, y + half};
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
    std::vector<bot::InputEvent> shifted(int pressTick, int shift, bool remove) const;
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
    void frame();
    void clusters();
    void walls();
    void edgeSpikes();
    // spikes along a run of free cells (same row, consecutive columns): groups of 1-3 with gaps,
    // as layouts place them (never a long identical row)
    void spikeRun(const std::vector<std::pair<int, int>>& run, double rot, double chance, double near);
    bool spikeAt(int cx, int cy, double rot, double near);
    void corridors();
    bool pillar(int cx, int rowFrom, int rowTo, int width, bool spikeTip, bool down, int floorRow);
    void measureWindows();
};

std::vector<bot::InputEvent> Structurer::shifted(int pressTick, int shift, bool remove) const {
    std::vector<bot::InputEvent> ev;
    const auto& src = m_path.inputs.events;
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i].press && src[i].tick == pressTick) {
            int rel = i + 1 < src.size() ? src[i + 1].tick : pressTick + 3;
            if (!remove) {
                ev.push_back({pressTick + shift, true});
                ev.push_back({rel + shift, false});
            }
            ++i; // skip the release
            continue;
        }
        ev.push_back(src[i]);
    }
    std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    return ev;
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
        int prevRelease = 0;
        for (auto& e : m_path.inputs.events) if (!e.press && e.tick < T) prevRelease = e.tick;
        for (int s = -m_halfWin; s <= m_halfWin; s += (std::abs(s) <= 2 ? 1 : 2)) {
            if (s == 0 || T + s <= prevRelease + 1) continue;
            if (s > 0 && T + s >= nextPressAfter(T) - 1) continue;
            std::vector<Sample> tr;
            auto ev = shifted(T, s, false);
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
    return !m_env.hits(cellBox(cx, cy), m_margin);
}

bool Structurer::hazardFree(const LevelObject& o) const {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return true;
    float hw = so.hw, hh = so.hh;
    if (so.kind == level::Kind::Saw) hh = hw;
    Box b{so.x - hw, so.y - hh, so.x + hw, so.y + hh};
    if (m_env.hits(b, m_margin + 1)) return false;
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

// A pillar of `width` columns from rowFrom to rowTo (inclusive, rowFrom <= rowTo), optionally with
// spikes on its free end (down = the free end is the bottom). All cells must be free.
bool Structurer::pillar(int cx, int rowFrom, int rowTo, int width, bool spikeTip, bool down, int floorRow) {
    if (rowFrom > rowTo) return false;
    for (int x = cx; x < cx + width; ++x)
        for (int y = rowFrom; y <= rowTo; ++y)
            if (!cellFree(x, y)) return false;
    (void)floorRow;
    for (int x = cx; x < cx + width; ++x)
        for (int y = rowFrom; y <= rowTo; ++y) addCell(x, y);
    ++m_res.pillars;
    if (spikeTip) {
        const int row = down ? rowFrom - 1 : rowTo + 1;
        for (int x = cx; x < cx + width; ++x) {
            LevelObject sp = make(oid::Spike, x * kCell + 15, row * kCell + 15, down ? 180 : 0);
            if (row >= 0 && cellFree(x, row) && hazardFree(sp)) addHazard(sp);
        }
    }
    return true;
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
        // 2. a pillar in the way: rising when falling (towards the gravity floor), hanging otherwise
        if (!q.onGround && !(q.mode == Mode::Wave)) {
            const bool falling = double(q.g) * q.v <= 0;
            const bool rise = (q.g > 0) == falling;
            const int floorRow = q.bounded ? int(std::lround(q.floorY / kCell)) : 0;
            const int ceilRow = q.bounded ? int(std::lround(q.ceilY / kCell)) - 1 : kTopRow;
            const CellGrid before = m_res.cells;
            const int pillarsBefore = m_res.pillars;
            const size_t hz = m_hazards.size();
            bool ok = rise ? pillar(cx, floorRow, cyc, 1, true, false, floorRow) : pillar(cx, cyc, ceilRow, 1, true, true, floorRow);
            if (ok) {
                if (dies()) return true;
                m_res.cells = before;
                m_res.pillars = pillarsBefore;
                m_hazards.resize(hz);
                m_W.truncate(mk);
            }
        }
        // 3. a saw in the way
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
        auto ev = shifted(c.tick, 0, true);
        std::vector<Sample> skip;
        int dt = -1;
        runFrom(m_pre[i], ev, horizon, &skip, &dt);
        if (dt >= 0) ck.forced = true;
        else if (tryKill(i, skip, ev, horizon)) { ck.forced = true; ++m_res.hazards; }
        m_res.checks.push_back(ck);
    }
}

void Structurer::frame() {
    // hanging pillars over jump apexes, rising pillars under the path, saws in pockets
    const int n = int(m_nominal.size());
    int lastX = -100;
    for (int i = 2; i + 2 < n; ++i) {
        const Sample& q = m_nominal[size_t(i)];
        const Sample& a = m_nominal[size_t(i - 2)];
        const Sample& b = m_nominal[size_t(i + 2)];
        const int cx = int(std::floor(q.x / kCell));
        if (cx - lastX < 3) continue;
        const bool apex = float(q.g) * q.y > float(q.g) * a.y && float(q.g) * q.y >= float(q.g) * b.y && !q.onGround;
        const bool trough = float(q.g) * q.y < float(q.g) * a.y && float(q.g) * q.y <= float(q.g) * b.y && !q.onGround;
        if (!apex && !trough) continue;
        const float dens = m_density * (0.6f + 0.8f * intensityAt(double(i + 1) / phys::kTPS));
        Rng r = at(cx, 0xF7A3);
        if (!r.chance(std::min(0.95, double(dens)))) continue;
        const int floorRow = q.bounded ? int(std::lround(q.floorY / kCell)) : 0;
        const int ceilRow = q.bounded ? int(std::lround(q.ceilY / kCell)) - 1 : kTopRow;
        const int width = 1 + int(r.below(m_density > 0.6f ? 3 : 2));
        bool placed = false;
        // over an apex (the side away from gravity): a pillar from the top/bottom towards the path
        if (apex == (q.g > 0)) {
            // free space above the path: hanging pillar
            int rowB = int(std::ceil((q.y + q.half + m_margin + 30) / kCell));
            if (rowB <= ceilRow - 1) placed = pillar(cx - width / 2, rowB, ceilRow, width, true, true, floorRow);
        } else {
            int rowT = int(std::floor((q.y - q.half - m_margin - 30) / kCell)) - 1;
            if (rowT >= floorRow) placed = pillar(cx - width / 2, floorRow, rowT, width, true, false, floorRow);
        }
        if (!placed && m_density > 0.35f) {
            // a saw between the path and the floor/ceiling instead
            const float gapLo = q.g > 0 ? float(floorRow) * kCell : q.y + q.half;
            const float gapHi = q.g > 0 ? q.y - q.half : float(ceilRow + 1) * kCell;
            if (gapHi - gapLo > 80) {
                LevelObject sw = make(gapHi - gapLo > 130 ? oid::SawLarge : oid::SawMedium, cx * kCell + 15, std::round((gapLo + gapHi) * 0.5f / 15.0f) * 15.0f);
                if (hazardFree(sw)) {
                    addHazard(sw);
                    ++m_res.saws;
                    placed = true;
                    // a small cluster: one or two smaller saws next to it
                    const int extra = int(r.below(3));
                    static const float off[4][2] = {{38, 22}, {-36, -20}, {30, -30}, {-30, 28}};
                    for (int e = 0, k = int(r.below(4)); e < extra; ++e, k = (k + 1) % 4) {
                        LevelObject s2 = make(oid::SawSmall, sw.x + off[k][0], sw.y + off[k][1]);
                        if (hazardFree(s2)) { addHazard(s2); ++m_res.saws; }
                    }
                }
            }
        }
        if (placed) lastX = cx;
    }
}

// Small block groups floating next to the path, the shapes layouts are made of: a hanging block
// with a spike under it, short slabs with a spike at one end, 2x2 blocks, T shapes, steps,
// columns. Written with the path below them (row 0 is the row nearest the path, spikes on row
// -1 point at it); under the path they are mirrored.
namespace {
struct Motif {
    std::vector<std::pair<int, int>> cells, spikes;
    int width;
};
const std::vector<Motif>& motifs() {
    static const std::vector<Motif> m = {
        {{{0, 0}}, {{0, -1}}, 1},                                         // block, spike
        {{{0, 0}, {1, 0}}, {{0, -1}, {1, -1}}, 2},                        // slab of two, two spikes
        {{{0, 0}, {1, 0}, {2, 0}}, {{2, -1}}, 3},                         // slab of three, end spike
        {{{0, 0}, {1, 0}, {2, 0}}, {{0, -1}}, 3},
        {{{0, 0}, {0, 1}, {0, 2}}, {{0, -1}}, 1},                         // column, spike tip
        {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}, {{0, -1}, {1, -1}}, 2},        // 2x2, two spikes
        {{{0, 0}, {1, 0}, {2, 0}, {1, 1}}, {{1, -1}}, 3},                 // T
        {{{0, 0}, {1, 0}, {2, 0}, {3, 0}}, {{0, -1}, {3, -1}}, 4},        // long slab, both ends
        {{{0, 0}, {1, 0}, {1, 1}}, {{0, -1}}, 2},                         // step
        {{{0, 0}, {1, 0}, {2, 0}}, {}, 3},                                // plain slab
        {{{0, 0}, {0, 1}}, {{0, -1}}, 1},                                 // short column
    };
    return m;
}
} // namespace

void Structurer::clusters() {
    if (m_nominal.empty()) return;
    const auto& lib = motifs();
    const int cFirst = int(std::floor(m_nominal.front().x / kCell)) + 4;
    const int cLast = int(std::floor(m_nominal.back().x / kCell)) - 3;
    // the nominal sample at a column (the player's mode and bounds there)
    size_t si = 0;
    auto sampleAt = [&](int cx) -> const Sample& {
        while (si + 1 < m_nominal.size() && m_nominal[si].x < (cx + 0.5f) * kCell) ++si;
        return m_nominal[si];
    };
    int cx = cFirst;
    while (cx < cLast) {
        const Sample& q = sampleAt(cx);
        const double t = double(si + 1) / phys::kTPS;
        // flying stretches are framed more densely (both sides of the path are open)
        const float modeBoost = isFlyingMode(q.mode) ? 1.5f : 1.0f;
        const float dens = std::clamp(m_density * (0.55f + 0.9f * intensityAt(t)) * modeBoost, 0.05f, 1.0f);
        Rng r = at(cx, 0xC1A5);
        const int gap = 2 + int(std::lround((1.0f - dens) * 4.0f)) + int(r.below(3));
        if (!r.chance(0.35 + 0.6 * double(dens))) { cx += gap; continue; }
        const Motif& mo = lib[r.below(lib.size())];
        const int floorRow = q.bounded ? int(std::lround(q.floorY / kCell)) : 0;
        const int ceilRow = q.bounded ? int(std::lround(q.ceilY / kCell)) - 1 : kTopRow;
        // where the path is over the motif's columns
        float lo = 1e9f, hi = -1e9f;
        bool any = false;
        for (int x = cx - 1; x <= cx + mo.width; ++x) {
            float a, b;
            if (m_env.range(x, a, b)) { lo = std::min(lo, a); hi = std::max(hi, b); any = true; }
        }
        if (!any) { cx += gap; continue; }
        // above the path (hanging) or under it, where there is room for the motif to float
        int height = 0;
        for (auto [x, y] : mo.cells) height = std::max(height, y + 1);
        const bool hasSpikes = !mo.spikes.empty();
        const int aboveRow = int(std::ceil((hi + m_margin) / kCell)) + (hasSpikes ? 1 : 0); // nearest block row above
        const int belowRow = int(std::floor((lo - m_margin) / kCell)) - 1 - (hasSpikes ? 1 : 0);
        const int slack = q.bounded ? 0 : 1; // the bounds of a flying stretch can carry a group
        const bool roomAbove = aboveRow + height - 1 <= ceilRow - slack;
        const bool roomBelow = belowRow - (height - 1) >= floorRow + slack;
        if (!roomAbove && !roomBelow) { cx += gap; continue; }
        bool up = roomAbove && (!roomBelow || r.chance(q.g > 0 ? 0.6 : 0.4));
        bool placed = false;
        for (int attempt = 0; attempt < 2 && !placed; ++attempt, up = !up) {
            if (up ? !roomAbove : !roomBelow) continue;
            // the nearest rows first, then up to two rows further away
            for (int d = 0; d < 3 && !placed; ++d) {
                const int row0 = up ? aboveRow + d : belowRow - d;
                const int dir = up ? 1 : -1;
                auto cellOf = [&](std::pair<int, int> c) { return std::pair<int, int>{cx + c.first, row0 + dir * c.second}; };
                bool ok = true;
                for (auto c : mo.cells) {
                    auto [x, y] = cellOf(c);
                    // floating, or standing on / hanging from the bounds of a flying stretch
                    const bool onBounds = q.bounded && (y == floorRow || y == ceilRow);
                    if (y < floorRow || y > ceilRow || ((y == floorRow || y == ceilRow) && !onBounds) || !cellFree(x, y)) { ok = false; break; }
                    // keep a free cell around it (separate groups, not one blob)
                    for (int dx = -1; dx <= 1 && ok; ++dx)
                        for (int dy = -1; dy <= 1 && ok; ++dy)
                            if (m_res.cells.filled(x + dx, y + dy)) ok = false;
                }
                if (!ok) continue;
                for (auto c : mo.cells) { auto [x, y] = cellOf(c); addCell(x, y); }
                for (auto c : mo.spikes) {
                    auto [x, y] = cellOf(c);
                    if (r.chance(0.55 + 0.45 * double(dens))) spikeAt(x, y, up ? 180 : 0, 0);
                }
                // now and then a spike on its front, pointing at the player
                if (r.chance(0.2)) spikeAt(cx - 1, row0, 270, 0);
                ++m_res.pillars;
                placed = true;
            }
        }
        // no room here: try the next column (a placed group keeps its distance)
        cx += placed ? mo.width + gap : 1;
    }
}

// Walls that follow the path, the backbone of most layouts: pillars hanging from the top and
// rising from the floor, broken into columns 1-3 wide with gaps, each column ending right at the
// reserved space (so the walls step along with the path), spike tips pointing at the player.
void Structurer::walls() {
    if (m_nominal.empty()) return;
    const int cFirst = int(std::floor(m_nominal.front().x / kCell)) + 4;
    const int cLast = int(std::floor(m_nominal.back().x / kCell)) - 2;
    size_t si = 0;
    auto sampleAt = [&](int cx) -> const Sample& {
        while (si + 1 < m_nominal.size() && m_nominal[si].x < (cx + 0.5f) * kCell) ++si;
        return m_nominal[si];
    };
    // the reserved space around column x (with its neighbours: the wall keeps its distance)
    auto band = [&](int x, float& lo, float& hi) {
        lo = 1e9f;
        hi = -1e9f;
        bool any = false;
        for (int c = x - 1; c <= x + 1; ++c) {
            float a, b;
            if (m_env.range(c, a, b)) { lo = std::min(lo, a); hi = std::max(hi, b); any = true; }
        }
        return any;
    };
    int cx = cFirst;
    while (cx < cLast) {
        const Sample& q = sampleAt(cx);
        const double t = double(si + 1) / phys::kTPS;
        const float dens = std::clamp(m_density * (0.6f + 0.8f * intensityAt(t)), 0.05f, 1.0f);
        Rng r = at(cx, 0x3A11);
        const int width = 1 + int(r.below(3));
        const int gapAfter = 1 + int(r.below(dens > 0.7f ? 2 : 3));
        if (!r.chance(0.35 + 0.6 * double(dens))) { cx += width + gapAfter; continue; }
        const int floorRow = q.bounded ? int(std::lround(q.floorY / kCell)) : 0;
        const int ceilRow = q.bounded ? int(std::lround(q.ceilY / kCell)) - 1 : kTopRow;
        float lo0, hi0;
        if (!band(cx, lo0, hi0)) { cx += 1; continue; }
        // the top of a ground stretch: the walls hang from a few rows over the path (off screen)
        const int topRow = q.bounded ? ceilRow : std::min(kTopRow, int(std::ceil(hi0 / kCell)) + 5 + int(r.below(4)));
        const bool top = r.chance(0.55 + 0.35 * double(dens));
        const bool bottom = r.chance(0.45 + 0.4 * double(dens));
        const bool spikes = r.chance(0.6 + 0.35 * double(dens));
        bool placed = false;
        for (int x = cx; x < cx + width; ++x) {
            float lo, hi;
            if (!band(x, lo, hi)) continue;
            if (top) {
                // from the top down to the reserved space (one row left for the spike tip)
                const int nearRow = int(std::ceil((hi + m_margin) / kCell)) + (spikes ? 1 : 0);
                int y = topRow, last = -1;
                for (; y >= nearRow && cellFree(x, y); --y) { addCell(x, y); last = y; placed = true; }
                if (last >= 0 && last == nearRow && spikes) spikeAt(x, last - 1, 180, 0);
            }
            if (bottom) {
                const int nearRow = int(std::floor((lo - m_margin) / kCell)) - 1 - (spikes ? 1 : 0);
                int y = floorRow, last = -1;
                for (; y <= nearRow && cellFree(x, y); ++y) { addCell(x, y); last = y; placed = true; }
                if (last >= 0 && last == nearRow && spikes) spikeAt(x, last + 1, 0, 0);
            }
        }
        if (placed) ++m_res.pillars;
        cx += width + gapAfter;
    }
}

bool Structurer::spikeAt(int cx, int cy, double rot, double near) {
    if (cy < 0 || cy > kTopRow || m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy)) return false;
    if (m_hazardCells.count({cx, cy})) return false;
    // never a spike standing on (or hanging from) another hazard
    if (rot == 0 && m_hazardCells.count({cx, cy - 1})) return false;
    if (rot == 180 && m_hazardCells.count({cx, cy + 1})) return false;
    if (rot == 270 && m_hazardCells.count({cx + 1, cy})) return false;
    LevelObject sp = make(oid::Spike, cx * kCell + 15, cy * kCell + 15, rot);
    // near the path (where it matters), but clear of every fair run
    Box b{cx * kCell, cy * kCell, cx * kCell + kCell, cy * kCell + kCell};
    if (near > 0 && !m_env.hits(b, float(near))) return false;
    // pointing at the path: a spike on top has the path above it, one underneath has it below
    // (never on the far side of a wall)
    if (rot == 0 && !m_env.beyond(cx, b.y1 - 2, 1)) return false;
    if (rot == 180 && !m_env.beyond(cx, b.y0 + 2, -1)) return false;
    if (!hazardFree(sp)) return false;
    addHazard(sp);
    return true;
}

void Structurer::spikeRun(const std::vector<std::pair<int, int>>& run, double rot, double chance, double near) {
    if (run.empty()) return;
    Rng r = at(run.front().first * 64 + run.front().second, 0x5B1E + uint64_t(rot));
    if (!r.chance(chance)) return;
    size_t k = r.below(2); // start on the first or second cell
    while (k < run.size()) {
        const int len = 1 + int(r.below(3));          // a group of 1-3
        for (int n = 0; n < len && k < run.size(); ++n, ++k) spikeAt(run[k].first, run[k].second, rot, near);
        k += 1 + r.below(3);                          // then a gap of 1-3
    }
}

// Spikes on the edges of the structures (and on the floor / the bounds) that face the path, in
// groups: on top of blocks under the path, under blocks over it, one side spike on a wall facing
// the player. Only where they keep off every fair run, and only near the path.
void Structurer::edgeSpikes() {
    const double near = 75.0;
    const double base = std::clamp(0.35 + 0.55 * double(m_density), 0.2, 0.95);
    auto solid = [&](int cx, int cy) { return m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy); };
    std::vector<std::pair<int, int>> cells;
    for (auto& [k, kind] : m_res.cells.cells()) if (kind == CellKind::Solid) cells.push_back({int(int32_t(uint32_t(k >> 32))), int(int32_t(uint32_t(k)))});
    for (auto& [k, kind] : m_platforms.cells()) cells.push_back({int(int32_t(uint32_t(k >> 32))), int(int32_t(uint32_t(k)))});
    std::sort(cells.begin(), cells.end(), [](auto a, auto b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    // horizontal faces: runs of free cells on top of / under the blocks (row by row, left to right)
    for (int side : {1, -1}) {
        std::vector<std::pair<int, int>> run;
        auto flush = [&]() { spikeRun(run, side > 0 ? 0 : 180, base, near); run.clear(); };
        for (auto [cx, cy] : cells) {
            const int y = cy + side;
            if (y < 0 || solid(cx, y)) continue;
            if (!run.empty() && (run.back().second != y || run.back().first != cx - 1)) flush();
            run.push_back({cx, y});
        }
        flush();
    }
    // walls facing the player: one spike per wall, at the height the player passes
    std::sort(cells.begin(), cells.end());
    for (size_t i = 0; i < cells.size();) {
        const int cx = cells[i].first;
        size_t j = i;
        while (j + 1 < cells.size() && cells[j + 1].first == cx && cells[j + 1].second == cells[j].second + 1) ++j;
        if (!at(cx * 64 + cells[i].second, 0x51DE).chance(base * 0.45)) { i = j + 1; continue; }
        float lo = 0, hi = 0;
        const float pathY = m_env.range(cx - 1, lo, hi) ? 0.5f * (lo + hi) : -1e9f;
        int best = -1;
        float bd = 1e9f;
        for (size_t k = i; k <= j; ++k) {
            const int cy = cells[k].second;
            if (solid(cx - 1, cy)) continue;
            const float d = std::fabs((cy + 0.5f) * kCell - pathY);
            if (d < bd) { bd = d; best = cy; }
        }
        if (best >= 0) spikeAt(cx - 1, best, 270, near);
        i = j + 1;
    }
    // the floor (and the floor/ceiling of flying stretches) where the player never runs
    std::vector<std::pair<int, int>> floorRun, ceilRun;
    int lastCx = -1;
    for (const Sample& q : m_nominal) {
        const int cx = int(std::floor(q.x / kCell));
        if (cx == lastCx) continue;
        lastCx = cx;
        const int floorRow = q.bounded ? int(std::lround(q.floorY / kCell)) : 0;
        if (!floorRun.empty() && (floorRun.back().second != floorRow || floorRun.back().first != cx - 1)) { spikeRun(floorRun, 0, base, near); floorRun.clear(); }
        if (!solid(cx, floorRow)) floorRun.push_back({cx, floorRow});
        if (q.bounded) {
            const int ceilRow = int(std::lround(q.ceilY / kCell)) - 1;
            if (!ceilRun.empty() && (ceilRun.back().second != ceilRow || ceilRun.back().first != cx - 1)) { spikeRun(ceilRun, 180, base, near); ceilRun.clear(); }
            if (!solid(cx, ceilRow)) ceilRun.push_back({cx, ceilRow});
        }
    }
    spikeRun(floorRun, 0, base, near);
    spikeRun(ceilRun, 180, base, near);
}

void Structurer::corridors() {
    // wave/ship/swing stretches between the bounds: floor and ceiling following the path at the
    // reserved distance, with 45-degree slopes where the boundary moves by one row
    const int n = int(m_nominal.size());
    int i = 0;
    while (i < n) {
        const Sample& q = m_nominal[size_t(i)];
        if (!(q.mode == Mode::Wave || q.mode == Mode::Ship || q.mode == Mode::Swing) || !q.bounded) { ++i; continue; }
        int j = i;
        while (j < n && m_nominal[size_t(j)].mode == q.mode && m_nominal[size_t(j)].bounded) ++j;
        if (q.mode != Mode::Wave && !at(int(std::floor(q.x / kCell)), 0xC0DD).chance(0.6)) { i = j; continue; }
        const int c0 = int(std::floor(m_nominal[size_t(i)].x / kCell)) + 2;
        const int c1 = int(std::floor(m_nominal[size_t(j - 1)].x / kCell)) - 2;
        const int floorRow = int(std::lround(q.floorY / kCell)), ceilRow = int(std::lround(q.ceilY / kCell)) - 1;
        const bool wide = q.mode != Mode::Wave; // wave walls are sharp 45-degree triangles
        std::vector<int> top, bottom; // per column: solid floor up to top (exclusive), ceiling from bottom
        for (int c = c0; c <= c1; ++c) {
            // exactly the cells that may be built (the same test the placement uses)
            // (cells already built, e.g. a pillar, count as part of the wall)
            int t = floorRow;
            while (t <= ceilRow && (m_res.cells.filled(c, t) || cellFree(c, t))) ++t;
            int b = ceilRow + 1;
            while (b - 1 >= t && (m_res.cells.filled(c, b - 1) || cellFree(c, b - 1))) --b;
            if (t > ceilRow) { t = floorRow; b = ceilRow + 1; } // the whole column is free: no path here
            top.push_back(t);
            bottom.push_back(b);
        }
        // smooth: the walls move by at most one row per column (lowering a floor or raising a
        // ceiling only makes more room), so every change becomes a 45-degree slope
        const size_t m = top.size();
        for (size_t k = 1; k < m; ++k) top[k] = std::min(top[k], top[k - 1] + 1);
        for (size_t k = m; k-- > 1;) top[k - 1] = std::min(top[k - 1], top[k] + 1);
        for (size_t k = 1; k < m; ++k) bottom[k] = std::max(bottom[k], bottom[k - 1] - 1);
        for (size_t k = m; k-- > 1;) bottom[k - 1] = std::max(bottom[k - 1], bottom[k] - 1);
        // one-column bumps go (lowering a floor / raising a ceiling only makes room)
        for (size_t k = 1; k + 1 < m && wide; ++k) {
            if (top[k] > top[k - 1] && top[k] > top[k + 1]) top[k] = std::max(top[k - 1], top[k + 1]);
            if (bottom[k] < bottom[k - 1] && bottom[k] < bottom[k + 1]) bottom[k] = std::min(bottom[k - 1], bottom[k + 1]);
        }
        // the wall cells: 45-degree slopes where the wall moves a row per column, 2-wide slopes
        // where it moves a row over two columns (each only removes material from the wall)
        std::vector<CellKind> fk(m, CellKind::Solid), ck(m, CellKind::Solid);
        for (size_t k = 0; k < m; ++k) {
            const int dIn = k > 0 ? top[k] - top[k - 1] : 0, dOut = k + 1 < m ? top[k + 1] - top[k] : 0;
            if (fk[k] != CellKind::Solid) continue;
            if (wide && dIn == 1 && k + 1 < m && dOut == 0) { fk[k] = CellKind::WideFloorUpL; fk[k + 1] = CellKind::WideFloorUpR; continue; }
            if (wide && dOut == -1 && dIn == 0 && k > 0 && fk[k - 1] == CellKind::Solid) { fk[k - 1] = CellKind::WideFloorDownL; fk[k] = CellKind::WideFloorDownR; continue; }
            if (dIn == 1 && dOut != -1) fk[k] = CellKind::SlopeFloorUp;
            else if (dOut == -1 && dIn != 1) fk[k] = CellKind::SlopeFloorDown;
        }
        for (size_t k = 0; k < m; ++k) {
            const int dIn = k > 0 ? bottom[k] - bottom[k - 1] : 0, dOut = k + 1 < m ? bottom[k + 1] - bottom[k] : 0;
            if (ck[k] != CellKind::Solid) continue;
            if (wide && dIn == -1 && k + 1 < m && dOut == 0) { ck[k] = CellKind::WideCeilDownL; ck[k + 1] = CellKind::WideCeilDownR; continue; }
            if (wide && dOut == 1 && dIn == 0 && k > 0 && ck[k - 1] == CellKind::Solid) { ck[k - 1] = CellKind::WideCeilUpL; ck[k] = CellKind::WideCeilUpR; continue; }
            if (dIn == -1 && dOut != 1) ck[k] = CellKind::SlopeCeilDown;
            else if (dOut == 1 && dIn != -1) ck[k] = CellKind::SlopeCeilUp;
        }
        for (int c = c0; c <= c1; ++c) {
            const size_t k = size_t(c - c0);
            const int t = top[k], b = bottom[k];
            for (int y = floorRow; y < t; ++y) {
                const CellKind kind = y == t - 1 ? fk[k] : CellKind::Solid;
                if (cellFree(c, y)) { addCell(c, y, kind); ++m_res.corridorCells; }
            }
            for (int y = b; y <= ceilRow; ++y) {
                const CellKind kind = y == b ? ck[k] : CellKind::Solid;
                if (cellFree(c, y)) { addCell(c, y, kind); ++m_res.corridorCells; }
            }
        }
        i = j;
    }
}

void Structurer::measureWindows() {
    for (size_t i = 0; i < m_path.clicks.size() && i < m_res.checks.size(); ++i) {
        const PathClick& c = m_path.clicks[i];
        const int horizon = std::min(m_path.endTick, nextPressAfter(c.tick) + 15);
        int prevRelease = 0;
        for (auto& e : m_path.inputs.events) if (!e.press && e.tick < c.tick) prevRelease = e.tick;
        auto ok = [&](int s) {
            if (c.tick + s <= prevRelease + 1 || c.tick + s >= nextPressAfter(c.tick) - 1) return false;
            return runFrom(m_pre[i], shifted(c.tick, s, false), horizon, nullptr);
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
    m_margin = std::clamp(prof.clearance * 0.3f + 2.0f, 2.0f, 16.0f);
    m_seed = m_rng.next();
    m_density = m_prm.density >= 0 ? m_prm.density : (m_plan.style == Style::Minimal ? 0.3f : m_plan.style == Style::Chaotic ? 0.85f : 0.65f);
    // the path as planned: its objects and platform pieces (same objects, same order)
    m_base = pathLevel(m_path);
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
    if (m_prm.necessity) necessity();
    else for (auto& c : m_path.clicks) m_res.checks.push_back({c.click, false, 0, 0});
    corridors();
    walls();
    frame();
    clusters();
    edgeSpikes();
    // ---- the finished layout: path objects, platform pieces (as planned), structures, hazards
    level::Level& L = m_res.level;
    L = m_base;
    tileCells(m_res.cells, m_prm.look, L.objects);
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
