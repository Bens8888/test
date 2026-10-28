#include "AutoLayoutPopup.hpp"

#include "../bridge/EditorBridge.hpp"

#include <al/gen/test_pattern.hpp>

using namespace geode::prelude;

namespace al::mod {

static AutoLayoutPopup* s_current = nullptr;

static constexpr float kWidth = 460.f;
static constexpr float kHeight = 290.f;

static char const* tabName(int i) {
    static char const* names[] = {"Generate", "Style", "Modes", "Sections", "Playtest", "Training", "Settings"};
    return names[i];
}

AutoLayoutPopup* AutoLayoutPopup::create(LevelEditorLayer* lel) {
    auto ret = new AutoLayoutPopup();
    if (ret->init(lel)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

AutoLayoutPopup* AutoLayoutPopup::current() { return s_current; }

bool AutoLayoutPopup::init(LevelEditorLayer* lel) {
    if (!Popup::init(kWidth, kHeight)) return false;
    m_editor = lel;
    s_current = this;
    this->setTitle("AutoLayout");

    // tab row
    auto tabMenu = CCMenu::create();
    tabMenu->setContentSize({kWidth - 20.f, 30.f});
    const int n = int(Tab::Count);
    const float tabW = (kWidth - 30.f) / n;
    for (int i = 0; i < n; ++i) {
        auto btn = TabButton::create(tabName(i), this, menu_selector(AutoLayoutPopup::onTab));
        btn->setTag(i);
        btn->setScale(std::min(1.f, tabW / std::max(1.f, btn->getContentWidth())));
        btn->setPosition({tabW * (i + 0.5f), 15.f});
        tabMenu->addChild(btn);
        m_tabButtons[size_t(i)] = btn;
    }
    m_mainLayer->addChildAtPosition(tabMenu, Anchor::Top, {-(kWidth - 30.f) / 2.f, -62.f});

    m_contentSize = CCSize(kWidth - 30.f, kHeight - 95.f);
    for (int i = 0; i < n; ++i) {
        CCNode* page = nullptr;
        switch (Tab(i)) {
            case Tab::Generate: page = buildGenerateTab(); break;
            case Tab::Style: page = buildInfoTab("Style presets and the style model arrive with the generator milestone."); break;
            case Tab::Gamemodes: page = buildInfoTab("Gamemode weights arrive with the generator milestone."); break;
            case Tab::Sections: page = buildInfoTab("Per-section difficulty arrives with the generator milestone."); break;
            case Tab::Playtest: page = buildInfoTab("Bot playtest report and ghost playback arrive with the bot milestone."); break;
            case Tab::Training: page = buildInfoTab("Dataset collection and retraining arrive with the training milestone."); break;
            case Tab::Settings: page = buildInfoTab("Settings are in Geode's mod settings for now."); break;
            default: break;
        }
        page->setContentSize(m_contentSize);
        page->setAnchorPoint({0.5f, 0.5f});
        m_mainLayer->addChildAtPosition(page, Anchor::Center, {0.f, -22.f});
        m_tabPages[size_t(i)] = page;
    }
    selectTab(Tab::Generate);
    return true;
}

void AutoLayoutPopup::onClose(CCObject* sender) {
    if (s_current == this) s_current = nullptr;
    Popup::onClose(sender);
}

void AutoLayoutPopup::onTab(CCObject* sender) {
    auto node = static_cast<CCNode*>(sender);
    selectTab(Tab(node->getTag()));
}

void AutoLayoutPopup::selectTab(Tab tab) {
    for (int i = 0; i < int(Tab::Count); ++i) {
        bool on = i == int(tab);
        if (m_tabButtons[size_t(i)]) {
            m_tabButtons[size_t(i)]->toggle(on);
            m_tabButtons[size_t(i)]->setEnabled(!on);
        }
        if (m_tabPages[size_t(i)]) m_tabPages[size_t(i)]->setVisible(on);
    }
}

CCNode* AutoLayoutPopup::buildInfoTab(char const* text) {
    auto page = CCNode::create();
    page->setContentSize(m_contentSize);
    auto label = CCLabelBMFont::create(text, "chatFont.fnt", m_contentSize.width - 40.f, kCCTextAlignmentCenter);
    label->setScale(0.8f);
    page->addChildAtPosition(label, Anchor::Center);
    return page;
}

CCNode* AutoLayoutPopup::buildGenerateTab() {
    auto page = CCNode::create();
    page->setContentSize(m_contentSize);
    auto menu = CCMenu::create();
    menu->setContentSize(m_contentSize);
    menu->ignoreAnchorPointForPosition(false);

    auto place = CCMenuItemSpriteExtra::create(ButtonSprite::create("Place test pattern", "goldFont.fnt", "GJ_button_01.png", 0.8f), this,
                                               menu_selector(AutoLayoutPopup::onPlaceTestPattern));
    place->setPosition({m_contentSize.width / 2.f, m_contentSize.height / 2.f + 30.f});
    menu->addChild(place);

    m_undoButton = CCMenuItemSpriteExtra::create(ButtonSprite::create("Undo AutoLayout", "goldFont.fnt", "GJ_button_06.png", 0.7f), this,
                                                 menu_selector(AutoLayoutPopup::onUndo));
    m_undoButton->setPosition({m_contentSize.width / 2.f, m_contentSize.height / 2.f - 15.f});
    menu->addChild(m_undoButton);
    page->addChildAtPosition(menu, Anchor::Center);

    m_status = CCLabelBMFont::create("Ready.", "chatFont.fnt");
    m_status->setScale(0.75f);
    page->addChildAtPosition(m_status, Anchor::Bottom, {0.f, 12.f});
    refreshUndoButton();
    return page;
}

void AutoLayoutPopup::setStatus(std::string const& text) {
    if (m_status) m_status->setString(text.c_str());
}

void AutoLayoutPopup::refreshUndoButton() {
    if (!m_undoButton) return;
    bool any = EditorBridge::get().historySize() > 0;
    m_undoButton->setEnabled(any);
    m_undoButton->setOpacity(any ? 255 : 110);
}

void AutoLayoutPopup::onPlaceTestPattern(CCObject*) {
    auto& bridge = EditorBridge::get();
    float x0 = std::max(0.f, bridge.lastObjectX(m_editor)) + 300.f;
    x0 = std::floor(x0 / 30.f) * 30.f;
    auto res = bridge.apply(m_editor, gen::makeTestPattern(x0), "test pattern");
    if (res.isErr()) {
        setStatus("Error: " + res.unwrapErr());
        Notification::create(res.unwrapErr(), NotificationIcon::Error)->show();
    } else {
        setStatus(fmt::format("Placed {} objects at x = {:.0f} (editor layer {}).", res.unwrap(), x0, bridge.freeEditorLayer(m_editor) - 1));
    }
    refreshUndoButton();
}

void AutoLayoutPopup::onUndo(CCObject*) {
    auto& bridge = EditorBridge::get();
    std::string label = bridge.lastLabel();
    if (bridge.undoLast(m_editor)) setStatus("Removed: " + label);
    else setStatus("Nothing to undo.");
    refreshUndoButton();
}

} // namespace al::mod
