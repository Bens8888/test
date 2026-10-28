#pragma once
#include <Geode/Geode.hpp>

#include <array>
#include <string>

namespace al::mod {

class AutoLayoutPopup : public geode::Popup {
public:
    static AutoLayoutPopup* create(LevelEditorLayer* lel);
    static AutoLayoutPopup* current();

protected:
    enum class Tab { Generate = 0, Style, Gamemodes, Sections, Playtest, Training, Settings, Count };

    bool init(LevelEditorLayer* lel);
    void onClose(cocos2d::CCObject* sender) override;

    void selectTab(Tab tab);
    void onTab(cocos2d::CCObject* sender);
    cocos2d::CCNode* buildGenerateTab();
    cocos2d::CCNode* buildInfoTab(char const* text);

    void onPlaceTestPattern(cocos2d::CCObject*);
    void onUndo(cocos2d::CCObject*);
    void setStatus(std::string const& text);
    void refreshUndoButton();

    LevelEditorLayer* m_editor = nullptr;
    std::array<geode::TabButton*, size_t(Tab::Count)> m_tabButtons{};
    std::array<cocos2d::CCNode*, size_t(Tab::Count)> m_tabPages{};
    cocos2d::CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_undoButton = nullptr;
    cocos2d::CCSize m_contentSize;
};

} // namespace al::mod
