// Generator v2: the gameplay path (see path.hpp). Beam search over what each click does and
// what happens between clicks, with the physics simulator as the judge.
#include <al/gen/path.hpp>
#include <al/gen/profile.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace al::gen {

namespace oid = level::id;
using level::LevelObject;

const char* actName(Act a) {
    switch (a) {
        case Act::None: return "none";
        case Act::Jump: return "jump";
        case Act::Tap: return "tap";
        case Act::Hold: return "hold";
        case Act::Orb: return "orb";
        case Act::Land: return "land";
        case Act::Pad: return "pad";
        case Act::GravityPortal: return "gravity";
        case Act::ModePortal: return "portal";
        default: return "?";
    }
}

namespace {

constexpr float kCell = 30.0f;
constexpr int kTopRow = 17;          // hanging platforms reach up to here (above the screen top)
constexpr float kMaxY = 450.0f;      // highest comfortable player position (unbounded modes)

int tickOf(double t) { return int(std::lround(t * phys::kTPS)); }

bool groundMode(Mode m) { return m == Mode::Cube || m == Mode::Robot; }
bool isFlying(Mode m) { return m == Mode::Ship || m == Mode::Ufo || m == Mode::Wave || m == Mode::Swing; }
bool surfaceMode(Mode m) { return m == Mode::Ball || m == Mode::Spider; }

int portalIdFor(Mode m) {
    switch (m) {
        case Mode::Cube: return oid::PortalCube;
        case Mode::Ship: return oid::PortalShip;
        case Mode::Ball: return oid::PortalBall;
        case Mode::Ufo: return oid::PortalUfo;
        case Mode::Wave: return oid::PortalWave;
        case Mode::Robot: return oid::PortalRobot;
        case Mode::Spider: return oid::PortalSpider;
        default: return oid::PortalSwing;
    }
}
int speedPortalId(int s) {
    static const int ids[5] = {oid::Speed05, oid::Speed1, oid::Speed2, oid::Speed3, oid::Speed4};
    return ids[std::clamp(s, 0, 4)];
}

LevelObject make(int id, double x, double y, double rot = 0) {
    LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

// A surface as the outline pieces it will have in the finished level (the stair-snap rule of
// the game depends on the exact pieces, so planning uses the same ones). Each piece gets its own
// object id: kSurfaceIds + surface id * 512 + n.
constexpr int kSurfaceIds = 1 << 24;
template <class F>
void forEachSurfaceBlock(const PathSurface& s, int gid, F&& f) {
    CellGrid g;
    g.fill(s.cx0, s.cy0, s.cx1, s.cy1);
    std::vector<LevelObject> tiles;
    tileCells(g, TileLook::Outline, tiles);
    int n = 0;
    for (auto& o : tiles) f(std::move(o), kSurfaceIds + (gid & 0x7fff) * 512 + std::min(n++, 511));
}

struct Obj {
    int gid = 0;
    LevelObject o;
    Act act = Act::None;
    int click = -1;
};

struct Node;
using NodePtr = std::shared_ptr<const Node>;

// The state's object references (sim indices) are stored as object ids (gid) between worlds.
struct Node {
    NodePtr parent;
    int click = -1;
    phys::GameState s;              // end of the interval, object references as gids
    std::vector<Obj> objs;
    std::vector<std::pair<int, PathSurface>> surfaces; // (gid, surface)
    std::vector<bot::InputEvent> inputs;
    PathClick info;
    bool used = false;
    double score = 0;
    Act act = Act::None;
    int sub = 0;
    float endX = 0;
    std::vector<TraceSample> tail; // the last stretch of the path (new platforms must not cut it)
};

// ---- object references in the player state <-> gids

template <class F>
void forEachRef(phys::PlayerState& p, F&& f) {
    for (auto& a : p.activated) a = f(a);
    for (auto& a : p.touchingRings) a = f(a);
    p.slopeObj = f(p.slopeObj);
    p.lastSnapObj = f(p.lastSnapObj);
    p.eventObj = f(p.eventObj);
    p.deathObj = f(p.deathObj);
}

void toGid(phys::GameState& s, const phys::World& W) {
    auto f = [&](int i) { return i >= 0 && i < int(W.size()) ? W.objects()[size_t(i)].levelIndex : -1; };
    for (auto& p : s.p) forEachRef(p, f);
}

void fromGid(phys::GameState& s, const std::unordered_map<int, int>& simOf) {
    auto f = [&](int g) {
        if (g < 0) return -1;
        auto it = simOf.find(g);
        return it == simOf.end() ? -2 : it->second; // -2: an object no longer in the local world
    };
    for (auto& p : s.p) {
        forEachRef(p, f);
        p.touchingCount = 0; // recomputed in the new world (refreshTouching)
    }
}

// ---- the local world of a node: every object near or ahead of the player

struct LocalWorld {
    phys::World W;
    std::unordered_map<int, int> simOf;
    void add(const LevelObject& o, int gid) {
        int i = W.addObject(o, gid);
        if (i >= 0) simOf[gid] = i;
    }
    void truncate(size_t n) {
        while (W.size() > n) {
            simOf.erase(W.objects().back().levelIndex);
            W.truncate(W.size() - 1);
        }
    }
};

void buildLocal(LocalWorld& lw, const Node* n, float x0, const level::LevelSettings& st) {
    lw.W = phys::World();
    lw.W.setSettings(st);
    lw.W.setEndX(1e9f);
    lw.simOf.clear();
    std::vector<std::pair<int, LevelObject>> objs;
    int walked = 0;
    for (const Node* k = n; k; k = k->parent.get(), ++walked) {
        for (auto& ob : k->objs) if (ob.o.x + 60 >= x0 - 150) objs.push_back({ob.gid, ob.o});
        for (auto& [gid, sf] : k->surfaces)
            if ((sf.cx1 + 1) * kCell >= x0 - 150) forEachSurfaceBlock(sf, gid, [&](LevelObject o, int id) { objs.push_back({id, std::move(o)}); });
        if (walked >= 3 && k->endX < x0 - 900) break;
    }
    std::sort(objs.begin(), objs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [gid, o] : objs) lw.add(o, gid);
}

TraceSample sampleOf(const phys::GameState& s) {
    const auto& p = s.p[0];
    TraceSample q;
    q.x = p.x;
    q.y = p.y;
    q.half = p.height() * 0.5f;
    q.mode = p.mode;
    q.g = int8_t(p.flip());
    q.onGround = p.onGround;
    q.pressed = s.pressed;
    return q;
}

// Run the state (in local-world sim indices) to untilTick with the given inputs.
bool runLocal(const phys::World& W, phys::GameState& s, const std::vector<bot::InputEvent>& ev, int untilTick, std::vector<TraceSample>* trace) {
    bool held = s.pressed;
    size_t ei = 0;
    while (ei < ev.size() && ev[ei].tick <= s.tick) { held = ev[ei].press; ++ei; }
    while (s.tick < untilTick && !s.dead && !s.won) {
        int next = s.tick + 1;
        while (ei < ev.size() && ev[ei].tick <= next) { held = ev[ei].press; ++ei; }
        phys::step(W, s, held);
        if (trace) trace->push_back(sampleOf(s));
    }
    return !s.dead;
}

// ---- candidates

struct Cand {
    std::vector<Obj> objs;
    std::vector<std::pair<int, PathSurface>> surfaces;
    std::vector<bot::InputEvent> inputs;
    Act act = Act::None;
    int sub = 0;
    bool used = false;
    PathClick info;
    double score = 0;
    // results of the last evaluation
    phys::GameState end;
    std::vector<TraceSample> trace; // from the node's tick + 1
    bool alive = false;
};

class Planner {
public:
    Planner(const DesignPlan& plan, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng, const PathParams& prm, const Progress* progress)
        : m_plan(plan), m_A(A), m_opt(opt), m_rng(rng), m_prm(prm), m_progress(progress) {}

    PathPlan run();

private:
    const DesignPlan& m_plan;
    const audio::MusicAnalysis& m_A;
    const GenOptions& m_opt;
    Rng& m_rng;
    PathParams m_prm;
    const Progress* m_progress;
    std::vector<PathSegment> m_segs;
    std::vector<int> m_segOf;
    level::LevelSettings m_settings;
    int m_nextGid = 1;
    uint64_t m_noiseSeed = 0;
    float m_D = 4;
    int m_minRun = 8;

    // the node being expanded
    const Node* m_node = nullptr;
    LocalWorld m_lw;
    phys::GameState m_start; // node state in local sim indices

    int newGid() { return m_nextGid++; }
    bool evaluate(Cand& c, int untilTick);
    void addObj(Cand& c, LevelObject o, Act act, int click) { c.objs.push_back({newGid(), std::move(o), act, click}); }
    void expand(const NodePtr& n, int k, std::vector<NodePtr>& out);
    void clickOptions(int k, int T, int Tn, std::vector<Cand>& out);
    void bridges(Cand base, int k, int T, int Tn, std::vector<Cand>& out);
    bool addLandings(const Cand& base, int fromTick, int Tn, std::vector<Cand>& out, int maxOptions);
    bool objectsNear(const Cand& c, float x, float y, float r) const;
    double scoreCand(const Cand& c, int k, int T, int Tn) const;
    NodePtr finish(const NodePtr& n, int k, Cand& c);
    NodePtr search(NodePtr start, int fromClick, PathPlan& out);
    void rescue(Cand base, int k, int T, int Tn, std::vector<Cand>& out);
    void makeSegments(const std::vector<uint64_t>& salts);
    const Part* partAt(double t) const {
        for (const auto& p : m_plan.parts) if (t >= p.t0 && t < p.t1) return &p;
        return m_plan.parts.empty() ? nullptr : &m_plan.parts.back();
    }
};

bool Planner::evaluate(Cand& c, int untilTick) {
    // objects of this stretch must not reach back to where the player already was (planning
    // would not see them there); orbs are the exception (refreshTouching + the hold check)
    const float startX = m_start.p[0].x + m_start.p[0].width() * 0.5f;
    for (auto& ob : c.objs) {
        const auto& info = ob.o.info();
        if (info.kind == level::Kind::Orb) continue;
        const float hw = (std::fmod(std::fabs(ob.o.rotation), 180.0) > 45 ? info.h : info.w) * 0.5f;
        if (float(ob.o.x) - hw < startX + 1) { c.alive = false; return false; }
    }
    const size_t mark = m_lw.W.size();
    for (auto& ob : c.objs) m_lw.add(ob.o, ob.gid);
    for (auto& [gid, sf] : c.surfaces) forEachSurfaceBlock(sf, gid, [&](const LevelObject& o, int id) { m_lw.add(o, id); });
    c.end = m_start;
    phys::refreshTouching(m_lw.W, c.end);
    c.trace.clear();
    std::vector<bot::InputEvent> ev = c.inputs;
    std::sort(ev.begin(), ev.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    c.alive = runLocal(m_lw.W, c.end, ev, untilTick, &c.trace);
    toGid(c.end, m_lw.W);
    m_lw.truncate(mark);
    return c.alive;
}

bool Planner::objectsNear(const Cand& c, float x, float y, float r) const {
    auto near = [&](const LevelObject& o) {
        auto k = o.info().kind;
        if (k != level::Kind::Orb && k != level::Kind::Pad && k != level::Kind::ModePortal && k != level::Kind::GravityPortal &&
            k != level::Kind::SpeedPortal && k != level::Kind::SizePortal)
            return false;
        return std::fabs(float(o.x) - x) < r && std::fabs(float(o.y) - y) < r;
    };
    for (auto& ob : c.objs) if (near(ob.o)) return true;
    int walked = 0;
    for (const Node* k = m_node; k && walked < 4; k = k->parent.get(), ++walked)
        for (auto& ob : k->objs) if (near(ob.o)) return true;
    return false;
}

// What the click at tick T can do, given the player's state just before it.
void Planner::clickOptions(int k, int T, int Tn, std::vector<Cand>& out) {
    const Click& ck = m_plan.clicks[size_t(k)];
    const auto& p = m_start.p[0];
    const Mode m = p.mode;
    const int latest = std::max(T + 2, Tn - 2);
    const int recordedRelease = std::clamp(tickOf(ck.release), T + 2, latest);
    auto press = [&](Cand& c, int rel) {
        c.inputs = {{T, true}, {std::clamp(rel, T + 2, latest), false}};
        c.used = true;
        c.info.click = k;
        c.info.tick = T;
        c.info.releaseTick = std::clamp(rel, T + 2, latest);
        c.info.mode = m;
        c.info.grounded = p.onGround;
        c.info.x = p.x;
        c.info.y = p.y;
        c.info.segment = m_segOf[size_t(k)];
    };
    auto holdsFor = [&](std::vector<int>& rels) {
        if (ck.recorded) { rels.push_back(recordedRelease); return; }
        const int gap = Tn - T;
        for (double f : {0.0, 0.25, 0.5, 0.8}) {
            int r = f == 0.0 ? T + 4 : T + int(gap * f);
            if (std::find(rels.begin(), rels.end(), r) == rels.end() && r <= latest) rels.push_back(r);
        }
    };
    // where the player will be at the press (for orbs)
    auto posAtPress = [&](float& x, float& y) {
        phys::GameState q = m_start;
        phys::refreshTouching(m_lw.W, q);
        runLocal(m_lw.W, q, {{T, true}}, T, nullptr);
        x = q.p[0].x;
        y = q.p[0].y;
    };
    auto orbOptions = [&](std::initializer_list<int> ids) {
        float x, y;
        posAtPress(x, y);
        if (y < 25 || y > 470) return;
        for (int id : ids) {
            Cand c;
            press(c, T + 3);
            if (id == oid::OrbDash) {
                if (Tn - T < 60) continue;
                c.inputs[1].tick = c.info.releaseTick = std::min(latest, T + 40);
            }
            if (id == oid::OrbBlack && y < 150) continue;
            if (objectsNear(c, x, y, 40)) continue;
            // the end of the previous stretch must not touch the orb while holding (a held button
            // would take the orb early)
            bool early = false;
            const float reach = 18 + 16;
            if (m_node) for (auto& r : m_node->tail) if (r.pressed && std::fabs(r.x - x) < reach && std::fabs(r.y - y) < reach) { early = true; break; }
            if (early) continue;
            LevelObject o = make(id, std::round(x * 2) / 2, std::round(y * 2) / 2);
            if (p.upsideDown && (id == oid::OrbDash || id == oid::OrbDashPink)) o.flipY = true;
            addObj(c, o, Act::Orb, k);
            c.act = Act::Orb;
            c.sub = id;
            c.info.act = Act::Orb;
            c.info.objId = id;
            out.push_back(std::move(c));
        }
    };
    if (groundMode(m)) {
        if (p.onGround) {
            std::vector<int> rels;
            if (m == Mode::Robot) holdsFor(rels);
            else rels.push_back(T + 3);
            for (int r : rels) {
                Cand c;
                press(c, r);
                c.act = Act::Jump;
                c.sub = r - T > 6 ? 1 : 0;
                c.info.act = Act::Jump;
                out.push_back(std::move(c));
            }
        } else {
            orbOptions({oid::OrbYellow, oid::OrbPink, oid::OrbRed, oid::OrbBlue, oid::OrbGreen, oid::OrbBlack, oid::OrbDash});
        }
    } else if (surfaceMode(m)) {
        if (p.onGround) {
            Cand c;
            press(c, T + 3);
            c.act = Act::Jump;
            c.info.act = Act::Jump;
            out.push_back(std::move(c));
        } else {
            orbOptions({oid::OrbYellow, oid::OrbBlue, oid::OrbGreen});
        }
    } else if (m == Mode::Ufo || m == Mode::Swing) {
        Cand c;
        press(c, T + 3);
        c.act = Act::Tap;
        c.info.act = Act::Tap;
        out.push_back(std::move(c));
        if (m == Mode::Ufo) orbOptions({oid::OrbYellow, oid::OrbBlue, oid::OrbGreen});
    } else { // ship, wave
        std::vector<int> rels;
        holdsFor(rels);
        for (int r : rels) {
            Cand c;
            press(c, r);
            c.act = r - T > 8 ? Act::Hold : Act::Tap;
            c.info.act = c.act;
            out.push_back(std::move(c));
        }
    }
}

// Platforms the falling player of `base` could land on before tick Tn (so it is on the ground
// when the next click comes).
bool Planner::addLandings(const Cand& base, int fromTick, int Tn, std::vector<Cand>& out, int maxOptions) {
    if (base.trace.empty()) return false;
    const int t0 = m_start.tick + 1;
    std::vector<std::pair<int, int>> seen; // (cx0, row)
    int added = 0;
    // landing ticks spread over the descent: late ones (land just before the click) and early
    // ones (land soon and run along the platform; needed for long gaps)
    const int lastLand = std::min(Tn - 1 - m_minRun, t0 + int(base.trace.size()) - 2);
    int firstDesc = -1;
    for (int t = std::max(fromTick + 6, t0 + 2); t <= lastLand; ++t) {
        const TraceSample& q = base.trace[size_t(t - t0)];
        const TraceSample& q0 = base.trace[size_t(t - t0 - 2)];
        if (!q.onGround && q.g * (q.y - q0.y) < 0) { firstDesc = t; break; }
    }
    std::vector<int> landTicks;
    if (firstDesc >= 0) {
        for (int j = 0; j < 3; ++j) landTicks.push_back(lastLand - j * 14);
        for (int j = 1; j <= 3; ++j) landTicks.push_back(firstDesc + j * 10);
    }
    for (int L : landTicks) {
        if (added >= maxOptions) break;
        if (L <= fromTick + 6 || L - t0 < 0 || L - t0 >= int(base.trace.size()) || L > lastLand) continue;
        const TraceSample& q = base.trace[size_t(L - t0)];
        if (q.onGround || !(groundMode(q.mode) || surfaceMode(q.mode))) continue;
        // descending (towards the gravity floor) at L
        const TraceSample& q0 = base.trace[size_t(std::max(0, L - t0 - 2))];
        if (q.g * (q.y - q0.y) >= 0) continue;
        const float feet = q.y - float(q.g) * q.half;
        PathSurface sf;
        sf.g = q.g;
        sf.click = base.info.click;
        if (q.g > 0) {
            int top = int(std::floor((feet + 4) / kCell));
            if (top < 1 || top * kCell > 420) continue;
            sf.cy0 = 0;
            sf.cy1 = top - 1;
        } else {
            int bottom = int(std::ceil((feet - 4) / kCell));
            if (bottom < 3 || bottom * kCell > 480) continue;
            sf.cy0 = bottom;
            sf.cy1 = std::max(bottom, kTopRow);
        }
        const float ups = float(phys::speedParams(m_start.p[0].speed).unitsPerStep());
        sf.cx0 = int(std::floor((q.x - 40) / kCell));
        sf.cx1 = int(std::floor((q.x + float(Tn - L) * ups + 45) / kCell));
        // the platform must not stand in the way of the path before the landing (including the
        // end of the previous stretch)
        bool blocked = false;
        auto cuts = [&](const TraceSample& r) {
            if (r.x + r.half <= sf.cx0 * kCell || r.x - r.half >= (sf.cx1 + 1) * kCell) return false;
            return r.y + r.half > sf.cy0 * kCell + 1 && r.y - r.half < (sf.cy1 + 1) * kCell - 1;
        };
        if (m_node) for (auto& r : m_node->tail) if (cuts(r)) { blocked = true; break; }
        for (int t = 0; t < L - t0 && !blocked; ++t) if (cuts(base.trace[size_t(t)])) blocked = true;
        if (blocked) continue;
        if (std::find(seen.begin(), seen.end(), std::make_pair(sf.cx0, q.g > 0 ? sf.cy1 : sf.cy0)) != seen.end()) continue;
        seen.push_back({sf.cx0, q.g > 0 ? sf.cy1 : sf.cy0});
        Cand c = base;
        c.surfaces.push_back({newGid(), sf});
        out.push_back(std::move(c));
        ++added;
    }
    return added > 0;
}

// What keeps the player going from the click to the next one (including the portals of the
// next segment).
void Planner::bridges(Cand base, int k, int T, int Tn, std::vector<Cand>& out) {
    const int until = Tn - 1;
    if (!evaluate(base, until)) return;
    const int n = int(m_plan.clicks.size());
    const int t0 = m_start.tick + 1;
    auto at = [&](const Cand& c, int tick) -> const TraceSample& { return c.trace[size_t(std::clamp(tick - t0, 0, int(c.trace.size()) - 1))]; };
    // ---- the next click belongs to a segment the player is not in yet: its portals. When they do
    // not fit into this gap, the player stays in its mode for now (scored down) and the switch
    // is tried again in the next gap.
    const auto& cur = base.end.p[0];
    const PathSegment* want = k + 1 < n ? &m_segs[size_t(m_segOf[size_t(k + 1)])] : nullptr;
    const bool switchWanted = want && (cur.mode != want->mode || int(cur.speed) != want->speed || cur.mini != want->mini ||
                                       (m_segOf[size_t(k + 1)] != m_segOf[size_t(k)] && cur.upsideDown != want->flipped));
    if (switchWanted) {
        const PathSegment& sg = *want;
        const int lead = std::max(30, int(0.22 * phys::kTPS));
        const int P = std::clamp(Tn - lead, T + 12, std::max(T + 12, Tn - 18));
        if (P < until && Tn - T >= int(0.2 * phys::kTPS)) {
            Cand c = base;
            const TraceSample& q = at(base, P);
            auto portalY = [&](float px) {
                for (auto& r : base.trace) if (r.x >= px) return std::round(r.y);
                return std::round(q.y);
            };
            // speed, gamemode, size, gravity: one after the other, all ahead of the player
            float px = std::round(q.x / 15.0f) * 15.0f;
            if (sg.speed != int(cur.speed)) { addObj(c, make(speedPortalId(sg.speed), px, portalY(px)), Act::ModePortal, k); px += 45; }
            addObj(c, make(portalIdFor(sg.mode), px, portalY(px)), Act::ModePortal, k);
            if (sg.mini != cur.mini) { px += 45; addObj(c, make(sg.mini ? oid::PortalSizeMini : oid::PortalSizeNormal, px, portalY(px)), Act::ModePortal, k); }
            if (sg.flipped != cur.upsideDown) { px += 45; addObj(c, make(sg.flipped ? oid::PortalGravFlip : oid::PortalGravNormal, px, portalY(px)), Act::ModePortal, k); }
            if (px <= at(base, Tn - 12).x && evaluate(c, until) && c.end.p[0].mode == sg.mode) {
                // after the portals the new mode may need a surface for its first click
                std::vector<Cand> landed;
                if (groundMode(sg.mode) && !c.end.p[0].onGround) addLandings(c, P + 20, Tn, landed, 2);
                for (auto& l : landed) if (evaluate(l, until) && l.end.p[0].onGround) out.push_back(std::move(l));
                out.push_back(std::move(c));
                return;
            }
        }
        base.score -= 1.5;
    }
    // ---- same segment
    const Mode m = base.end.p[0].mode;
    out.push_back(base); // free: whatever happens without new objects
    if (groundMode(m) || surfaceMode(m)) {
        std::vector<Cand> landed;
        addLandings(base, T + 6, Tn, landed, 4);
        for (auto& l : landed) {
            if (!evaluate(l, until) || !l.end.p[0].onGround) continue;
            // a pad on the platform for long gaps: bounce once more without a click
            if (groundMode(m) && Tn - T > int(0.55 * phys::kTPS)) {
                int landTick = -1;
                for (size_t i = 0; i < l.trace.size(); ++i) if (l.trace[i].onGround && int(i) + t0 > T + 6) { landTick = int(i) + t0; break; }
                if (landTick > 0 && Tn - landTick > int(0.32 * phys::kTPS)) {
                    const TraceSample& q = at(l, landTick);
                    const auto& sf = l.surfaces.back().second;
                    const float surfY = sf.g > 0 ? (sf.cy1 + 1) * kCell : sf.cy0 * kCell;
                    for (int id : {oid::PadYellow, oid::PadPink, oid::PadRed, oid::PadBlue}) {
                        Cand pc = l;
                        LevelObject o = make(id, std::round((q.x + 20) / 15.0f) * 15.0f, surfY + float(sf.g) * float(level::objectInfo(id).h) * 0.5f);
                        if (sf.g < 0) o.flipY = true;
                        if (objectsNear(pc, float(o.x), float(o.y), 40)) continue;
                        addObj(pc, o, Act::Pad, k);
                        if (!evaluate(pc, until)) continue;
                        if (!pc.end.p[0].onGround && groundMode(pc.end.p[0].mode)) {
                            std::vector<Cand> again;
                            addLandings(pc, landTick + 20, Tn, again, 1);
                            for (auto& a : again) if (evaluate(a, until) && a.end.p[0].onGround) out.push_back(std::move(a));
                        }
                        out.push_back(std::move(pc));
                    }
                }
            }
            out.push_back(std::move(l));
        }
        // a gravity portal in mid-air for long gaps
        if (m_opt.gravityFlips != Tri::Off && m_D >= 2.5f && Tn - T > int(0.5 * phys::kTPS)) {
            const int P = T + (Tn - T) / 2;
            const TraceSample& q = at(base, P);
            if (!q.onGround) {
                Cand gc = base;
                const bool up = base.end.p[0].upsideDown;
                addObj(gc, make(up ? oid::PortalGravNormal : oid::PortalGravFlip, std::round(q.x / 15.0f) * 15.0f, std::round(q.y)), Act::GravityPortal, k);
                if (evaluate(gc, until)) {
                    std::vector<Cand> again;
                    addLandings(gc, P + 10, Tn, again, 2);
                    for (auto& a : again) if (evaluate(a, until) && a.end.p[0].onGround) out.push_back(std::move(a));
                    out.push_back(std::move(gc));
                }
            }
        }
    }
}

// When nothing else survives the stretch: portals right after the click back to a plain cube
// (normal gravity, normal size), which then runs on the floor.
void Planner::rescue(Cand base, int k, int T, int Tn, std::vector<Cand>& out) {
    const int until = Tn - 1;
    evaluate(base, until);
    if (base.trace.empty()) return;
    const int t0 = m_start.tick + 1;
    for (int delay : {8, 16, 30}) {
        const int P = T + delay;
        if (P - t0 < 0 || P - t0 >= int(base.trace.size())) continue;
        const TraceSample& q = base.trace[size_t(P - t0)];
        Cand c = base;
        float px = std::round(q.x / 15.0f) * 15.0f + 15.0f;
        auto y = [&](float x) {
            for (auto& r : base.trace) if (r.x >= x) return std::round(r.y);
            return std::round(q.y);
        };
        if (q.mode != Mode::Cube) { addObj(c, make(oid::PortalCube, px, y(px)), Act::ModePortal, k); px += 30; }
        if (q.g < 0) { addObj(c, make(oid::PortalGravNormal, px, y(px)), Act::ModePortal, k); px += 30; }
        if (q.half < 14.0f) { addObj(c, make(oid::PortalSizeNormal, px, y(px)), Act::ModePortal, k); px += 30; }
        if (c.objs.size() == base.objs.size()) continue;
        if (!evaluate(c, until)) continue;
        c.score -= 2.0;
        out.push_back(std::move(c));
        return;
    }
}

double Planner::scoreCand(const Cand& c, int k, int T, int Tn) const {
    const Click& ck = m_plan.clicks[size_t(k)];
    double s = c.used ? 1.0 : -3.0;
    // action preferences (a hand-made style profile; replaced by the one learned from real levels)
    auto prior = [&](Act a, int sub) -> double {
        switch (a) {
            case Act::Orb:
                switch (sub) {
                    case oid::OrbYellow: return 1.0;
                    case oid::OrbPink: return 0.85;
                    case oid::OrbRed: return 0.6;
                    case oid::OrbBlue: return 0.75;
                    case oid::OrbGreen: return 0.75;
                    case oid::OrbBlack: return 0.45;
                    case oid::OrbDash: return 0.45;
                    default: return 0.5;
                }
            case Act::Jump: return 1.0;
            case Act::Tap: case Act::Hold: return 1.0;
            default: return 0.5;
        }
    };
    s += std::log(prior(c.act, c.sub));
    // the style learned from real levels: more or less often than the built-in preferences
    if (m_opt.profile && c.used) {
        const int mode = int(c.info.mode);
        std::string key = c.act == Act::Orb ? actionForObject(c.sub) : c.act == Act::Jump ? "jump" : c.act == Act::Hold ? "hold" : "tap";
        s += 0.7 * std::log(m_opt.profile->actionFactor(mode, key));
        for (auto& ob : c.objs) {
            if (ob.act == Act::Pad) s += 0.5 * std::log(m_opt.profile->actionFactor(mode, actionForObject(ob.o.id)));
            if (ob.act == Act::GravityPortal) s += 0.5 * std::log(m_opt.profile->actionFactor(mode, "gravity"));
        }
    }
    // variety
    const Node* n = m_node;
    if (n && n->used && n->act == c.act && n->sub == c.sub && c.act == Act::Orb) s -= 0.6;
    if (n && n->parent && n->parent->used && n->parent->act == c.act && n->parent->sub == c.sub && c.act == Act::Orb) s -= 0.3;
    // big actions on strong hits
    const bool big = (c.act == Act::Orb && (c.sub == oid::OrbRed || c.sub == oid::OrbBlack || c.sub == oid::OrbDash || c.sub == oid::OrbGreen || c.sub == oid::OrbBlue));
    if (big) s += 0.8 * (ck.strength - 0.45);
    // events between clicks
    int pads = 0, grav = 0, lands = 0;
    for (auto& ob : c.objs) { pads += ob.act == Act::Pad; grav += ob.act == Act::GravityPortal; }
    lands = int(c.surfaces.size());
    s += 0.25 * pads + 0.2 * grav;
    // height: stay on the screen, use the vertical space
    float hi = 0, lo = 1e9f;
    int ground = 0, boundContact = 0;
    for (auto& q : c.trace) {
        hi = std::max(hi, q.y);
        lo = std::min(lo, q.y);
        if (q.onGround && q.y < 20) ++ground;
    }
    const auto& e = c.end.p[0];
    if (!e.bounded && hi > kMaxY) s -= 0.02 * (hi - kMaxY);
    if (e.bounded && !c.trace.empty()) {
        for (auto& q : c.trace) if (q.y - q.half <= e.floorY + 1 || q.y + q.half >= e.ceilY - 1) ++boundContact;
        s -= (isFlying(e.mode) ? 3.0 : 1.2) * double(boundContact) / double(c.trace.size());
        const float mid = (e.floorY + e.ceilY) * 0.5f;
        s -= 0.4 * std::fabs(e.y - mid) / std::max(60.0f, e.ceilY - e.floorY);
    }
    if (!c.trace.empty()) s -= 0.5 * double(ground) / double(c.trace.size()) * (m_D >= 3 ? 1.0 : 0.3);
    s += 0.08 * std::min(4.0f, (hi - lo) / 30.0f);
    if (lands) s += 0.1;
    (void)T;
    (void)Tn;
    return s;
}

NodePtr Planner::finish(const NodePtr& n, int k, Cand& c) {
    auto node = std::make_shared<Node>();
    node->parent = n;
    node->click = k;
    node->s = c.end;
    node->objs = std::move(c.objs);
    node->surfaces = std::move(c.surfaces);
    node->inputs = std::move(c.inputs);
    node->info = c.info;
    node->used = c.used;
    node->act = c.act;
    node->sub = c.sub;
    node->endX = c.end.p[0].x;
    node->score = n->score + c.score;
    // tail: the path of this stretch and the end of the previous one, within 240 units
    for (auto& q : n->tail) if (q.x >= node->endX - 240) node->tail.push_back(q);
    for (auto& q : c.trace) if (q.x >= node->endX - 240) node->tail.push_back(q);
    return node;
}

void Planner::expand(const NodePtr& n, int k, std::vector<NodePtr>& out) {
    m_node = n.get();
    buildLocal(m_lw, n.get(), n->s.p[0].x, m_settings);
    m_start = n->s;
    fromGid(m_start, m_lw.simOf);
    const int nClicks = int(m_plan.clicks.size());
    const int T = std::max(m_start.tick + 1, tickOf(m_plan.clicks[size_t(k)].time));
    const int Tn = k + 1 < nClicks ? std::max(T + 4, tickOf(m_plan.clicks[size_t(k + 1)].time)) : tickOf(m_plan.duration) + 120;
    std::vector<Cand> opts;
    clickOptions(k, T, Tn, opts);
    // the click can also be left out (scored down; keeps the search alive)
    Cand skip;
    skip.used = false;
    skip.info.click = k;
    opts.push_back(skip);
    const size_t before = out.size();
    for (auto& base : opts) {
        std::vector<Cand> full;
        bridges(base, k, T, Tn, full);
        for (auto& c : full) {
            if (!c.alive) continue;
            // tie-break noise from the click's own stream (a re-rolled segment changes only its own)
            const int seg = m_segOf[size_t(k)];
            const uint64_t salt = size_t(seg) < m_prm.salts.size() ? m_prm.salts[size_t(seg)] : 0;
            Rng noise(combineSeed(m_noiseSeed ^ salt, uint64_t(k) * 4096 + uint64_t(out.size())));
            c.score += scoreCand(c, k, T, Tn) + noise.uniform(0.0, 0.05);
            out.push_back(finish(n, k, c));
        }
    }
    if (out.size() == before) {
        // nothing survives this stretch from this node: the rescue portals
        for (auto& base : opts) {
            std::vector<Cand> full;
            rescue(base, k, T, Tn, full);
            for (auto& c : full) {
                c.score += scoreCand(c, k, T, Tn);
                out.push_back(finish(n, k, c));
            }
        }
    }
}

NodePtr Planner::search(NodePtr start, int fromClick, PathPlan& out) {
    const int n = int(m_plan.clicks.size());
    std::vector<NodePtr> beam{std::move(start)};
    const int B = std::max(1, m_prm.beamWidth);
    for (int k = fromClick; k < n; ++k) {
        checkCancel(m_progress);
        if (m_progress && k % 16 == 0) m_progress->report("path", float(k) / float(std::max(1, n)));
        std::vector<NodePtr> next;
        for (auto& b : beam) expand(b, k, next);
        if (next.empty()) {
            // nothing survived: keep the beam and leave the click out
            out.notes.push_back("no way to continue at click " + std::to_string(k));
            if (std::getenv("AL_DEBUG_PATH")) {
                const auto& p = beam.front()->s.p[0];
                std::fprintf(stderr, "stuck at click %d (t %.2f): tick %d x %.0f y %.1f v %.2f mode %s g %d ground %d bounded %d [%.0f,%.0f] speed %d\n", k,
                             m_plan.clicks[size_t(k)].time, beam.front()->s.tick, p.x, p.y, p.v, level::modeName(p.mode), p.flip(), p.onGround, p.bounded, p.floorY, p.ceilY, int(p.speed));
            }
            continue;
        }
        std::sort(next.begin(), next.end(), [](const NodePtr& a, const NodePtr& b) { return a->score > b->score; });
        // diversity: drop near-identical states
        std::vector<NodePtr> kept;
        for (auto& c : next) {
            bool dup = false;
            for (auto& q : kept) {
                const auto& a = c->s.p[0];
                const auto& b2 = q->s.p[0];
                if (a.mode == b2.mode && a.upsideDown == b2.upsideDown && a.onGround == b2.onGround && std::fabs(a.y - b2.y) < 4 &&
                    std::fabs(a.v - b2.v) < 0.5 && c->act == q->act && c->sub == q->sub) { dup = true; break; }
            }
            if (!dup) kept.push_back(c);
            if (int(kept.size()) >= B) break;
        }
        beam = std::move(kept);
    }
    return beam.front();
}

void Planner::makeSegments(const std::vector<uint64_t>& salts) {
    const int n = int(m_plan.clicks.size());
    Rng segRng = m_rng.fork(hashStr("segments"));
    m_segs = planSegments(m_plan, m_A, m_opt, m_prm.switchRate, segRng, &salts);
    m_segOf.assign(size_t(n), 0);
    for (size_t s = 0; s < m_segs.size(); ++s)
        for (int i = m_segs[s].first; i <= m_segs[s].last && i < n; ++i) m_segOf[size_t(i)] = int(s);
}

PathPlan Planner::run() {
    PathPlan out;
    const int n = int(m_plan.clicks.size());
    m_D = m_plan.baseDifficulty;
    m_minRun = std::clamp(int(std::lround(16 - 1.2 * m_D)), 5, 16);
    m_noiseSeed = m_rng.fork(hashStr("noise")).next();
    const bool locked = m_prm.lockSegment > 0 && !m_prm.baseSalts.empty();
    const std::vector<uint64_t> wanted = m_prm.salts;
    if (locked) m_prm.salts = m_prm.baseSalts;
    makeSegments(m_prm.salts);
    // start settings = the first segment
    m_settings.startMode = m_segs.front().mode;
    m_settings.startSpeed = m_segs.front().speed;
    m_settings.startMini = m_segs.front().mini;
    m_settings.startFlipped = false;
    m_settings.fixGravityBug = true;
    out.start = m_settings;
    // root: the run from the level start to the first click
    phys::World empty;
    empty.setSettings(m_settings);
    empty.setEndX(1e9f);
    auto root = std::make_shared<Node>();
    root->s = phys::initialState(empty);
    {
        // lead-in: nothing to do but move (ground modes run on the floor)
        LocalWorld lw;
        lw.W = empty;
        const int T0 = n > 0 ? tickOf(m_plan.clicks[0].time) - 1 : tickOf(m_plan.duration);
        runLocal(lw.W, root->s, {}, T0, nullptr);
        toGid(root->s, lw.W);
        root->endX = root->s.p[0].x;
    }
    NodePtr best;
    if (locked && size_t(m_prm.lockSegment) < m_segs.size()) {
        // re-roll: the path up to the re-rolled segment is the one the old salts give, exactly
        const int firstClick = m_segs[size_t(m_prm.lockSegment)].first;
        PathPlan scratch;
        NodePtr old = search(root, 0, scratch);
        NodePtr from = root;
        for (NodePtr k = old; k; k = k->parent) if (k->click == firstClick - 1) { from = k; break; }
        m_prm.salts = wanted;
        makeSegments(m_prm.salts);
        best = search(from, firstClick, out);
    } else {
        best = search(root, 0, out);
    }
    out.segments = m_segs;
    // ---- the best chain
    std::vector<const Node*> chain;
    for (const Node* k = best.get(); k; k = k->parent.get()) chain.push_back(k);
    std::reverse(chain.begin(), chain.end());
    for (const Node* k : chain) {
        for (auto& ob : k->objs) out.objects.push_back({ob.o, ob.act, ob.click});
        for (auto& [gid, sf] : k->surfaces) out.surfaces.push_back(sf);
        for (auto& e : k->inputs) out.inputs.events.push_back(e);
        if (k->click >= 0) {
            if (k->used) out.clicks.push_back(k->info);
            else out.skipped.push_back(k->click);
        }
    }
    out.inputs.normalize();
    out.score = best->score;
    out.endTick = tickOf(m_plan.duration) + 120;
    // authoritative run over the whole path (one world with everything)
    level::Level L = pathLevel(out);
    phys::World W(L);
    W.setEndX(1e9f);
    phys::GameState s = phys::initialState(W);
    std::vector<bot::InputEvent> ev = out.inputs.events;
    runLocal(W, s, ev, out.endTick, &out.trace);
    if (s.dead) out.notes.push_back("the full path dies at tick " + std::to_string(s.tick));
    for (const Node* k : chain) {
        const int i = k->s.tick - 1;
        if (k->click < 0 || i < 0 || i >= int(out.trace.size())) continue;
        const auto& q = out.trace[size_t(i)];
        if (std::fabs(q.x - k->s.p[0].x) > 0.5f || std::fabs(q.y - k->s.p[0].y) > 0.5f) {
            if (std::getenv("AL_DEBUG_PATH")) {
                for (const Node* j : chain) {
                    if (j->click < 0 || j->click > k->click) continue;
                    std::fprintf(stderr, "node click %d act %s sub %d end tick %d planned (%.1f, %.1f v %.2f) full (%.1f, %.1f) inputs:", j->click, actName(j->act), j->sub, j->s.tick,
                                 j->s.p[0].x, j->s.p[0].y, j->s.p[0].v, out.trace[size_t(std::max(0, j->s.tick - 1))].x, out.trace[size_t(std::max(0, j->s.tick - 1))].y);
                    for (auto& e : j->inputs) std::fprintf(stderr, " %d%c", e.tick, e.press ? 'v' : '^');
                    std::fprintf(stderr, " objs %zu surf %zu\n", j->objs.size(), j->surfaces.size());
                }
            }
            out.notes.push_back("planning and the full run differ after click " + std::to_string(k->click) + " (tick " + std::to_string(k->s.tick) +
                                "): planned (" + std::to_string(int(k->s.p[0].x)) + ", " + std::to_string(int(k->s.p[0].y)) + ") full (" +
                                std::to_string(int(q.x)) + ", " + std::to_string(int(q.y)) + ")");
            break;
        }
    }
    return out;
}

} // namespace

level::Level pathLevel(const PathPlan& path, CellGrid* cells) {
    level::Level L;
    L.settings = path.start;
    for (auto& o : path.objects) L.objects.push_back(o.o);
    for (auto& s : path.surfaces) {
        if (cells) cells->fill(s.cx0, s.cy0, s.cx1, s.cy1);
        else forEachSurfaceBlock(s, 0, [&](LevelObject o, int) { L.objects.push_back(std::move(o)); });
    }
    return L;
}

PathPlan planPath(const DesignPlan& plan, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng, const PathParams& prm, const Progress* progress) {
    Planner p(plan, A, opt, rng, prm, progress);
    return p.run();
}

} // namespace al::gen
