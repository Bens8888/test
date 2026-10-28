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
                if (hazardFree(sw)) { addHazard(sw); ++m_res.saws; placed = true; }
            }
        }
        if (placed) lastX = cx;
    }
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
        for (size_t k = 1; k + 1 < m; ++k) {
            if (top[k] > top[k - 1] && top[k] > top[k + 1]) top[k] = std::max(top[k - 1], top[k + 1]);
            if (bottom[k] < bottom[k - 1] && bottom[k] < bottom[k + 1]) bottom[k] = std::min(bottom[k - 1], bottom[k + 1]);
        }
        // the wall cells: 45-degree slopes where the wall moves a row per column, 2-wide slopes
        // where it moves a row over two columns (each only removes material from the wall)
        std::vector<CellKind> fk(m, CellKind::Solid), ck(m, CellKind::Solid);
        for (size_t k = 0; k < m; ++k) {
            const int dIn = k > 0 ? top[k] - top[k - 1] : 0, dOut = k + 1 < m ? top[k + 1] - top[k] : 0;
            if (fk[k] != CellKind::Solid) continue;
            if (dIn == 1 && k + 1 < m && dOut == 0) { fk[k] = CellKind::WideFloorUpL; fk[k + 1] = CellKind::WideFloorUpR; continue; }
            if (dOut == -1 && dIn == 0 && k > 0 && fk[k - 1] == CellKind::Solid) { fk[k - 1] = CellKind::WideFloorDownL; fk[k] = CellKind::WideFloorDownR; continue; }
            if (dIn == 1 && dOut != -1) fk[k] = CellKind::SlopeFloorUp;
            else if (dOut == -1 && dIn != 1) fk[k] = CellKind::SlopeFloorDown;
        }
        for (size_t k = 0; k < m; ++k) {
            const int dIn = k > 0 ? bottom[k] - bottom[k - 1] : 0, dOut = k + 1 < m ? bottom[k + 1] - bottom[k] : 0;
            if (ck[k] != CellKind::Solid) continue;
            if (dIn == -1 && k + 1 < m && dOut == 0) { ck[k] = CellKind::WideCeilDownL; ck[k + 1] = CellKind::WideCeilDownR; continue; }
            if (dOut == 1 && dIn == 0 && k > 0 && ck[k - 1] == CellKind::Solid) { ck[k - 1] = CellKind::WideCeilUpL; ck[k] = CellKind::WideCeilUpR; continue; }
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
    frame();
    // ---- the finished layout: path objects, platform pieces (as planned), structures, hazards
    level::Level& L = m_res.level;
    L = m_base;
    tileCells(m_res.cells, m_prm.look, L.objects);
    for (auto& h : m_hazards) L.objects.push_back(h);
    LevelObject end = make(oid::TrigEnd, std::round((m_nominal.empty() ? 600.0f : m_nominal.back().x) / 30.0f) * 30.0f + 15, 600);
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
