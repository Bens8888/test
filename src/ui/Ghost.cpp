#include "Ghost.hpp"

#include <al/gen/tiles.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace al::mod::ui {

namespace {

constexpr char const* kGhostID = "bens8888.autolayout/ghost-path";

class GhostNode : public CCNode {
public:
    static GhostNode* create() {
        auto r = new GhostNode();
        if (r->init()) {
            r->autorelease();
            return r;
        }
        delete r;
        return nullptr;
    }
    std::vector<CCPoint> path; // one point per tick
    CCSprite* icon = nullptr;
    double t = 0;

    void play() {
        if (path.empty()) return;
        if (!icon) {
            icon = CCSprite::createWithSpriteFrameName("player_01_001.png");
            if (!icon) return;
            icon->setScale(0.9f);
            icon->setOpacity(170);
            this->addChild(icon, 5);
        }
        t = 0;
        icon->setVisible(true);
        this->unschedule(schedule_selector(GhostNode::tick));
        this->schedule(schedule_selector(GhostNode::tick));
    }

    void tick(float dt) {
        t += dt;
        size_t k = size_t(t * phys::kTPS);
        if (!icon || k >= path.size()) {
            if (icon) icon->setVisible(false);
            this->unschedule(schedule_selector(GhostNode::tick));
            return;
        }
        icon->setPosition(path[k]);
    }
};

// One drawable piece of the layout for the build animation.
struct Piece {
    float x = 0;                 // reveal position
    bool path = false;           // orbs, pads, portals (appear first)
    level::LevelObject o;
};

ccColor4F rgba(uint32_t c, float a = 1.f) { return {float((c >> 24) & 255) / 255.f, float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, a}; }

void drawPiece(CCDrawNode* d, level::LevelObject const& o) {
    using level::Kind;
    namespace oid = level::id;
    const auto& info = o.info();
    phys::SimObject so;
    const bool sim = phys::World::makeSimObject(o, 0, so);
    const float x = float(o.x), y = float(o.y) + kEditorYOffset;
    const ccColor4F white{1.f, 1.f, 1.f, 1.f};
    switch (info.kind) {
        case Kind::Solid: case Kind::Breakable: {
            const uint8_t e = gen::drawnEdges(o);
            if (o.id == oid::OutlineTop) {
                const float hw = sim ? so.hw : 15.f, hh = sim ? std::max(so.hh, 0.75f) : 0.75f;
                d->drawSegment({x - hw + hh, y}, {x + hw - hh, y}, std::min(hw, hh), white);
                if (hh > hw) d->drawSegment({x, y - hh + hw}, {x, y + hh - hw}, hw, white);
            } else if (e) {
                if (e & gen::EdgeT) d->drawSegment({x - 15, y + 15}, {x + 15, y + 15}, 0.8f, white);
                if (e & gen::EdgeB) d->drawSegment({x - 15, y - 15}, {x + 15, y - 15}, 0.8f, white);
                if (e & gen::EdgeL) d->drawSegment({x - 15, y - 15}, {x - 15, y + 15}, 0.8f, white);
                if (e & gen::EdgeR) d->drawSegment({x + 15, y - 15}, {x + 15, y + 15}, 0.8f, white);
            } else if (sim) {
                CCPoint v[4] = {{so.x - so.hw, so.y - so.hh + kEditorYOffset}, {so.x + so.hw, so.y - so.hh + kEditorYOffset},
                                {so.x + so.hw, so.y + so.hh + kEditorYOffset}, {so.x - so.hw, so.y + so.hh + kEditorYOffset}};
                d->drawPolygon(v, 4, {0.05f, 0.07f, 0.2f, 0.9f}, 0.8f, white);
            }
            break;
        }
        case Kind::Slope: {
            const bool rising = so.slopeOrient == 0 || so.slopeOrient == 2;
            const float hw = so.hw, hh = so.hh; // 45-degree or 2-wide
            if (rising) d->drawSegment({x - hw, y - hh}, {x + hw, y + hh}, 0.8f, white);
            else d->drawSegment({x - hw, y + hh}, {x + hw, y - hh}, 0.8f, white);
            break;
        }
        case Kind::Hazard: {
            // up (0), right (90), down (180), left (270); cocos y points up
            int q = int(std::lround(std::fmod(o.rotation + 360.0, 360.0) / 90.0)) % 4;
            if (o.flipY) q = (q + 2) % 4;
            static const float dir[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
            const float ux = dir[q][0], uy = dir[q][1];
            CCPoint v[3] = {{x - ux * 15 - uy * 14, y - uy * 15 + ux * 14}, {x - ux * 15 + uy * 14, y - uy * 15 - ux * 14}, {x + ux * 15, y + uy * 15}};
            d->drawPolygon(v, 3, {0.04f, 0.04f, 0.15f, 1.f}, 0.8f, white);
            break;
        }
        case Kind::Saw:
            d->drawDot({x, y}, so.hw * 1.1f, {0.02f, 0.04f, 0.2f, 1.f});
            d->drawDot({x, y}, so.hw * 0.5f, {0.1f, 0.25f, 0.6f, 1.f});
            break;
        case Kind::Orb: {
            uint32_t c = 0xffe040ff;
            switch (o.id) {
                case oid::OrbPink: c = 0xff60e0ff; break;
                case oid::OrbRed: c = 0xff5020ff; break;
                case oid::OrbBlue: c = 0x40e8ffff; break;
                case oid::OrbGreen: c = 0x40ff50ff; break;
                case oid::OrbBlack: c = 0x181818ff; break;
                case oid::OrbDash: c = 0x60ff90ff; break;
                default: break;
            }
            d->drawDot({x, y}, 15.f, white);
            d->drawDot({x, y}, 12.f, rgba(c));
            break;
        }
        case Kind::Pad: {
            uint32_t c = o.id == oid::PadPink ? 0xff60e0ff : o.id == oid::PadRed ? 0xff5020ff : o.id == oid::PadBlue ? 0x40e8ffff : 0xffe040ff;
            d->drawSegment({x - 12, y}, {x + 12, y}, 3.f, rgba(c));
            break;
        }
        case Kind::ModePortal: case Kind::GravityPortal: case Kind::SizePortal: case Kind::SpeedPortal: case Kind::DualPortal: {
            uint32_t c = info.kind == Kind::ModePortal ? 0x40ff90ff : info.kind == Kind::SpeedPortal ? 0xffb030ff : 0x40c0ffff;
            const float hh = sim ? so.hh : 40.f;
            d->drawSegment({x, y - hh}, {x, y + hh}, 5.f, rgba(c, 0.9f));
            break;
        }
        default: break;
    }
}

class BuildNode : public CCNode {
public:
    static BuildNode* create() {
        auto r = new BuildNode();
        if (r->init()) {
            r->autorelease();
            return r;
        }
        delete r;
        return nullptr;
    }
    LevelEditorLayer* editor = nullptr;
    std::vector<CCPoint> path;
    std::vector<Piece> pieces;   // sorted by x
    size_t nextPath = 0, nextStruct = 0;
    std::vector<size_t> pathIdx, structIdx;
    CCDrawNode* layout = nullptr;
    CCDrawNode* trail = nullptr;
    CCSprite* icon = nullptr;
    double t = 0;
    size_t lastK = 0;

    void start() {
        layout = CCDrawNode::create();
        trail = CCDrawNode::create();
        this->addChild(layout, 1);
        this->addChild(trail, 2);
        icon = CCSprite::createWithSpriteFrameName("player_01_001.png");
        if (icon) {
            icon->setScale(0.9f);
            this->addChild(icon, 5);
        }
        for (size_t i = 0; i < pieces.size(); ++i) (pieces[i].path ? pathIdx : structIdx).push_back(i);
        this->schedule(schedule_selector(BuildNode::tick));
    }

    void tick(float dt) {
        t += dt;
        size_t k = size_t(t * phys::kTPS);
        if (k >= path.size()) {
            // the rest of the layout, then stop
            reveal(1e9f, 1e9f);
            if (icon) icon->setVisible(false);
            this->unschedule(schedule_selector(BuildNode::tick));
            return;
        }
        const CCPoint p = path[k];
        if (icon) icon->setPosition(p);
        for (size_t j = lastK + 1; j <= k; j += 2) trail->drawSegment(path[j - 1], path[j], 1.2f, {1.f, 1.f, 1.f, 0.75f});
        lastK = k;
        // the gameplay objects a little ahead, the structures closer (built around the path)
        reveal(p.x + 420.f, p.x + 260.f);
        // the camera follows the icon
        if (editor && editor->m_objectLayer) {
            const float scale = editor->m_objectLayer->getScale();
            const auto win = CCDirector::sharedDirector()->getWinSize();
            editor->m_objectLayer->setPositionX(win.width * 0.35f - p.x * scale);
        }
    }

    void reveal(float pathX, float structX) {
        while (nextPath < pathIdx.size() && pieces[pathIdx[nextPath]].x <= pathX) drawPiece(layout, pieces[pathIdx[nextPath++]].o);
        while (nextStruct < structIdx.size() && pieces[structIdx[nextStruct]].x <= structX) drawPiece(layout, pieces[structIdx[nextStruct++]].o);
    }
};

GhostNode* findGhost(LevelEditorLayer* lel) {
    if (!lel || !lel->m_objectLayer) return nullptr;
    return static_cast<GhostNode*>(lel->m_objectLayer->getChildByID(kGhostID));
}

} // namespace

bool showGhost(LevelEditorLayer* lel, level::Level const& lvl, bot::InputTrack const& inputs, float xOffset) {
    if (!lel || !lel->m_objectLayer) return false;
    hideGhost(lel);
    phys::World W(lvl);
    bot::RunOptions ro;
    ro.recordTrace = true;
    auto run = bot::run(W, inputs, ro);
    if (run.trace.empty()) return false;
    auto node = GhostNode::create();
    node->setID(kGhostID);
    auto draw = CCDrawNode::create();
    node->addChild(draw);
    node->path.reserve(run.trace.size());
    for (auto& p : run.trace) node->path.push_back({p.x + xOffset, p.y + kEditorYOffset});
    // the path, thicker and green while the button is held
    size_t ei = 0;
    bool held = false;
    for (size_t k = 1; k < run.trace.size(); k += 2) {
        int tick = int(k) + 1;
        while (ei < inputs.events.size() && inputs.events[ei].tick <= tick) {
            held = inputs.events[ei].press;
            if (held) draw->drawDot(node->path[k], 4.f, {0.2f, 1.f, 0.5f, 0.9f});
            ++ei;
        }
        auto col = held ? ccColor4F{0.35f, 1.f, 0.55f, 0.85f} : ccColor4F{1.f, 1.f, 1.f, 0.6f};
        draw->drawSegment(node->path[k - 1 < 2 ? 0 : k - 2], node->path[k], held ? 1.6f : 1.0f, col);
    }
    if (run.dead) draw->drawDot({run.x + xOffset, run.y + kEditorYOffset}, 7.f, {1.f, 0.1f, 0.1f, 1.f});
    lel->m_objectLayer->addChild(node, 9999);
    return true;
}

void hideGhost(LevelEditorLayer* lel) {
    if (auto g = findGhost(lel)) g->removeFromParent();
    if (lel && lel->m_objectLayer)
        if (auto b = lel->m_objectLayer->getChildByID("bens8888.autolayout/build-preview")) b->removeFromParent();
}

bool watchBuild(LevelEditorLayer* lel, level::Level const& lvl, bot::InputTrack const& inputs) {
    if (!lel || !lel->m_objectLayer) return false;
    hideGhost(lel);
    phys::World W(lvl);
    bot::RunOptions ro;
    ro.recordTrace = true;
    auto run = bot::run(W, inputs, ro);
    if (run.trace.empty()) return false;
    auto node = BuildNode::create();
    node->setID("bens8888.autolayout/build-preview");
    node->editor = lel;
    node->path.reserve(run.trace.size());
    for (auto& p : run.trace) node->path.push_back({p.x, p.y + kEditorYOffset});
    for (auto& o : lvl.objects) {
        const auto k = o.info().kind;
        if (k == level::Kind::Trigger || k == level::Kind::EndTrigger || k == level::Kind::Deco) continue;
        Piece pc;
        pc.x = float(o.x);
        pc.path = k == level::Kind::Orb || k == level::Kind::Pad || k == level::Kind::ModePortal || k == level::Kind::GravityPortal ||
                  k == level::Kind::SpeedPortal || k == level::Kind::SizePortal || k == level::Kind::DualPortal;
        pc.o = o;
        node->pieces.push_back(std::move(pc));
    }
    std::stable_sort(node->pieces.begin(), node->pieces.end(), [](const Piece& a, const Piece& b) { return a.x < b.x; });
    lel->m_objectLayer->addChild(node, 9999);
    node->start();
    return true;
}

bool ghostVisible(LevelEditorLayer* lel) { return findGhost(lel) != nullptr; }

void playGhost(LevelEditorLayer* lel) {
    if (auto g = findGhost(lel)) g->play();
}

} // namespace al::mod::ui
