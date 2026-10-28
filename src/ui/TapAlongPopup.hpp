#pragma once
// The tap-along window, laid out like GD's Guideline Creator: how it works, Record/Stop, Clear,
// Undo take, Start Offset (seconds). While recording, any click or touch on the window (or Space,
// Up, W) is a tap; holding is a hold.
#include <Geode/Geode.hpp>

#include <functional>

namespace al::mod {

class TapAlongPopup : public geode::Popup {
public:
    // `onChanged` runs when the track changed (to refresh the caller's labels).
    static TapAlongPopup* create(LevelEditorLayer* lel, std::function<void()> onChanged);

protected:
    bool init(LevelEditorLayer* lel, std::function<void()> onChanged);
    void onClose(cocos2d::CCObject* sender) override;
    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override;
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override;
    void ccTouchCancelled(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override;
    void keyDown(cocos2d::enumKeyCodes key, double timestamp) override;
    void keyUp(cocos2d::enumKeyCodes key, double timestamp) override;

    void onRecord();
    void onTick(float dt);
    void refresh();
    void flash();

    LevelEditorLayer* m_editor = nullptr;
    std::function<void()> m_onChanged;
    geode::TextInput* m_offsetInput = nullptr;
    cocos2d::CCLabelBMFont* m_info = nullptr;
    cocos2d::CCLabelBMFont* m_time = nullptr;
    cocos2d::CCDrawNode* m_circle = nullptr;
    CCMenuItemSpriteExtra* m_recordBtn = nullptr;
    cocos2d::CCNode* m_idleItems = nullptr;
    double m_startOffset = 0;
    bool m_touchTap = false;
};

} // namespace al::mod
