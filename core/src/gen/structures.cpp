// Generator v2: structures around the gameplay path (see structures.hpp).
#include <al/gen/structures.hpp>

#include <al/bot/bot.hpp>
#include <al/gen/modules.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace al::gen {

namespace oid = level::id;
using level::LevelObject;

namespace {

constexpr float kCell = 30.0f;
constexpr float kModuleGap = 12.0f; // free space between structures, so they stay separate shapes
constexpr float kSolidMargin = 5.0f; // blocks added close to the path keep at least this off it (a player grazing a block lands on it or bumps its head)

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

// What a hazard looks like, not its hitbox: a spike fills its cell, a half spike the lower half of
// it, a saw a little more than its hitbox. Two of these may touch but not overlap.
bool drawnBox(const LevelObject& o, Box& out) {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return false;
    if (so.kind == level::Kind::Saw) {
        const float r = so.hw * 1.1f;
        out = {so.x - r, so.y - r, so.x + r, so.y + r};
        return true;
    }
    if (so.kind != level::Kind::Hazard) return false;
    float w = 2 * so.hw + 6, h = 2 * so.hh + 6;
    switch (o.id) {
        case 8: case 144: case 177: case 216: w = h = 30; break;
        case 39: case 205: case 217: w = 30; h = 14; break;
        case 103: case 145: case 218: w = h = 18; break;
        case 392: w = 12; h = 10; break;
        case 458: case 459: w = 30; h = 10; break; // a row of tiny spikes
        default: break;
    }
    if (std::fmod(std::fabs(o.rotation), 180.0) == 90) std::swap(w, h);
    out = {float(o.x) - w / 2, float(o.y) - h / 2, float(o.x) + w / 2, float(o.y) + h / 2};
    return true;
}
Box shrunk(const Box& b, float m) { return {b.x0 + m, b.y0 + m, b.x1 - m, b.y1 - m}; }

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
    // spikes in rows: on the ground of the easier ratings at most m_maxRun side by side
    std::set<std::pair<long, long>> m_spikeAt; // (column, height in half cells)
    int m_maxRun = 99;
    bool m_groundStretch = false;
    static bool rowSpike(const LevelObject& o) {
        return o.info().kind == level::Kind::Hazard && std::fmod(std::fabs(o.rotation), 180.0) == 0;
    }
    static std::pair<long, long> spikeKey(const LevelObject& o) { return {long(std::floor(o.x / 30.0)), std::lround(o.y / 15.0)}; }
    void noteSpike(const LevelObject& o) { if (rowSpike(o)) m_spikeAt.insert(spikeKey(o)); }
    // the longest row the spikes in `add` would be part of
    int spikeRun(const std::vector<LevelObject>& add) const {
        std::set<std::pair<long, long>> s;
        for (auto& o : add) if (rowSpike(o)) s.insert(spikeKey(o));
        int best = 0;
        for (auto [x, y] : s) {
            auto has = [&](long xx) { return s.count({xx, y}) || m_spikeAt.count({xx, y}); };
            int n = 1;
            for (long d = 1; has(x - d); ++d) ++n;
            for (long d = 1; has(x + d); ++d) ++n;
            best = std::max(best, n);
        }
        return best;
    }
    // structures stay between the start and the end of the level (nothing behind the player at the
    // start or after the end trigger)
    float m_xMin = 300, m_xMax = 1e9f;
    // between the start and the end, and no more than a screen's third above the path there
    // (a shape far up in the sky is off the screen)
    bool inLevel(const Box& b) const {
        if (b.x0 < m_xMin || b.x1 > m_xMax) return false;
        if (m_pathTop.empty()) return true;
        const int c0 = std::clamp(int(std::floor(b.x0 / kCell)), 0, int(m_pathTop.size()) - 1);
        const int c1 = std::clamp(int(std::floor(b.x1 / kCell)), 0, int(m_pathTop.size()) - 1);
        float top = 0;
        for (int c = c0; c <= c1; ++c) top = std::max(top, m_pathTop[size_t(c)]);
        return b.y0 <= top + 240.0f;
    }
    std::vector<float> m_pathTop;     // per column: the highest the path gets within a screen of it
    Envelope m_drawn;                 // every hazard as it is drawn
    float m_margin = 4;               // extra distance kept from the reserved space
    float m_hazMargin = 4;            // the same for hazards (smaller at demon ratings)
    float m_faceMargin = 5;           // and on top of it for the spikes lining faces
    float m_waveMargin = 30;          // from the wave's path to its walls
    int m_halfWin = 12;               // ticks each press may move either way
    float m_density = 0.5f;
    int m_style = 0;                  // structure style (modules.hpp)
    Envelope m_built;                 // everything placed so far
    std::vector<std::pair<float, float>> m_waveSpans; // x ranges of the wave parts
    std::vector<LevelObject> m_objects; // the modules' pieces
    std::map<int, float> m_reachOver, m_reachUnder; // per column, how far the modules on that side reach
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
    void noteHazard(const LevelObject& o);
    bool hazardRoom(const LevelObject& o) const;
    void necessity();
    void tighten();
    void threadPath();
    // the designed run still lives from before the click nearest `tick` until `until`
    bool designedLives(int tick, int until) const;
    bool killOnFace(const std::vector<Sample>& run, size_t i, const std::vector<bot::InputEvent>& ev, int horizon, float margin);
    bool tryKill(size_t clickIndex, const std::vector<Sample>& skip, const std::vector<bot::InputEvent>& ev, int horizon);
    void placeModules(int firstLane, int lastLane);
    struct PillarEnd { LevelObject piece; float dir, target; int module; };
    int m_moduleCount = 0;
    std::vector<PillarEnd> m_pillarEnds;
    void extendPillars();
    void mergePlatforms(std::vector<LevelObject>& objs) const;
    static bool isCellPiece(const LevelObject& o);
    void putPiece(const LevelObject& o);
    void bands();
    void joinShapes();
    // a Classic ground stretch without a ceiling: real cube parts have nothing above the player
    bool openSky(const Sample& s) const { return m_style == 4 && noCeiling(s); }
    // a ground stretch without bounds: shapes over the player would hang from nothing, so in every
    // style only a band (one connected ceiling) goes there
    static bool noCeiling(const Sample& s) { return !s.bounded && !isFlyingMode(s.mode) && s.mode != Mode::Wave; }
    bool bandCell(int cx, int cy);
    void lineFace(float x, float faceY, int toward, int kind);
    void lineWall(float wallX, float y, int side);
    bool hazardFits(const LevelObject& o);
    void faceSaw(float x, float y);
    bool placeModule(const Module& m, bool mirrorX, bool under, int cx, int row, int floorRow, int ceilRow);
    bool objectFits(const LevelObject& o, Box* out) const;
    void placeObject(const LevelObject& o);
    void waveWalls();
    void wallPiece(float x, float y, bool up, bool solidBelow, bool steep, bool solid, float scale = 1.0f);
    bool nearPathObject(float x, float y) const;
    bool pieceBlocksPath(float x, float y, float slope, bool solidBelow, float halfW = 15.0f) const;
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
    // a teleport reserves everything it passes: a hazard on the way kills
    auto reserve = [this](const std::vector<Sample>& run) {
        for (size_t k = 0; k < run.size(); ++k) {
            const Sample& q = run[k];
            m_env.add(q.x, q.y, q.half);
            if (k > 0 && std::fabs(q.y - run[k - 1].y) > 40) {
                const float lo = std::min(q.y, run[k - 1].y), hi = std::max(q.y, run[k - 1].y);
                m_env.addBox({q.x - q.half, lo - q.half, q.x + q.half, hi + q.half});
            }
        }
    };
    // the designed run
    reserve(m_nominal);
    // every click pressed early and late (within the window); a variant only counts while it does
    // what the click is meant to do (the orb is taken, the jump happens)
    const uint32_t kClickEvents = phys::EvJump | phys::EvOrb | phys::EvUfoJump | phys::EvSwingFlip | phys::EvTeleport;
    for (size_t i = 0; i < m_path.clicks.size(); ++i) {
        const PathClick& c = m_path.clicks[i];
        const int T = c.tick;
        uint32_t want = 0;
        for (int t = T; t <= T + 3 && t - 1 < int(m_nominal.size()); ++t) want |= m_nominal[size_t(t - 1)].events & kClickEvents;
        // a moved press is followed up to the next click (below demon ratings a little past it): what
        // the run does after a click it never made is not the player's room
        const int horizon = std::min(m_path.endTick, nextPressAfter(T) + (m_plan.baseDifficulty >= 5.0f ? 2 : 15));
        int lo, hi;
        moveRange(i, lo, hi);
        // a wave corridor is as tight as the walls in hard demon wave parts: half the window of a jump
        const int win = c.mode == Mode::Wave && m_plan.baseDifficulty >= 6 ? std::max(3, m_halfWin / 2) : m_halfWin;
        for (int s = -win; s <= win; s += (std::abs(s) <= 2 ? 1 : 2)) {
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
            reserve(tr);
        }
    }
}

bool Structurer::cellFree(int cx, int cy) const {
    if (cy < 0 || !inLevel(cellBox(cx, cy))) return false;
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
    if (!inLevel(b)) return false;
    for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
    if (m_env.hits(b, m_hazMargin + 1) || m_built.hits(b, 2) || !hazardRoom(o)) return false;
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
    noteSpike(o);
    m_W.addObject(o, -1);
    m_hazardCells.insert({int(std::floor(o.x / kCell)), int(std::floor(o.y / kCell))});
    noteHazard(o);
}

void Structurer::noteHazard(const LevelObject& o) {
    Box d{};
    if (drawnBox(o, d)) m_drawn.addBox(d);
}

// Hazards stand side by side, never in a pile, and on a face, not sunk into the blocks (a saw may
// bite into a wall, its hub stays in the open).
bool Structurer::hazardRoom(const LevelObject& o) const {
    Box d{};
    if (!drawnBox(o, d)) return true;
    if (m_drawn.hits(d, -1.5f)) return false;
    const bool saw = o.info().kind == level::Kind::Saw;
    const Box in = shrunk(d, saw ? (d.x1 - d.x0) * 0.5f - 6.f : 3.f);
    if (in.x1 <= in.x0 || in.y1 <= in.y0) return true;
    if (m_built.hits(in, 0)) return false;
    for (int cx = int(std::floor(in.x0 / kCell)); cx <= int(std::floor(in.x1 / kCell)); ++cx)
        for (int cy = int(std::floor(in.y0 / kCell)); cy <= int(std::floor(in.y1 / kCell)); ++cy)
            if (m_platforms.filled(cx, cy) || m_res.cells.filled(cx, cy)) return false;
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

// A spike on a face the player passes (the ground, a bound, a platform or a structure's cells),
// pointing at the path, that kills the run `run` (a press moved out of the window) but keeps
// `margin` off every run inside the window. Nothing new floats in the air: the spike sits on
// something that is already there, or on a pillar (1-6 cells) grown from it toward the path.
bool Structurer::killOnFace(const std::vector<Sample>& run, size_t i, const std::vector<bot::InputEvent>& ev, int horizon, float margin) {
    auto filled = [&](int cx, int cy) { return m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy); };
    auto dies = [&]() {
        int dt = -1;
        runFrom(m_pre[i], ev, horizon, nullptr, &dt);
        return dt >= 0;
    };
    auto fits = [&](const LevelObject& o) {
        phys::SimObject so;
        if (!phys::World::makeSimObject(o, 0, so)) return false;
        const Box b{so.x - so.hw, so.y - so.hh, so.x + so.hw, so.y + so.hh};
        if (!inLevel(b)) return false;
        for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
        if (m_env.hits(b, margin) || !hazardRoom(o)) return false;
        for (auto& po : m_path.objects)
            if (std::fabs(po.o.x - o.x) < 36 && std::fabs(po.o.y - o.y) < 36) return false;
        return true;
    };
    // a cell a grown pillar may take
    auto open = [&](int cx, int cy) {
        if (cy < 0 || filled(cx, cy)) return false;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (m_platforms.filled(cx + dx, cy + dy)) return false;
        const Box b = cellBox(cx, cy);
        if (!inLevel(b) || m_env.hits(b, std::max(margin, kSolidMargin)) || m_built.hits(shrunk(b, 3.f), 0) || m_drawn.hits(b, -1.f)) return false;
        for (auto& po : m_path.objects)
            if (std::fabs(float(po.o.x) - (b.x0 + 15)) < 45 && std::fabs(float(po.o.y) - (b.y0 + 15)) < 45) return false;
        return true;
    };
    auto meets = [&](const LevelObject& o, size_t j) {
        phys::SimObject so;
        phys::World::makeSimObject(o, 0, so);
        for (size_t k = j >= 30 ? j - 30 : 0; k < run.size() && k < j + 60; ++k)
            if (std::fabs(run[k].x - so.x) < so.hw + run[k].half + 1 && std::fabs(run[k].y - so.y) < so.hh + run[k].half + 1) return true;
        return false;
    };
    // tried in the world alone; kept (and noted everywhere) only when it catches the press
    auto attempt = [&](const LevelObject& spike, const std::vector<std::pair<int, int>>& cells) {
        const size_t mk = mark();
        for (auto [cx, cy] : cells) m_W.addObject(make(oid::Block, cx * kCell + 15, cy * kCell + 15), -1);
        m_W.addObject(spike, -1);
        const bool caught = dies() && designedLives(m_path.clicks[i].tick, horizon + 60);
        m_W.truncate(mk);
        if (!caught) return false;
        for (auto [cx, cy] : cells) {
            addCell(cx, cy);
            m_built.addBox(cellBox(cx, cy));
        }
        addHazard(spike);
        return true;
    };
    for (size_t j = 0; j < run.size() && j < 180; j += 3) {
        const Sample& q = run[j];
        if (q.mode == Mode::Wave) continue;
        const int qx = int(std::floor(q.x / kCell));
        // the faces over and under the player in its column: a filled cell, the floor, a bound
        for (int dir : {-1, 1}) {
            float faceY = NAN;
            const int row = int(std::floor(q.y / kCell));
            for (int k = 0; k <= 6 && std::isnan(faceY); ++k) {
                const int r = row + dir * k;
                if (filled(qx, r)) faceY = float(dir > 0 ? r : r + 1) * kCell;
            }
            if (std::isnan(faceY)) {
                if (dir < 0) faceY = q.bounded ? q.floorY : 0.f;
                else if (q.bounded) faceY = q.ceilY;
                else continue;
            }
            if (std::fabs(faceY - q.y) > 6 * kCell) continue;
            const bool ground = faceY <= 0.5f || (q.bounded && (faceY == q.floorY || faceY == q.ceilY));
            for (float dx : {0.f, 7.5f, -7.5f, 15.f}) {
                const float x = std::round((q.x + dx) / 7.5f) * 7.5f;
                // a spike stands on the face along its whole width
                if (!ground) {
                    const int r = dir > 0 ? int(std::lround(faceY / kCell)) : int(std::lround(faceY / kCell)) - 1;
                    if (!filled(int(std::floor((x - 12) / kCell)), r) || !filled(int(std::floor((x + 12) / kCell)), r)) continue;
                }
                for (int id : {oid::Spike, oid::HalfSpike}) {
                    const float h = id == oid::Spike ? 15.f : 7.5f;
                    LevelObject o = make(id, x, faceY - float(dir) * h, dir > 0 ? 180 : 0);
                    if (fits(o) && meets(o, j) && attempt(o, {})) return true;
                }
            }
            // a pillar from the face toward the path, the spike on its tip (on the grid, in the
            // player's column and the next one)
            const int faceRow = int(std::lround(faceY / kCell));
            for (int cx : {qx, qx + 1}) {
                std::vector<std::pair<int, int>> cells;
                for (int ext = 1; ext <= 6; ++ext) {
                    const int cy = dir > 0 ? faceRow - ext : faceRow + ext - 1;
                    if (!open(cx, cy)) break;
                    // the pillar stands on the face: the first cell needs something under it
                    if (ext == 1 && !ground && !filled(cx, dir > 0 ? faceRow : faceRow - 1)) break;
                    cells.push_back({cx, cy});
                    const float tipY = float(dir > 0 ? cy : cy + 1) * kCell;
                    LevelObject o = make(oid::Spike, cx * kCell + 15, tipY - float(dir) * 15.f, dir > 0 ? 180 : 0);
                    if (!fits(o) || !meets(o, j)) continue;
                    if (attempt(o, cells)) return true;
                }
            }
        }
    }
    return false;
}

// Demons thread the player through hazards: along the whole path (not only where a press would
// go wrong) a spike sits as close as the timing window allows, on the ground, a face over or under
// the player, or the tip of a pillar (up to 6 cells) grown from one. Real Extreme demons keep a hazard about
// half a block from the player most of the time (Bloodbath, from a run logged in GD).
void Structurer::threadPath() {
    const float D = m_plan.baseDifficulty;
    if (D < 2.0f || m_nominal.empty()) return;
    const float margin = std::clamp(m_margin + 1.0f - (D - 5.0f) * 1.5f, 1.5f, m_margin + 1.0f);
    // below demon ratings a few, at their own distance (real Hard to Insane levels have about two
    // thirds as many hazards per second as Hard demons)
    const double rate = D < 5.0f ? 0.1 * double(D - 1.0f) : std::clamp(0.3 + 0.16 * double(D - 5.0f), 0.3, 0.95);
    auto filled = [&](int cx, int cy) { return m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy); };
    auto fits = [&](const LevelObject& o) {
        phys::SimObject so;
        if (!phys::World::makeSimObject(o, 0, so)) return false;
        const Box b{so.x - so.hw, so.y - so.hh, so.x + so.hw, so.y + so.hh};
        if (!inLevel(b)) return false;
        for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
        if (m_env.hits(b, margin) || !hazardRoom(o)) return false;
        for (auto& po : m_path.objects)
            if (std::fabs(po.o.x - o.x) < 36 && std::fabs(po.o.y - o.y) < 36) return false;
        return true;
    };
    auto open = [&](int cx, int cy) {
        if (cy < 0 || filled(cx, cy)) return false;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (m_platforms.filled(cx + dx, cy + dy)) return false;
        const Box b = cellBox(cx, cy);
        if (!inLevel(b) || m_env.hits(b, std::max(margin, kSolidMargin)) || m_built.hits(shrunk(b, 3.f), 0) || m_drawn.hits(b, -1.f)) return false;
        for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
        for (auto& po : m_path.objects)
            if (std::fabs(float(po.o.x) - (b.x0 + 15)) < 45 && std::fabs(float(po.o.y) - (b.y0 + 15)) < 45) return false;
        return true;
    };
    int lastCol = INT_MIN;
    for (const Sample& q : m_nominal) {
        const int cx = int(std::floor(q.x / kCell));
        if (cx == lastCol || q.mode == Mode::Wave) continue;
        lastCol = cx;
        for (int dir : {-1, 1}) {
            Rng r = at(cx, dir > 0 ? 0x7A1 : 0x7A2);
            if (!r.chance(rate)) continue;
            // the face on this side in the column
            float faceY = NAN;
            const int row = int(std::floor(q.y / kCell));
            for (int k = 0; k <= 6 && std::isnan(faceY); ++k)
                if (filled(cx, row + dir * k)) faceY = float(dir > 0 ? row + dir * k : row + dir * k + 1) * kCell;
            if (std::isnan(faceY)) {
                if (dir < 0) faceY = q.bounded ? q.floorY : 0.f;
                else if (q.bounded) faceY = q.ceilY;
                else continue;
            }
            const bool ground = faceY <= 0.5f || (q.bounded && (faceY == q.floorY || faceY == q.ceilY));
            const int faceRow = int(std::lround(faceY / kCell));
            if (!ground && !filled(cx, dir > 0 ? faceRow : faceRow - 1)) continue;
            // the closest that fits: the longest pillar, else the spike on the face itself
            std::vector<std::pair<int, int>> cells, best;
            bool found = false;
            LevelObject pick;
            auto spikeAt = [&](float tipY) { return make(oid::Spike, cx * kCell + 15, tipY - float(dir) * 15.f, dir > 0 ? 180 : 0); };
            if (LevelObject o = spikeAt(faceY); fits(o)) { pick = o; found = true; }
            for (int ext = 1; ext <= 6; ++ext) {
                const int cy = dir > 0 ? faceRow - ext : faceRow + ext - 1;
                if (!open(cx, cy)) break;
                cells.push_back({cx, cy});
                LevelObject o = spikeAt(float(dir > 0 ? cy : cy + 1) * kCell);
                if (fits(o)) { pick = o; best = cells; found = true; }
            }
            if (!found) continue;
            if (m_groundStretch && spikeRun({pick}) > m_maxRun) continue;
            {
                const size_t mk = mark();
                for (auto [x, y] : best) m_W.addObject(make(oid::Block, x * kCell + 15, y * kCell + 15), -1);
                m_W.addObject(pick, -1);
                const int tick = int(&q - m_nominal.data()) + 1;
                const bool ok = designedLives(tick, tick + 240);
                m_W.truncate(mk);
                if (!ok) continue;
            }
            for (auto [x, y] : best) {
                addCell(x, y);
                m_built.addBox(cellBox(x, y));
            }
            addHazard(pick);
            ++m_res.hazards;
        }
    }
}

bool Structurer::designedLives(int tick, int until) const {
    size_t k = 0;
    while (k + 1 < m_pre.size() && m_pre[k + 1].tick <= tick - 30) ++k;
    if (m_pre.empty()) return true;
    // alive is not enough: a block that bends the path (an earlier landing) sends the rest of the
    // run where nothing was checked, so it must be back on the designed path by the end
    const int end = std::min(until, m_path.endTick);
    std::vector<Sample> tr;
    if (!runFrom(m_pre[k], m_path.inputs.events, end, &tr)) return false;
    if (tr.empty() || m_nominal.empty()) return true;
    const Sample& want = m_nominal[size_t(std::clamp(m_pre[k].tick + int(tr.size()) - 1, 0, int(m_nominal.size()) - 1))];
    return std::fabs(tr.back().x - want.x) < 2.0f && std::fabs(tr.back().y - want.y) < 2.0f;
}

// Above Insane the windows are the rating's: a click the structures leave loose (a press well
// before or after the beat still survives) gets a spike on a nearby face that catches that press.
void Structurer::tighten() {
    const float D = m_plan.baseDifficulty;
    if (D < 5.0f) return;
    // demons trust the simulator closer to the path (Extreme: a unit and a half)
    const float margin = std::clamp(m_margin + 1.0f - (D - 5.0f) * 1.5f, 1.5f, m_margin + 1.0f);
    for (size_t i = 0; i < m_path.clicks.size(); ++i) {
        const PathClick& c = m_path.clicks[i];
        if (c.mode == Mode::Wave || c.act == Act::Release) continue;
        int lo, hi;
        moveRange(i, lo, hi);
        const int horizon = std::min(m_path.endTick, nextPressAfter(c.tick) + 15);
        for (int side : {-1, 1}) {
            // early presses can be simulated back to the state kept before the click
            const int far = 3 * m_halfWin;
            for (int s = m_halfWin + 1; s <= far; s += 3) {
                const int t = c.tick + side * s;
                if (t <= lo + 1 || t >= hi - 1) break;
                auto ev = shifted(i, side * s, false);
                std::vector<Sample> tr;
                int dt = -1;
                runFrom(m_pre[i], ev, horizon, &tr, &dt);
                if (dt >= 0) break; // already caught here
                if (killOnFace(tr, i, ev, horizon, margin)) { ++m_res.hazards; break; }
            }
        }
    }
}

bool Structurer::objectFits(const LevelObject& o, Box* out) const {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return true;
    const float hw = so.hw, hh = so.kind == level::Kind::Saw ? so.hw : so.hh;
    const Box b{so.x - hw, so.y - hh, so.x + hw, so.y + hh};
    if (out) *out = b;
    if (!inLevel(b)) return false;
    for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false; // wave parts hold only their walls
    const bool hazard = so.kind == level::Kind::Hazard || so.kind == level::Kind::Saw || so.kind == level::Kind::HazardSlope;
    if (m_env.hits(b, hazard ? m_hazMargin + 2.0f : m_margin)) return false;
    // a slope pulls a player passing just over it down onto it (and the run off its path): slopes
    // keep a block away
    if (so.kind == level::Kind::Slope && m_env.hits(b, m_margin + 30.0f)) return false;
    if (m_built.hits(b, kModuleGap)) return false;
    if (hazard ? !hazardRoom(o) : m_drawn.hits(b, -2.f)) return false;
    // nothing inside a path platform or the cells built so far (its outline would cross theirs)
    for (int cx = int(std::floor((b.x0 + 1) / kCell)); cx <= int(std::floor((b.x1 - 1) / kCell)); ++cx)
        for (int cy = int(std::floor((b.y0 + 1) / kCell)); cy <= int(std::floor((b.y1 - 1) / kCell)); ++cy)
            if ((m_platforms.filled(cx, cy) || m_res.cells.filled(cx, cy)) && overlap(shrunk(b, 1.f), cellBox(cx, cy), 0)) return false;
    for (auto& po : m_path.objects)
        if (std::fabs(po.o.x - so.x) < 40 + hw && std::fabs(po.o.y - so.y) < 40 + hh) return false;
    return true;
}

// A whole-cell block or outline piece on the grid: it joins the cells, so it is drawn in the
// layout's look and as one outline with whatever it touches (side by side, two shapes would each
// draw their own edge, or an outline layout would show filled black blocks).
bool Structurer::isCellPiece(const LevelObject& o) {
    const bool id = o.id == oid::Block || o.id == oid::OutlineSquare || o.id == oid::OutlineCorner || o.id == oid::OutlineTopPillar ||
                    o.id == oid::OutlinePillar;
    const double fx = o.x - std::floor(o.x / 30.0) * 30.0, fy = o.y - std::floor(o.y / 30.0) * 30.0;
    return id && std::fabs(fx - 15) < 0.5 && std::fabs(fy - 15) < 0.5;
}

void Structurer::putPiece(const LevelObject& o) {
    if (!isCellPiece(o)) { placeObject(o); return; }
    const int cx = int(std::floor(o.x / kCell)), cy = int(std::floor(o.y / kCell));
    addCell(cx, cy);
    m_built.addBox(cellBox(cx, cy));
}

void Structurer::placeObject(const LevelObject& o) {
    m_objects.push_back(o);
    noteSpike(o);
    m_W.addObject(o, -1);
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return;
    if (so.kind == level::Kind::Hazard || so.kind == level::Kind::Saw) noteHazard(o);
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
        // the thorn and wavy spikes some source levels use look bent: the plain spike has their hitbox
        int id = p.id;
        if (id == 216 || id == 177) id = oid::Spike;
        else if (id == 217 || id == 178) id = oid::HalfSpike;
        else if (id == 218 || id == 179) id = 103;
        LevelObject o = make(id, double(cx) * kCell + x, double(row) * kCell + y, rot);
        o.flipX = fx;
        o.flipY = fy;
        // some source levels pile hazards (a small spike on a spike): the first one stays
        Box d{};
        if (drawnBox(o, d)) {
            bool piled = false;
            for (auto& e : objs) {
                Box d2{};
                if (drawnBox(e, d2) && overlap(d, d2, -1.5f)) piled = true;
            }
            if (piled) continue;
        }
        Box b{};
        if (!objectFits(o, &b)) return false;
        const float lowest = float(floorRow) * kCell, highest = float(ceilRow + 1) * kCell;
        if (b.y0 < lowest - 0.5f || b.y1 > highest + 0.5f) {
            if (o.info().kind != level::Kind::Solid || std::min(b.x1 - b.x0, b.y1 - b.y0) > 4) return false;
            continue;
        }
        objs.push_back(o);
    }
    // a hazard of the source sunk into one of its own blocks stays out
    {
        std::vector<Box> solids;
        for (auto& o : objs) {
            phys::SimObject so;
            if (phys::World::makeSimObject(o, 0, so) && so.kind == level::Kind::Solid) solids.push_back({so.x - so.hw, so.y - so.hh, so.x + so.hw, so.y + so.hh});
        }
        std::vector<LevelObject> kept;
        for (auto& h : objs) {
            Box d{};
            bool sunk = false;
            if (drawnBox(h, d)) {
                const Box in = shrunk(d, h.info().kind == level::Kind::Saw ? (d.x1 - d.x0) * 0.25f : 3.f);
                for (auto& b : solids) sunk = sunk || overlap(in, b, 0);
            }
            if (!sunk) kept.push_back(h);
        }
        objs = std::move(kept);
    }
    // an edge line touching no other piece (its pillar was cut off at a bound) stands alone in the air
    {
        std::vector<Box> boxes(objs.size());
        std::vector<bool> solid(objs.size());
        for (size_t k = 0; k < objs.size(); ++k) {
            phys::SimObject so;
            solid[k] = phys::World::makeSimObject(objs[k], 0, so) && (so.kind == level::Kind::Solid || so.kind == level::Kind::Slope);
            if (solid[k]) boxes[k] = {so.x - so.hw, so.y - so.hh, so.x + so.hw, so.y + so.hh};
        }
        // groups of touching solid pieces; a group of only upright edge lines is a lone line
        std::vector<int> group(objs.size(), -1);
        int groups = 0;
        for (size_t k = 0; k < objs.size(); ++k) {
            if (!solid[k] || group[k] >= 0) continue;
            std::vector<size_t> todo{k};
            group[k] = groups;
            while (!todo.empty()) {
                const size_t a = todo.back();
                todo.pop_back();
                for (size_t m = 0; m < objs.size(); ++m)
                    if (solid[m] && group[m] < 0 && overlap(boxes[a], boxes[m], 1.5f)) { group[m] = groups; todo.push_back(m); }
            }
            ++groups;
        }
        std::vector<bool> lone(size_t(groups), true);
        for (size_t k = 0; k < objs.size(); ++k)
            if (group[k] >= 0 && !(objs[k].id == oid::OutlineTop && std::fmod(std::fabs(objs[k].rotation), 180.0) == 90)) lone[size_t(group[k])] = false;
        // so is a scrap of a few bare lines (edge lines, outline slopes) left from a shape cut short
        std::vector<int> pieces(size_t(groups), 0), thin(size_t(groups), 0);
        for (size_t k = 0; k < objs.size(); ++k) {
            if (group[k] < 0) continue;
            ++pieces[size_t(group[k])];
            if (objs[k].id == oid::OutlineTop || objs[k].id == 1338 || objs[k].id == 1339) ++thin[size_t(group[k])];
        }
        for (int g = 0; g < groups; ++g)
            if (pieces[size_t(g)] <= 3 && thin[size_t(g)] == pieces[size_t(g)]) lone[size_t(g)] = true;
        std::vector<LevelObject> kept;
        for (size_t k = 0; k < objs.size(); ++k)
            if (group[k] < 0 || !lone[size_t(group[k])]) kept.push_back(objs[k]);
        objs = std::move(kept);
    }
    if (objs.empty()) return false;
    if (m_groundStretch && spikeRun(objs) > m_maxRun) return false;
    // A pillar's far end was cut off from the source level, where it ran on into the ground or the
    // ceiling: its side lines go on later (extendPillars, once the bands are built).
    std::vector<PillarEnd> ends;
    {
        float lo = 1e9f, hi = -1e9f;
        std::vector<Box> boxes(objs.size());
        for (size_t k = 0; k < objs.size(); ++k) {
            objectFits(objs[k], &boxes[k]);
            if (objs[k].info().kind != level::Kind::Solid) continue;
            lo = std::min(lo, boxes[k].y0);
            hi = std::max(hi, boxes[k].y1);
        }
        const bool bounded = ceilRow < 90;
        const float target = under ? float(floorRow) * kCell : bounded ? float(ceilRow + 1) * kCell : hi + 4 * kCell;
        for (size_t k = 0; k < objs.size(); ++k) {
            const LevelObject& o = objs[k];
            const bool upright = std::fmod(std::fabs(o.rotation), 180.0) == 90;
            const bool side = (o.id == oid::OutlineTop && upright) || (o.id == oid::OutlinePillar && !upright);
            if (side && (under ? boxes[k].y0 <= lo + 1 : boxes[k].y1 >= hi - 1)) ends.push_back({o, under ? -1.f : 1.f, target, m_moduleCount});
        }
        ++m_moduleCount;
    }
    for (auto& o : objs) {
        Box b{};
        objectFits(o, &b);
        putPiece(o);
        for (int c = int(std::floor(b.x0 / kCell)); c <= int(std::floor((b.x1 - 0.01f) / kCell)); ++c) {
            auto& reach = under ? m_reachUnder : m_reachOver;
            auto it = reach.find(c);
            reach[c] = it == reach.end() ? (under ? b.y0 : b.y1) : under ? std::min(it->second, b.y0) : std::max(it->second, b.y1);
        }
    }
    for (auto& e : ends) m_pillarEnds.push_back(e);
    ++m_res.pillars;
    return true;
}

// The side lines of the pillars go on to the floor or the bound, so they do not end open in the
// air, and stop at the first face on the way (a band, another structure): a line through a block
// would show inside it. The last piece moves up to the face, overlapping the one before.
// A pillar that cannot reach either stops at the same height on both sides and is closed there.
void Structurer::extendPillars() {
    auto fits = [&](const LevelObject& e) {
        const Box b{float(e.x) - 2, float(e.y) - 15, float(e.x) + 2, float(e.y) + 15};
        if (!inLevel(b)) return false;
        for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
        if (m_env.hits(b, m_margin) || m_drawn.hits(b, -1)) return false;
        if (m_built.hits({b.x0, b.y0 + 2, b.x1, b.y1 - 2}, 0.5f)) return false;
        const int cy = int(std::floor(float(e.y) / kCell));
        for (int cx = int(std::floor(b.x0 / kCell)); cx <= int(std::floor(b.x1 / kCell)); ++cx)
            for (int y = cy - 1; y <= cy + 1; ++y) {
                const Box cb = cellBox(cx, y);
                if ((m_res.cells.filled(cx, y) || m_platforms.filled(cx, y)) && overlap({b.x0, b.y0 + 2, b.x1, b.y1 - 2}, cb, 0)) return false;
            }
        return true;
    };
    // whole cells each end can go before it is blocked, and whether that reaches its bound
    auto reach = [&](const PillarEnd& pe, bool& reaches) {
        const float sink = pe.dir < 0 && pe.target <= 0 ? 29.f : 0.5f;
        LevelObject e = pe.piece;
        for (int step = 0; step < 10; ++step) {
            e.y = pe.piece.y + double(pe.dir * kCell * float(step + 1));
            if (pe.dir < 0 ? e.y - 15 < pe.target - sink : e.y + 15 > pe.target + 0.5f) { reaches = true; return step; }
            if (!fits(e)) break;
        }
        reaches = false;
        return 0;
    };
    std::vector<PillarEnd> open;
    for (size_t a = 0; a < m_pillarEnds.size();) {
        size_t b = a;
        while (b < m_pillarEnds.size() && m_pillarEnds[b].module == m_pillarEnds[a].module) ++b;
        bool all = true;
        for (size_t k = a; k < b && all; ++k) reach(m_pillarEnds[k], all);        if (all) {
            for (size_t k = a; k < b; ++k) open.push_back(m_pillarEnds[k]);
        } else {
            // stop every side at the shortest whole reach and close the pillars there
            std::vector<PillarEnd> ends(m_pillarEnds.begin() + long(a), m_pillarEnds.begin() + long(b));
            std::sort(ends.begin(), ends.end(), [](const PillarEnd& p, const PillarEnd& q) { return p.piece.x < q.piece.x; });
            const float d = ends[0].dir;
            int steps = 10;
            for (auto& pe : ends) { bool r; steps = std::min(steps, reach(pe, r)); }
            for (auto& pe : ends)
                for (int s = 1; s <= steps; ++s) {
                    LevelObject e = pe.piece;
                    e.y = pe.piece.y + double(d * kCell * float(s));
                    putPiece(e);
                }
            const double endY = ends[0].piece.y + double(d * kCell * (float(steps) + 0.5f)) - double(d * 0.75f);
            auto cap = [&](double x0, double x1) {
                for (double x = x0 + 15; x < x1 + 14; x += kCell) {
                    LevelObject c = make(oid::OutlineTop, std::min(x, x1 - 15), endY, d < 0 ? 180 : 0);
                    m_objects.push_back(c);
                    m_W.addObject(c, -1);
                }
            };
            for (size_t k = 0; k < ends.size();) {
                const LevelObject& o = ends[k].piece;
                if (o.id == oid::OutlinePillar) { if (!isCellPiece(o)) cap(o.x - 15, o.x + 15); ++k; continue; }
                // two side lines up to four cells apart are one pillar
                if (k + 1 < ends.size() && ends[k + 1].piece.id == oid::OutlineTop && ends[k + 1].piece.x - o.x > 20 && ends[k + 1].piece.x - o.x < 125) {
                    cap(o.x - 0.75, ends[k + 1].piece.x + 0.75);
                    k += 2;
                    continue;
                }
                ++k;
            }
        }
        a = b;
    }
    for (const PillarEnd& pe : open) {
        const LevelObject& o = pe.piece;
        const float d = pe.dir;
        // the ground is drawn over what reaches into it
        const float sink = d < 0 && pe.target <= 0 ? 29.f : 0.5f;
        double y = o.y;
        for (int step = 0; step < 10; ++step) {
            LevelObject e = o;
            e.y = y + double(d * kCell);
            if (d < 0 ? e.y - 15 < pe.target - sink : e.y + 15 > pe.target + 0.5f) {
                // the last bit up to the bound
                const double rest = d < 0 ? (y - 15) - pe.target : pe.target - (y + 15);
                if (rest > 2 && rest < kCell && sink < 1) {
                    e.y = y + d * rest;
                    if (!isCellPiece(o) && fits(e)) placeObject(e); // a cell cannot be cut short
                }
                break;
            }
            if (fits(e)) {
                putPiece(e);
                y = e.y;
                continue;
            }
            for (float part : {25.f, 20.f, 15.f, 10.f, 5.f}) {
                e.y = y + double(d * part);
                if (!isCellPiece(o) && fits(e)) { placeObject(e); break; }
            }
            break;
        }
    }
}

// Path platforms that overlap or touch are one shape: drawn as one, without their edges crossing
// inside it. Only edges with solid on both sides go, so the faces the player touches stay as planned
// (the final check runs on the merged layout).
void Structurer::mergePlatforms(std::vector<LevelObject>& objs) const {
    const auto& S = m_path.surfaces;
    const size_t n = S.size();
    auto touch = [](const PathSurface& a, const PathSurface& b) {
        if (a.look != b.look) return false;
        const bool xs = a.cx0 <= b.cx1 + 1 && b.cx0 <= a.cx1 + 1, ys = a.cy0 <= b.cy1 + 1 && b.cy0 <= a.cy1 + 1;
        const bool xo = a.cx0 <= b.cx1 && b.cx0 <= a.cx1, yo = a.cy0 <= b.cy1 && b.cy0 <= a.cy1;
        return (xo && ys) || (yo && xs); // overlapping or sharing an edge, not only a corner
    };
    std::vector<int> comp(n, -1);
    int comps = 0;
    for (size_t i = 0; i < n; ++i) {
        if (comp[i] >= 0) continue;
        std::vector<size_t> todo{i};
        comp[i] = comps;
        while (!todo.empty()) {
            const size_t a = todo.back();
            todo.pop_back();
            for (size_t b = 0; b < n; ++b)
                if (comp[b] < 0 && touch(S[a], S[b])) { comp[b] = comps; todo.push_back(b); }
        }
        ++comps;
    }
    auto key = [](const LevelObject& o) {
        return std::to_string(o.id) + ":" + std::to_string(std::lround(o.x * 4)) + ":" + std::to_string(std::lround(o.y * 4)) + ":" +
               std::to_string(std::lround(std::fmod(o.rotation + 360.0, 360.0)));
    };
    std::vector<int> size(size_t(comps), 0);
    for (int c : comp) ++size[size_t(c)];
    std::multiset<std::string> drop;
    std::vector<LevelObject> add;
    for (int c = 0; c < comps; ++c) {
        if (size[size_t(c)] < 2) continue;
        CellGrid all;
        uint8_t look = 0;
        for (size_t i = 0; i < n; ++i) {
            if (comp[i] != c) continue;
            CellGrid one;
            one.fill(S[i].cx0, S[i].cy0, S[i].cx1, S[i].cy1);
            std::vector<LevelObject> pieces;
            tileCells(one, TileLook(S[i].look), pieces);
            for (auto& o : pieces) drop.insert(key(o));
            all.fill(S[i].cx0, S[i].cy0, S[i].cx1, S[i].cy1);
            look = S[i].look;
        }
        tileCells(all, TileLook(look), add);
    }
    if (add.empty()) return;
    std::vector<LevelObject> kept;
    kept.reserve(objs.size());
    for (auto& o : objs) {
        auto it = drop.find(key(o));
        if (it != drop.end()) { drop.erase(it); continue; }
        kept.push_back(o);
    }
    for (auto& o : add) kept.push_back(o);
    objs = std::move(kept);
}

// Every gamemode stretch uses a small set of shapes and repeats them, as real layouts do: that
// repetition is what makes a stretch look designed. A lane runs over the path and one under it,
// each structure as close to the reserved space as it fits.
void Structurer::placeModules(int firstLane, int lastLane) {
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
        m_groundStretch = !isFlyingMode(q.mode);
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
        // the front lanes hug the path; the back lanes fill the space behind them, so the player
        // flies through structures instead of past a few shapes
        for (int lane = firstLane; lane <= lastLane; ++lane) {
            const bool under = lane % 2 == 1, back = lane >= 2;
            // a classic ground stretch keeps its sky open and stands everything on the floor
            if ((openSky(q) && back) || (noCeiling(q) && under == (q.g < 0))) continue;
            si = size_t(i);
            int cx = c0 + (under ? 1 + int(r.below(3)) : 0) + (back ? 2 : 0);
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
                    for (int d = back ? 4 : 0; d < (back ? 10 : 7) && !placed; ++d) {
                        const int row = under ? base - d : base + d;
                        if (row < floorRow && under && row + m.h <= floorRow) break;
                        placed = placeModule(m, e.second, under, cx, row, floorRow, ceilRow);
                    }
                }
                if (placed) {
                    const float dens = std::clamp(m_density * (0.6f + 0.8f * intensityAt(double(si + 1) / phys::kTPS)), 0.05f, 1.0f);
                    const float pitch = st.pitchLo + (st.pitchHi - st.pitchLo) * (1.0f - dens) * 0.25f;
                    const int gap = std::max(1, int(std::lround(pitch))) + int(r.below(2));
                    if (gap >= 2 && r.chance(std::min(0.8, double(st.saws) * 0.6 + double(dens) * 0.2))) {
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

bool Structurer::bandCell(int cx, int cy) {
    if (cy < 0 || m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy)) return false;
    const Box b = cellBox(cx, cy);
    if (!inLevel(b)) return false;
    for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
    if (m_env.hits(b, m_margin) || m_built.hits(b, -1) || m_drawn.hits(b, -3)) return false;
    for (auto& po : m_path.objects)
        if (std::fabs(float(po.o.x) - (b.x0 + 15)) < 55 && std::fabs(float(po.o.y) - (b.y0 + 15)) < 55) return false;
    addCell(cx, cy);
    m_built.addBox(b);
    return true;
}

// A hazard on a face of the mass, pointing `toward` the open side (+1 up, -1 down). The check uses
// the circle around the object.
void Structurer::lineFace(float x, float faceY, int toward, int kind) {
    LevelObject o;
    if (kind == 0) o = make(oid::Spike, x, faceY + float(toward) * 15.f, toward > 0 ? 0 : 180);
    else o = make(oid::HalfSpike, x, faceY + float(toward) * 7.5f, toward > 0 ? 0 : 180);
    if (m_groundStretch && spikeRun({o}) > m_maxRun) return;
    if (hazardFits(o)) addHazard(o);
}

bool Structurer::hazardFits(const LevelObject& o) {
    phys::SimObject so;
    if (!phys::World::makeSimObject(o, 0, so)) return false;
    const float rad = std::sqrt(so.hw * so.hw + so.hh * so.hh);
    const Box b{so.x - rad, so.y - rad, so.x + rad, so.y + rad};
    if (!inLevel(b)) return false;
    for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
    if (m_env.hits(b, m_hazMargin + m_faceMargin)) return false;
    if (!hazardRoom(o)) return false;
    for (auto& po : m_path.objects)
        if (std::fabs(float(po.o.x) - so.x) < 45 && std::fabs(float(po.o.y) - so.y) < 45) return false;
    return true;
}

// A spike on the side of a step, pointing away from the wall.
void Structurer::lineWall(float wallX, float y, int side) {
    LevelObject o = make(oid::Spike, wallX + float(side) * 15.f, y, side > 0 ? 90 : 270);
    if (hazardFits(o)) addHazard(o);
}

// A saw biting out of a face.
void Structurer::faceSaw(float x, float y) {
    LevelObject sw = make(oid::SawMedium, x, y);
    phys::SimObject so;
    if (!phys::World::makeSimObject(sw, 0, so)) return;
    const Box b{so.x - so.hw, so.y - so.hw, so.x + so.hw, so.y + so.hw};
    if (!inLevel(b) || m_env.hits(b, m_hazMargin + 2)) return;
    if (!hazardRoom(sw)) return;
    for (auto& po : m_path.objects)
        if (std::fabs(float(po.o.x) - so.x) < 60 && std::fabs(float(po.o.y) - so.y) < 60) return;
    addHazard(sw);
    ++m_res.saws;
}

// The mass of a stretch: above and below the path one connected band, stepped in a rhythm, that the
// modules hang from. Real layouts are a corridor of solid shapes with hazards lining its faces, not
// separate shapes in the air.
void Structurer::bands() {
    if (m_density <= 0.31f || m_nominal.empty()) return;
    const int n = int(m_nominal.size());
    const float D = m_plan.baseDifficulty;
    static const int kThick[kStructureStyles][2] = {{1, 2}, {2, 3}, {3, 5}, {1, 3}, {1, 2}};
    const double hazardRate = std::clamp(0.45 + 0.13 * double(D), 0.45, 1.0);
    for (int i = 0; i < n;) {
        const Sample& q = m_nominal[size_t(i)];
        int j = i;
        while (j < n && m_nominal[size_t(j)].mode == q.mode) ++j;
        const int c0 = int(std::floor(q.x / kCell)), c1 = int(std::floor(m_nominal[size_t(j - 1)].x / kCell));
        const int first = i;
        i = j;
        if (q.mode == Mode::Wave || c1 - c0 < 3) continue;
        m_groundStretch = !isFlyingMode(q.mode);
        Rng r = at(c0, 0xBA4D);
        const float heat = intensityAt(double(first + 1) / phys::kTPS);
        if (heat < 0.3f && r.chance(0.2)) continue; // some quiet stretches stay open
        const int widths[3] = {2 + int(r.below(3)), 3 + int(r.below(4)), 2 + int(r.below(2))};
        const int depths[3] = {0, 1 + int(r.below(2)), int(r.below(2))};
        const int cols = c1 - c0 + 1;
        const size_t ncol = size_t(cols);
        for (int dir : {1, -1}) {
            if (openSky(q) && dir == q.g) continue;
            const auto& reach = dir > 0 ? m_reachOver : m_reachUnder;
            auto further = [dir](int a, int b) { return dir > 0 ? std::max(a, b) : std::min(a, b); };
            // the first free row of each column on this side of the reserved space
            std::vector<int> inner(ncol, INT_MIN);
            for (int c = 0; c < cols; ++c)
                for (int x = c0 + c - 1; x <= c0 + c + 1; ++x) {
                    float a, b;
                    if (!m_env.range(x, a, b)) continue;
                    const int row = dir > 0 ? int(std::ceil((b + m_margin) / kCell)) : int(std::floor((a - m_margin) / kCell)) - 1;
                    inner[size_t(c)] = inner[size_t(c)] == INT_MIN ? row : further(inner[size_t(c)], row);
                }
            // straight runs in a rhythm
            std::vector<int> start(ncol, INT_MIN), stop(ncol, INT_MIN), seg(ncol, 0);
            for (int c = 0, k = 0; c < cols; ++k) {
                const int w = std::min(widths[k % 3], cols - c);
                int row = INT_MIN;
                for (int x = c; x < c + w; ++x)
                    if (inner[size_t(x)] != INT_MIN) row = row == INT_MIN ? inner[size_t(x)] : further(row, inner[size_t(x)]);
                const int thick = kThick[m_style][0] + int(r.below(uint32_t(kThick[m_style][1] - kThick[m_style][0] + 1)));
                for (int x = c; x < c + w; ++x) {
                    seg[size_t(x)] = k;
                    if (row == INT_MIN) continue;
                    start[size_t(x)] = row + dir * depths[k % 3];
                    stop[size_t(x)] = start[size_t(x)] + dir * thick;
                }
                c += w;
            }
            // neighbours overlap, so the band is one piece; a very large step leaves an opening
            std::vector<int> stop2 = stop;
            for (int c = 0; c < cols; ++c) {
                if (start[size_t(c)] == INT_MIN) continue;
                for (int nb : {c - 1, c + 1})
                    if (nb >= 0 && nb < cols && start[size_t(nb)] != INT_MIN && std::abs(start[size_t(nb)] - start[size_t(c)]) <= 8)
                        stop2[size_t(c)] = further(stop2[size_t(c)], start[size_t(nb)]);
            }
            int lastFace = INT_MIN;
            for (int c = 0; c < cols; ++c) {
                int a = start[size_t(c)], b = stop2[size_t(c)];
                if (a == INT_MIN) { lastFace = INT_MIN; continue; }
                const float xc = (float(c0 + c) + 0.5f) * kCell;
                auto it = std::lower_bound(m_nominal.begin() + first, m_nominal.begin() + j, xc, [](const Sample& s, float v) { return s.x < v; });
                if (it == m_nominal.begin() + j) --it;
                const int floorRow = it->bounded ? int(std::lround(it->floorY / kCell)) : 0;
                const int ceilRow = it->bounded ? int(std::lround(it->ceilY / kCell)) - 1 : 90;
                // down to the ground or up to the bound when it is close: the mass stands on something
                if (dir < 0 && b - floorRow <= 3) b = floorRow;
                if (dir > 0 && it->bounded && ceilRow - b <= 3) b = ceilRow;
                // a module on this side sits between the path and the band: join it
                bool joined = false;
                if (auto m = reach.find(c0 + c); m != reach.end()) {
                    const int row = int(std::floor((m->second - float(dir)) / kCell));
                    if (dir > 0 ? row < a : row > a) { a = row; joined = true; }
                }
                int face = INT_MIN;
                for (int row = a; dir > 0 ? row <= b : row >= b; row += dir) {
                    if (row < floorRow || row > ceilRow) continue;
                    if (bandCell(c0 + c, row)) ++m_res.corridorCells;
                    if (face == INT_MIN && m_res.cells.filled(c0 + c, row)) face = row;
                }
                // no room for the mass: the floor or the ceiling itself is the face
                if (face == INT_MIN && !joined && (dir > 0 ? a > ceilRow : a < floorRow)) face = dir > 0 ? ceilRow + 1 : floorRow - 1;
                if (face == INT_MIN || joined) { lastFace = INT_MIN; continue; } // the modules bring their own spikes
                // the side of a step: the one further in faces back along the band
                if (lastFace != INT_MIN && lastFace != face) {
                    const bool thisIn = dir > 0 ? face < lastFace : face > lastFace;
                    const int low = std::min(face, lastFace), high = std::max(face, lastFace);
                    Rng w = at(c0 + c, 0xFACE3 + dir);
                    for (int row = low; row < high && row < low + 3; ++row)
                        if (w.chance(hazardRate)) lineWall(float(c0 + c) * kCell, (float(row + (dir > 0 ? 0 : 1)) + 0.5f) * kCell, thisIn ? -1 : 1);
                }
                lastFace = face;
                const float faceY = float(dir > 0 ? face : face + 1) * kCell;
                Rng h = at(c0 + c, dir > 0 ? 0xFACE1 : 0xFACE2);
                if (h.chance(std::clamp(double(D) * 0.04, 0.03, 0.2) + 0.5 * double(structureStyle(m_style).saws) + (heat > 0.65f ? 0.08 : 0.0)))
                    faceSaw(xc, faceY - float(dir) * 8.f);
                if (m_style == 3 || (heat > 0.65f && D >= 4)) {
                    if (h.chance(hazardRate)) lineFace(xc, faceY, -dir, 0);
                } else if (seg[size_t(c)] % 3 == 0 && h.chance(hazardRate)) {
                    lineFace(xc, faceY, -dir, 0);
                } else if (seg[size_t(c)] % 3 == 1 && h.chance(hazardRate)) {
                    lineFace(xc, faceY, -dir, 1);
                } else if (seg[size_t(c)] % 3 == 2 && h.chance(hazardRate)) {
                    lineFace(xc, faceY, -dir, 0);
                }
            }
        }
    }
}

// Some stretches are one big structure: the gaps between shapes on the same side of the path (a
// few cells across or down) are filled, so the shapes over the path, and those under it, join into
// one mass with one outline, the way many layouts build a whole stretch.
void Structurer::joinShapes() {
    if (m_density <= 0.31f || m_nominal.empty()) return;
    const int n = int(m_nominal.size());
    auto side = [&](int cx, int cy) {
        float a, b;
        if (!m_env.range(cx, a, b)) return 0;
        if (float(cy) * kCell >= b + m_margin) return 1;
        if (float(cy + 1) * kCell <= a - m_margin) return -1;
        return 0;
    };
    auto free = [&](int cx, int cy) {
        if (cy < 0 || m_res.cells.filled(cx, cy)) return false;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (m_platforms.filled(cx + dx, cy + dy)) return false;
        const Box b = cellBox(cx, cy);
        if (!inLevel(b)) return false;
        for (auto& w : m_waveSpans) if (b.x1 > w.first && b.x0 < w.second) return false;
        if (m_env.hits(b, std::max(m_margin, kSolidMargin)) || m_built.hits(shrunk(b, 3.f), 0) || m_drawn.hits(b, -1.f)) return false;
        for (auto& po : m_path.objects)
            if (std::fabs(float(po.o.x) - (b.x0 + 15)) < 50 && std::fabs(float(po.o.y) - (b.y0 + 15)) < 50) return false;
        return true;
    };
    for (int i = 0; i < n;) {
        const Sample& q = m_nominal[size_t(i)];
        int j = i;
        while (j < n && m_nominal[size_t(j)].mode == q.mode) ++j;
        const int c0 = int(std::floor(q.x / kCell)), c1 = int(std::floor(m_nominal[size_t(j - 1)].x / kCell));
        i = j;
        if (q.mode == Mode::Wave || c1 - c0 < 6) continue;
        Rng r = at(c0, 0x701E);
        if (!r.chance(0.55)) continue;
        // the filled cells of the stretch, by row and by column
        std::map<int, std::vector<int>> rows, cols;
        for (auto [k, kind] : m_res.cells.cells()) {
            (void)kind;
            const int cx = int(int32_t(uint32_t(k >> 32))), cy = int(int32_t(uint32_t(k)));
            if (cx < c0 || cx > c1) continue;
            rows[cy].push_back(cx);
            cols[cx].push_back(cy);
        }
        std::vector<std::pair<int, int>> add;
        auto bridge = [&](std::vector<std::pair<int, int>> gap) {
            if (gap.empty()) return;
            const int s = side(gap.front().first, gap.front().second);
            if (s == 0) return;
            for (auto [cx, cy] : gap)
                if (side(cx, cy) != s || !free(cx, cy)) return;
            for (auto& g : gap) add.push_back(g);
        };
        for (auto& [cy, xs] : rows) {
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); ++k) {
                const int gap = xs[k + 1] - xs[k] - 1;
                if (gap < 1 || gap > 4 || side(xs[k], cy) != side(xs[k + 1], cy)) continue;
                std::vector<std::pair<int, int>> g;
                for (int x = xs[k] + 1; x < xs[k + 1]; ++x) g.push_back({x, cy});
                bridge(g);
            }
        }
        for (auto& [cx, ys] : cols) {
            std::sort(ys.begin(), ys.end());
            for (size_t k = 0; k + 1 < ys.size(); ++k) {
                const int gap = ys[k + 1] - ys[k] - 1;
                if (gap < 1 || gap > 3 || side(cx, ys[k]) != side(cx, ys[k + 1])) continue;
                std::vector<std::pair<int, int>> g;
                for (int y = ys[k] + 1; y < ys[k + 1]; ++y) g.push_back({cx, y});
                bridge(g);
            }
        }
        for (auto [cx, cy] : add) {
            if (m_res.cells.filled(cx, cy)) continue;
            addCell(cx, cy);
            m_built.addBox(cellBox(cx, cy));
            ++m_res.corridorCells;
        }
    }
}

// One slope of a wave wall, its long edge through (x, y). solid: the player can reach it.
void Structurer::wallPiece(float x, float y, bool up, bool solidBelow, bool steep, bool solid, float scale) {
    LevelObject o = make(steep ? 1339 : 1338, x, y, steep ? 90 : 0);
    o.scale = scale;
    if (steep) {
        o.flipX = !solidBelow;
        o.flipY = solidBelow == up;
    } else {
        o.flipX = up != solidBelow;
        o.flipY = !solidBelow;
    }
    m_objects.push_back(o);
    if (!solid) return;
    m_W.addObject(o, -1);
    const float hw = 15.f * scale, hh = (steep ? 30.f : 15.f) * scale;
    m_built.addBox({x - hw, y - hh, x + hw, y + hh});
    ++m_res.corridorCells;
}

bool Structurer::nearPathObject(float x, float y) const {
    for (auto& po : m_path.objects) {
        const auto kind = po.o.info().kind;
        // a wall line runs past a portal: leaving it out would open a hole in the corridor
        if (kind == level::Kind::ModePortal || kind == level::Kind::GravityPortal || kind == level::Kind::SizePortal || kind == level::Kind::SpeedPortal) continue;
        if (std::fabs(float(po.o.x) - x) < 55 && std::fabs(float(po.o.y) - y) < 70) return true;
    }
    return false;
}

// Whether the planned path comes within a few units of the solid side of a wall piece.
bool Structurer::pieceBlocksPath(float x, float y, float slope, bool solidBelow, float halfW) const {
    const float x0 = x - halfW, x1 = x + halfW;
    const float half = std::fabs(slope) * halfW; // half the piece's height
    auto it = std::lower_bound(m_nominal.begin(), m_nominal.end(), x0 - 12.f, [](const Sample& q, float v) { return q.x < v; });
    const float clear = m_waveMargin * 0.3f;
    for (; it != m_nominal.end() && it->x <= x1 + 12.f; ++it) {
        if (it->x + it->half + 4 < x0 || it->x - it->half - 4 > x1) continue; // a turn beyond the tip
        // the sloped edge, and past the ends of the piece the tip of it
        const float edge = y + slope * (std::clamp(it->x, x0, x1) - x);
        if (solidBelow ? it->y - it->half < edge + clear : it->y + it->half > edge - clear) return true;
        // the straight side of the piece, at one end only
        const bool atRight = (slope < 0) != solidBelow;
        if (half > 0 && it->x >= x0 && it->x <= x1 && std::fabs(it->y - y) < half + it->half && (atRight ? it->x > x1 - 6 : it->x < x0 + 6)) return true;
    }
    return false;
}

// Wave parts: both walls of the corridor follow every stroke of the zig-zag at the same distance,
// the whole way, as one outline with nothing behind it. GD objects need no grid, so the pieces lie
// exactly along the strokes and the clicks can fall anywhere.
void Structurer::waveWalls() {
    if (m_density <= 0.01f) return;
    const int n = int(m_nominal.size());
    for (int i = 0; i < n;) {
        if (m_nominal[size_t(i)].mode != Mode::Wave) { ++i; continue; }
        int j = i;
        while (j < n && m_nominal[size_t(j)].mode == Mode::Wave) ++j;
        m_waveSpans.push_back({m_nominal[size_t(i)].x - 45.f, m_nominal[size_t(j - 1)].x + 45.f});
        // the open ends of both walls, to close the corridor at the end of the part
        struct End { float x, y; bool set = false; };
        End first[2], last[2]; // [0] over the path, [1] under it
        auto note = [&](int side, float xl, float yl, float xr, float yr) {
            if (!first[side].set || xl < first[side].x) first[side] = {xl, yl, true};
            if (!last[side].set || xr > last[side].x) last[side] = {xr, yr, true};
        };
        // the strokes: straight pieces of the path, split where the slope changes (a mini or speed
        // portal) as well as at every turn
        const auto dirAt = [&](int t) {
            const Sample& u = m_nominal[size_t(t)];
            const Sample& v = m_nominal[size_t(t + 1)];
            const float d = v.y - u.y, dx = std::max(0.01f, v.x - u.x);
            const int sgn = d > 0.05f ? 1 : d < -0.05f ? -1 : 0;
            return std::fabs(d) > 1.5f * dx ? sgn * 2 : sgn;
        };
        // where each wall's line ended, so a jump in the corridor's width (a size portal) is closed
        // with an upright line instead of leaving a gap
        struct Tip { float x, y; bool set = false; };
        Tip tip[2];
        auto joinTo = [&](int side, float x, float y) {
            Tip& t = tip[side];
            if (t.set && std::fabs(t.x - x) < 2.0f && std::fabs(t.y - y) > 2.0f) {
                const float lo = std::min(t.y, y), hi = std::max(t.y, y);
                // never in the way: the player must not graze it (at the end of a part it would)
                const float clear = m_waveMargin * 0.3f;
                auto it = std::lower_bound(m_nominal.begin(), m_nominal.end(), x - 40.f, [](const Sample& q, float v) { return q.x < v; });
                for (; it != m_nominal.end() && it->x <= x + 40.f; ++it)
                    if (std::fabs(it->x - x) < it->half + clear && it->y + it->half + clear > lo && it->y - it->half - clear < hi) return;
                const int count = std::max(1, int(std::ceil((hi - lo) / kCell - 0.01f)));
                const float len = (hi - lo) / float(count);
                for (int k = 0; k < count; ++k) {
                    const float cy = lo + len * (float(k) + 0.5f);
                    LevelObject line = make(oid::OutlineTop, x, cy, 90);
                    line.scale = len / kCell;
                    m_objects.push_back(line);
                    m_W.addObject(line, -1);
                }
            }
        };
        for (int a = i + 1; a + 1 < j;) {
            const int key = dirAt(a);
            int b = a + 1;
            while (b + 1 < j && dirAt(b) == key) ++b;
            const Sample& p0 = m_nominal[size_t(a)];
            const Sample& p1 = m_nominal[size_t(b)];
            const float width = p1.x - p0.x;
            a = b;
            if (width < 3) continue;
            const int dir = key > 0 ? 1 : key < 0 ? -1 : 0;
            // a 2:1 stroke moves twice as fast up and down, so a late press costs twice the room
            const float gap0 = m_waveMargin * (p0.half < 4 ? 1.6f : 1.0f) + p0.half;
            const float ceilEdge = p0.bounded ? p0.ceilY : 2700.f, floorEdge = p0.bounded ? p0.floorY : 0.f;
            // the stroke is tiled exactly: whole pieces, all scaled down a little so the last one ends
            // on the turn (a stroke shorter than a block gets one small piece)
            const int count = std::max(1, int(std::ceil(width / kCell - 0.01f)));
            const float step = width / float(count), sc = step / kCell, hw = 15.f * sc;
            if (dir == 0) {
                // a slide: a straight stretch of wall joins the strokes on both sides
                for (const float y : {p0.y + gap0, p0.y - gap0}) {
                    const int side = y > p0.y ? 0 : 1;
                    if (y >= ceilEdge || y <= floorEdge) continue;
                    joinTo(side, p0.x, y);
                    for (int k = 0; k < count; ++k) {
                        const float x = p0.x + step * (float(k) + 0.5f);
                        if (nearPathObject(x, y) || pieceBlocksPath(x, y, 0, side == 1, hw)) continue;
                        LevelObject line = make(oid::OutlineTop, x, y, 0);
                        line.scale = sc;
                        m_objects.push_back(line);
                        m_W.addObject(line, -1);
                        note(side, x - hw, y, x + hw, y);
                    }
                    tip[side] = {p1.x, y, true};
                }
                continue;
            }
            const bool steep = key == 2 || key == -2;
            const float slope = (p1.y - p0.y) / width;
            const float rise = float(dir) * (steep ? 2.0f : 1.0f);
            const float half = (steep ? 30.f : 15.f) * sc;
            for (int side = 0; side < 2; ++side) {
                const float off = side == 0 ? gap0 : -gap0;
                joinTo(side, p0.x, p0.y + off);
                for (int k = 0; k < count; ++k) {
                    const float x = p0.x + step * (float(k) + 0.5f);
                    const float y = p0.y + slope * (x - p0.x) + off;
                    // the bounds where the piece is (a wave portal can move them)
                    auto at = std::lower_bound(m_nominal.begin() + i, m_nominal.begin() + j, x, [](const Sample& q, float v) { return q.x < v; });
                    if (at == m_nominal.begin() + j) --at;
                    const float ceilY = (at->bounded ? at->ceilY : ceilEdge) - 2.f, floorY = (at->bounded ? at->floorY : floorEdge) + 2.f;
                    bool fits = side == 0 ? y + half <= ceilY : y - half >= floorY;
                    // at the portals in and out the player is another mode with a bigger hitbox (a
                    // ship is three times as tall as a wave): there the whole piece keeps clear of it
                    if (fits && (x - hw < m_nominal[size_t(i)].x + 60.f || x + hw > m_nominal[size_t(j - 1)].x - 60.f)) {
                        auto it = std::lower_bound(m_nominal.begin(), m_nominal.end(), x - hw - 40.f, [](const Sample& q, float v) { return q.x < v; });
                        for (; fits && it != m_nominal.end() && it->x <= x + hw + 40.f; ++it) {
                            const float r = std::max(it->half, 15.f) + 3.f;
                            fits = !(it->x + r > x - hw && it->x - r < x + hw && it->y + r > y - half && it->y - r < y + half);
                        }
                    }
                    if (!fits || nearPathObject(x, y) || pieceBlocksPath(x, y, rise, side == 1, hw)) continue;
                    wallPiece(x, y, dir > 0, side == 1, steep, true, sc);
                    note(side, x - hw, y - rise * hw, x + hw, y + rise * hw);
                }
                tip[side] = {p1.x, p1.y + off, true};
            }
        }
        // the corridor is closed at both ends: from each wall's end a line runs on to the bound,
        // where it does not cross the way in or out
        const Sample& in = m_nominal[size_t(i)];
        const Sample& out = m_nominal[size_t(j - 1)];
        for (int side = 0; side < 2; ++side)
            for (const End& e : {first[side], last[side]}) {
                const Sample& at = &e == &first[side] ? in : out;
                if (!e.set || !at.bounded) continue;
                const float dir = side == 0 ? 1.f : -1.f, bound = side == 0 ? at.ceilY : at.floorY;
                for (float y = e.y + dir * 15.f; dir > 0 ? y - 15 < bound - 1 : y + 15 > bound + 1; y += dir * kCell) {
                    LevelObject line = make(oid::OutlineTop, e.x, y, 90);
                    const Box b{e.x - 2, y - 15, e.x + 2, y + 15};
                    if (m_env.hits(b, 2) || m_built.hits(b, -3)) break;
                    m_objects.push_back(line);
                    m_W.addObject(line, -1);
                    m_built.addBox(b);
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
        // measured well past the target (up to 100 ms each way), so loose clicks show as loose
        const int cap = std::max(m_halfWin * 2, 24);
        int e = 0, l = 0;
        while (e < cap && ok(-(e + 1))) ++e;
        while (l < cap && ok(l + 1)) ++l;
        m_res.checks[i].early = e;
        m_res.checks[i].late = l;
    }
}

StructureResult Structurer::run() {
    const DifficultyProfile prof = profileAt(m_plan.baseDifficulty);
    m_halfWin = std::clamp(int(std::lround(prof.minWindowMs * 0.5 / 1000.0 * phys::kTPS)), 4, 30);
    // 3 units on top: on runs logged in GD the simulator's path is off by a few units after some pads
    m_margin = std::clamp(prof.clearance * 0.3f + 2.0f, 2.0f, 16.0f) + 3.0f;
    // demons: the spikes come close to the path (Extreme a couple of units off the timing window's
    // runs), which is what makes a mistimed press die; blocks keep their distance
    m_hazMargin = m_plan.baseDifficulty > 5.0f ? std::max(2.5f, m_margin - (m_plan.baseDifficulty - 5.0f)) : m_margin;
    m_faceMargin = m_plan.baseDifficulty > 5.0f ? std::max(1.0f, 5.0f - (m_plan.baseDifficulty - 5.0f) * 1.2f) : 5.0f;
    // about a block and a half to each wall at Insane, Grief's 25 at the top
    m_waveMargin = std::clamp(76.0f - 6.0f * m_plan.baseDifficulty, 24.0f, 70.0f);
    m_seed = m_rng.next();
    if (m_opt.structureStyle > 0) {
        m_style = std::clamp(m_opt.structureStyle - 1, 0, kStructureStyles - 1);
    } else {
        // automatic: below demon ratings the classic look of RobTop's and rated Insane levels;
        // demons mostly the rail style of Bidal Wave
        const uint64_t pick = m_seed % 20;
        if (m_plan.baseDifficulty < 5.0f) m_style = pick < 16 ? 4 : 0;
        else m_style = pick < 9 ? 0 : pick < 15 ? 1 : pick < 18 ? 2 : 3;
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
            while (ci < m_path.clicks.size() && std::max(0, m_path.clicks[ci].tick - std::max(3 * m_halfWin, 24) - 3) <= s.tick) { pre[ci] = s; ++ci; }
            int next = s.tick + 1;
            while (ei < ev.size() && ev[ei].tick <= next) { held = ev[ei].press; ++ei; }
            phys::step(W, s, held);
            samples.push_back(sampleOf(s));
        }
        for (; ci < m_path.clicks.size(); ++ci) pre[ci] = s;
        return s.dead ? s.tick : -1;
    };
    if (int dt = nominalRun(m_W, m_nominal, m_pre); dt >= 0) m_res.notes.push_back("the path dies before the structures are built (tick " + std::to_string(dt) + ")");
    const float endX = std::floor(((m_nominal.empty() ? 600.0f : m_nominal.back().x) - 45.0f) / 30.0f) * 30.0f + 15;
    m_xMax = endX - 15;
    {
        std::vector<float> col(size_t(std::max(1.0f, endX / kCell + 2)), 0.0f);
        for (const Sample& q : m_nominal) {
            const int c = int(std::floor(q.x / kCell));
            if (c >= 0 && c < int(col.size())) col[size_t(c)] = std::max(col[size_t(c)], q.y + q.half);
        }
        m_pathTop.assign(col.size(), 0.0f);
        for (int c = 0; c < int(col.size()); ++c)
            for (int d = -10; d <= 10; ++d)
                if (c + d >= 0 && c + d < int(col.size())) m_pathTop[size_t(c)] = std::max(m_pathTop[size_t(c)], col[size_t(c + d)]);
    }
    buildEnvelope();
    waveWalls();
    // Easy and Normal ground parts have at most two spikes in a row, Hard and Harder three
    m_maxRun = m_plan.baseDifficulty < 2.0f ? 2 : m_plan.baseDifficulty < 3.5f ? 3 : 99;
    placeModules(0, 1);
    bands();
    placeModules(2, 3);
    m_groundStretch = false;
    extendPillars();
    joinShapes();
    // what the structures do not force yet gets a hazard of its own
    if (m_prm.necessity) necessity();
    else for (auto& c : m_path.clicks) m_res.checks.push_back({c.click, false, 0, 0});
    if (m_prm.necessity) tighten();
    if (m_prm.necessity) threadPath();
    // ---- the finished layout: path objects, platform pieces (as planned), structures, hazards
    level::Level& L = m_res.level;
    // an edge line with filled cells on both sides is inside a shape
    {
        auto solid = [&](int cx, int cy) { return m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy); };
        std::vector<LevelObject> kept;
        for (auto& o : m_objects) {
            if (o.id == oid::OutlineTop) {
                const bool upright = std::fmod(std::fabs(o.rotation), 180.0) == 90;
                const int a = int(std::lround((upright ? o.x : o.y) / kCell)); // the cell boundary it lies on
                const int c = int(std::floor((upright ? o.y : o.x) / kCell));
                const bool inside = upright ? solid(a - 1, c) && solid(a, c) : solid(c, a - 1) && solid(c, a);
                if (inside) continue;
            }
            kept.push_back(o);
        }
        m_objects = std::move(kept);
    }
    // a saw bites into something (a block, the ground, a bound): one alone in the air is dropped
    {
        std::vector<Box> solids;
        for (auto& o : m_objects) {
            phys::SimObject so;
            if (phys::World::makeSimObject(o, 0, so) && so.kind == level::Kind::Solid) solids.push_back({so.x - so.hw, so.y - so.hh, so.x + so.hw, so.y + so.hh});
        }
        auto attached = [&](const LevelObject& o) {
            phys::SimObject so;
            if (!phys::World::makeSimObject(o, 0, so) || so.kind != level::Kind::Saw) return true;
            const float r = so.hw + 4;
            if (so.y - r <= 0) return true;
            auto it = std::lower_bound(m_nominal.begin(), m_nominal.end(), so.x, [](const Sample& q, float v) { return q.x < v; });
            if (it != m_nominal.end() && it->bounded && (so.y - r <= it->floorY || so.y + r >= it->ceilY)) return true;
            const Box b{so.x - r, so.y - r, so.x + r, so.y + r};
            for (int cx = int(std::floor(b.x0 / kCell)); cx <= int(std::floor(b.x1 / kCell)); ++cx)
                for (int cy = int(std::floor(b.y0 / kCell)); cy <= int(std::floor(b.y1 / kCell)); ++cy)
                    if (m_res.cells.filled(cx, cy) || m_platforms.filled(cx, cy)) return true;
            for (auto& sb : solids) if (overlap(b, sb, 0)) return true;
            return false;
        };
        m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [&](const LevelObject& o) { return !attached(o); }), m_objects.end());
        m_hazards.erase(std::remove_if(m_hazards.begin(), m_hazards.end(), [&](const LevelObject& o) { return !attached(o); }), m_hazards.end());
    }
    L = m_base;
    mergePlatforms(L.objects);
    tileCells(m_res.cells, m_prm.look, L.objects);
    for (auto& o : m_objects) L.objects.push_back(o);
    for (auto& h : m_hazards) L.objects.push_back(h);
    // the end: a little before the last planned position (the run beyond it is not planned)
    LevelObject end = make(oid::TrigEnd, endX, 600);
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
