#include "Ghost.hpp"

#include <al/physics/sim.hpp>

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
}

bool ghostVisible(LevelEditorLayer* lel) { return findGhost(lel) != nullptr; }

void playGhost(LevelEditorLayer* lel) {
    if (auto g = findGhost(lel)) g->play();
}

} // namespace al::mod::ui
