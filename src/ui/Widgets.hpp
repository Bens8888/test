#pragma once
// Small reusable controls for the AutoLayout popup. Each control owns its callback (std::function)
// so the tabs can be written declaratively: label on the left, control on the right.
#include <Geode/Geode.hpp>

#include <functional>
#include <string>
#include <vector>

namespace al::mod::ui {

// "<  value  >" selector over a list of labels.
class CycleControl : public cocos2d::CCMenu {
public:
    static CycleControl* create(std::vector<std::string> labels, int index, float width, std::function<void(int)> onChange);
    void setIndex(int i, bool notify = false);
    int index() const { return m_index; }

private:
    bool init(std::vector<std::string> labels, int index, float width, std::function<void(int)> onChange);
    void onLeft(cocos2d::CCObject*);
    void onRight(cocos2d::CCObject*);
    std::vector<std::string> m_labels;
    int m_index = 0;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    float m_width = 120.f;
    std::function<void(int)> m_onChange;
};

// Slider 0..1 with a value label; `format` turns the value into text.
class SliderControl : public cocos2d::CCNode {
public:
    static SliderControl* create(float value, float width, std::function<std::string(float)> format, std::function<void(float)> onChange);
    void setValue(float v);
    float value() const { return m_value; }

private:
    bool init(float value, float width, std::function<std::string(float)> format, std::function<void(float)> onChange);
    void onSlide(cocos2d::CCObject*);
    Slider* m_slider = nullptr;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    float m_value = 0;
    std::function<std::string(float)> m_format;
    std::function<void(float)> m_onChange;
};

// Checkbox.
class ToggleControl : public cocos2d::CCMenu {
public:
    static ToggleControl* create(bool on, std::function<void(bool)> onChange);
    void setOn(bool on);
    bool on() const;

private:
    bool init(bool on, std::function<void(bool)> onChange);
    void onToggle(cocos2d::CCObject*);
    CCMenuItemToggler* m_toggler = nullptr;
    std::function<void(bool)> m_onChange;
};

// A button with a lambda.
CCMenuItemSpriteExtra* button(char const* text, std::function<void()> onClick, float scale = 0.6f, char const* bg = "GJ_button_01.png",
                              char const* font = "goldFont.fnt");

// A column of "label | control" rows laid out top to bottom inside `size`.
class Form : public cocos2d::CCNode {
public:
    static Form* create(cocos2d::CCSize size, float labelWidth = 120.f, float rowHeight = 22.f);
    void row(char const* label, cocos2d::CCNode* control, char const* help = nullptr);
    void heading(char const* text);
    void text(std::string const& text, float scale = 0.55f);
    float cursorY() const { return m_y; }
    float controlX() const { return m_labelWidth + 8.f; }

private:
    bool init(cocos2d::CCSize size, float labelWidth, float rowHeight);
    float m_labelWidth = 120.f, m_rowHeight = 22.f, m_y = 0;
};

cocos2d::CCLabelBMFont* label(std::string const& text, float scale = 0.5f, char const* font = "bigFont.fnt");

} // namespace al::mod::ui
