// Editor entry points: the AutoLayout button in the editor's top-right menu, and the keybind.
#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

#include "../bridge/EditorBridge.hpp"
#include "../ui/AutoLayoutPopup.hpp"

using namespace geode::prelude;

class $modify(ALEditorUI, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;
        auto menu = this->getChildByID("settings-menu");
        if (!menu) {
            log::warn("AutoLayout: settings-menu not found, button not added");
            return true;
        }
        auto spr = CircleButtonSprite::createWithSpriteFrameName("GJ_musicOnBtn_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::Small);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ALEditorUI::onAutoLayout));
        btn->setID("autolayout-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
        return true;
    }

    void onAutoLayout(CCObject*) {
        if (al::mod::AutoLayoutPopup::current()) return;
        if (auto popup = al::mod::AutoLayoutPopup::create(m_editorLayer)) popup->show();
    }
};

class $modify(ALLevelEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        // AutoLayout's undo history is per editor session.
        al::mod::EditorBridge::get().reset();
        return LevelEditorLayer::init(level, noUI);
    }
};

$on_mod(Loaded) {
    listenForKeybindSettingPresses("open-autolayout", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;
        auto lel = LevelEditorLayer::get();
        if (!lel || al::mod::AutoLayoutPopup::current()) return;
        if (auto popup = al::mod::AutoLayoutPopup::create(lel)) popup->show();
    });
}
