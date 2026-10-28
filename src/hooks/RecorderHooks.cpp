// Click recorder hooks: inputs (handleButton, once per processed button at its physics step),
// orbs (ringJump), practice checkpoints and respawns, deaths, completion, editor playtests, the
// pause-menu toggle, the hotkey and the on-screen REC indicator.
#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include "../recorder/LiveRecorder.hpp"

using namespace geode::prelude;
using al::mod::LiveRecorder;

namespace {

constexpr int kIndicatorTag = 0x41ec;

void updateIndicator(CCNode* parent) {
    if (!parent) return;
    auto label = static_cast<CCLabelBMFont*>(parent->getChildByTag(kIndicatorTag));
    auto& r = LiveRecorder::get();
    bool show = r.recording() || r.armed();
    if (!show) {
        if (label) label->removeFromParent();
        return;
    }
    if (!label) {
        label = CCLabelBMFont::create("REC", "bigFont.fnt");
        label->setTag(kIndicatorTag);
        label->setID("recording-indicator"_spr);
        label->setScale(0.45f);
        label->setAnchorPoint({0.f, 1.f});
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({8.f, win.height - 22.f});
        label->setZOrder(1000);
        parent->addChild(label);
    }
    // red while recording, dim while armed (waiting for the next attempt)
    label->setColor(r.recording() ? ccColor3B{255, 60, 60} : ccColor3B{150, 150, 150});
    label->setString(r.recording() ? "REC" : "REC (next attempt)");
}

} // namespace

class $modify(ALRecBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        LiveRecorder::get().onButton(this, down, button, isPlayer1);
    }
};

class $modify(ALRecPlayerObject, PlayerObject) {
    void ringJump(RingObject* ring, bool skipCheck) {
        PlayerObject::ringJump(ring, skipCheck);
        if (ring && LiveRecorder::get().recording()) LiveRecorder::get().onOrb(GJBaseGameLayer::get(), ring->m_objectID);
    }
};

class $modify(ALRecPlayLayer, PlayLayer) {
    struct Fields {
        bool loadingCheckpoint = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        LiveRecorder::get().beginSession(this, level, false);
        updateIndicator(this);
        return true;
    }

    void resetLevel() {
        m_fields->loadingCheckpoint = false;
        PlayLayer::resetLevel();
        auto& r = LiveRecorder::get();
        if (!r.recording()) {
            if (r.armed()) r.beginSession(this, m_level, false); // armed during an attempt: starts now
        } else if (!m_fields->loadingCheckpoint) {
            r.onRestart(); // a full restart (not a practice respawn)
        }
        updateIndicator(this);
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        m_fields->loadingCheckpoint = true;
        int32_t tick = cp ? int32_t(cp->m_gameState.m_currentProgress) : LiveRecorder::tickOf(this);
        LiveRecorder::get().onRespawn(this, tick);
    }

    void storeCheckpoint(CheckpointObject* cp) {
        PlayLayer::storeCheckpoint(cp);
        if (cp) LiveRecorder::get().onCheckpoint(int32_t(cp->m_gameState.m_currentProgress));
    }

    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        // `first` drops the oldest one (checkpoint limit), which does not change the stitched path
        if (!first) LiveRecorder::get().onCheckpointRemoved();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool probe = object && object == m_anticheatSpike; // GD's anticheat probe, not a death
        PlayLayer::destroyPlayer(player, object);
        if (!probe && LiveRecorder::get().recording()) LiveRecorder::get().onDeath(LiveRecorder::tickOf(this));
    }

    void levelComplete() {
        LiveRecorder::get().onComplete(this);
        PlayLayer::levelComplete();
        updateIndicator(this);
    }

    void onQuit() {
        LiveRecorder::get().endSession(this, false);
        PlayLayer::onQuit();
    }
};

class $modify(ALRecEditorLayer, LevelEditorLayer) {
    void onPlaytest() {
        LevelEditorLayer::onPlaytest();
        auto& r = LiveRecorder::get();
        if (!r.recording()) r.beginSession(this, m_level, true);
        if (m_editorUI) updateIndicator(m_editorUI);
    }

    void onStopPlaytest() {
        LiveRecorder::get().endSession(this, true);
        LevelEditorLayer::onStopPlaytest();
        if (m_editorUI) updateIndicator(m_editorUI);
    }
};

class $modify(ALRecPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;
        auto btn = CCMenuItemSpriteExtra::create(makeSprite(), this, menu_selector(ALRecPauseLayer::onToggleRecord));
        btn->setID("autolayout-record-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    static CCNode* makeSprite() {
        bool on = LiveRecorder::get().armed();
        auto spr = ButtonSprite::create(on ? "Rec ON" : "Rec", 50, true, "bigFont.fnt", on ? "GJ_button_02.png" : "GJ_button_04.png", 30.f, 0.6f);
        spr->setScale(0.7f);
        return spr;
    }

    void onToggleRecord(CCObject* sender) {
        LiveRecorder::get().toggle();
        if (auto item = static_cast<CCMenuItemSpriteExtra*>(sender)) item->setNormalImage(makeSprite());
        if (auto pl = PlayLayer::get()) updateIndicator(pl);
    }
};

$on_mod(Loaded) {
    listenForKeybindSettingPresses("record-toggle", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        LiveRecorder::get().toggle();
        if (auto pl = PlayLayer::get()) updateIndicator(pl);
        else if (auto lel = LevelEditorLayer::get(); lel && lel->m_editorUI) updateIndicator(lel->m_editorUI);
    });
}
