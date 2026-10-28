#include "Widgets.hpp"

using namespace geode::prelude;

namespace al::mod::ui {

CCLabelBMFont* label(std::string const& text, float scale, char const* font) {
    auto l = CCLabelBMFont::create(text.c_str(), font);
    l->setScale(scale);
    return l;
}

// ---------------------------------------------------------------- cycle

CycleControl* CycleControl::create(std::vector<std::string> labels, int index, float width, std::function<void(int)> onChange) {
    auto r = new CycleControl();
    if (r->init(std::move(labels), index, width, std::move(onChange))) {
        r->autorelease();
        return r;
    }
    delete r;
    return nullptr;
}

bool CycleControl::init(std::vector<std::string> labels, int index, float width, std::function<void(int)> onChange) {
    if (!CCMenu::init()) return false;
    m_labels = std::move(labels);
    if (m_labels.empty()) m_labels.push_back("-");
    m_index = std::clamp(index, 0, int(m_labels.size()) - 1);
    m_width = width;
    m_onChange = std::move(onChange);
    this->setContentSize({width, 20.f});
    this->ignoreAnchorPointForPosition(false);
    this->setAnchorPoint({0.f, 0.5f});
    auto mk = [&](bool right) {
        auto spr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        spr->setScale(0.35f);
        if (right) spr->setFlipX(true);
        auto b = CCMenuItemSpriteExtra::create(spr, this, right ? menu_selector(CycleControl::onRight) : menu_selector(CycleControl::onLeft));
        b->setPosition({right ? width - 8.f : 8.f, 10.f});
        this->addChild(b);
    };
    mk(false);
    mk(true);
    m_label = CCLabelBMFont::create(m_labels[size_t(m_index)].c_str(), "bigFont.fnt");
    m_label->setPosition({width / 2.f, 10.f});
    this->addChild(m_label);
    setIndex(m_index);
    return true;
}

void CycleControl::setIndex(int i, bool notify) {
    int n = int(m_labels.size());
    m_index = ((i % n) + n) % n;
    m_label->setString(m_labels[size_t(m_index)].c_str());
    m_label->setScale(std::min(0.4f, (m_width - 34.f) / std::max(1.f, m_label->getContentWidth())));
    if (notify && m_onChange) m_onChange(m_index);
}

void CycleControl::onLeft(CCObject*) { setIndex(m_index - 1, true); }
void CycleControl::onRight(CCObject*) { setIndex(m_index + 1, true); }

// ---------------------------------------------------------------- slider

SliderControl* SliderControl::create(float value, float width, std::function<std::string(float)> format, std::function<void(float)> onChange) {
    auto r = new SliderControl();
    if (r->init(value, width, std::move(format), std::move(onChange))) {
        r->autorelease();
        return r;
    }
    delete r;
    return nullptr;
}

bool SliderControl::init(float value, float width, std::function<std::string(float)> format, std::function<void(float)> onChange) {
    if (!CCNode::init()) return false;
    m_format = std::move(format);
    m_onChange = std::move(onChange);
    this->setContentSize({width, 20.f});
    this->setAnchorPoint({0.f, 0.5f});
    m_slider = Slider::create(this, menu_selector(SliderControl::onSlide), 0.55f);
    // the GD slider is ~210 units wide at scale 1; centre it in the space left of the value label
    float sliderW = 210.f * 0.55f;
    m_slider->setPosition({sliderW / 2.f + 4.f, 10.f});
    this->addChild(m_slider);
    m_label = CCLabelBMFont::create("", "bigFont.fnt");
    m_label->setAnchorPoint({0.f, 0.5f});
    m_label->setPosition({sliderW + 14.f, 10.f});
    this->addChild(m_label);
    setValue(value);
    return true;
}

void SliderControl::setValue(float v) {
    m_value = std::clamp(v, 0.f, 1.f);
    m_slider->setValue(m_value);
    m_label->setString(m_format ? m_format(m_value).c_str() : fmt::format("{:.2f}", m_value).c_str());
    m_label->setScale(std::min(0.35f, 70.f / std::max(1.f, m_label->getContentWidth())));
}

void SliderControl::onSlide(CCObject*) {
    m_value = std::clamp(m_slider->getValue(), 0.f, 1.f);
    m_label->setString(m_format ? m_format(m_value).c_str() : fmt::format("{:.2f}", m_value).c_str());
    m_label->setScale(std::min(0.35f, 70.f / std::max(1.f, m_label->getContentWidth())));
    if (m_onChange) m_onChange(m_value);
}

// ---------------------------------------------------------------- toggle

ToggleControl* ToggleControl::create(bool on, std::function<void(bool)> onChange) {
    auto r = new ToggleControl();
    if (r->init(on, std::move(onChange))) {
        r->autorelease();
        return r;
    }
    delete r;
    return nullptr;
}

bool ToggleControl::init(bool on, std::function<void(bool)> onChange) {
    if (!CCMenu::init()) return false;
    m_onChange = std::move(onChange);
    this->setContentSize({20.f, 20.f});
    this->ignoreAnchorPointForPosition(false);
    this->setAnchorPoint({0.f, 0.5f});
    m_toggler = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(ToggleControl::onToggle), 0.5f);
    m_toggler->setPosition({10.f, 10.f});
    m_toggler->toggle(on);
    this->addChild(m_toggler);
    return true;
}

void ToggleControl::setOn(bool on) { m_toggler->toggle(on); }
bool ToggleControl::on() const { return m_toggler->isToggled(); }

void ToggleControl::onToggle(CCObject*) {
    // the toggler flips its state after this callback
    bool now = !m_toggler->isToggled();
    if (m_onChange) m_onChange(now);
}

// ---------------------------------------------------------------- button

CCMenuItemSpriteExtra* button(char const* text, std::function<void()> onClick, float scale, char const* bg, char const* font) {
    auto spr = ButtonSprite::create(text, font, bg, scale);
    return CCMenuItemExt::createSpriteExtra(spr, [cb = std::move(onClick)](CCMenuItemSpriteExtra*) {
        if (cb) cb();
    });
}

// ---------------------------------------------------------------- form

Form* Form::create(CCSize size, float labelWidth, float rowHeight) {
    auto r = new Form();
    if (r->init(size, labelWidth, rowHeight)) {
        r->autorelease();
        return r;
    }
    delete r;
    return nullptr;
}

bool Form::init(CCSize size, float labelWidth, float rowHeight) {
    if (!CCNode::init()) return false;
    this->setContentSize(size);
    m_labelWidth = labelWidth;
    m_rowHeight = rowHeight;
    m_y = size.height - rowHeight / 2.f;
    return true;
}

void Form::row(char const* text, CCNode* control, char const* help) {
    auto l = CCLabelBMFont::create(text, "bigFont.fnt");
    l->setAnchorPoint({0.f, 0.5f});
    l->setScale(std::min(0.38f, (m_labelWidth - 4.f) / std::max(1.f, l->getContentWidth())));
    l->setPosition({4.f, m_y});
    this->addChild(l);
    if (control) {
        control->setAnchorPoint({0.f, 0.5f});
        control->setPosition({controlX(), m_y});
        this->addChild(control);
    }
    if (help) {
        auto h = CCLabelBMFont::create(help, "chatFont.fnt");
        h->setAnchorPoint({1.f, 0.5f});
        h->setScale(0.5f);
        h->setOpacity(170);
        h->setPosition({this->getContentWidth() - 4.f, m_y});
        this->addChild(h);
    }
    m_y -= m_rowHeight;
}

void Form::heading(char const* text) {
    auto l = CCLabelBMFont::create(text, "goldFont.fnt");
    l->setAnchorPoint({0.f, 0.5f});
    l->setScale(0.5f);
    l->setPosition({4.f, m_y});
    this->addChild(l);
    m_y -= m_rowHeight * 0.9f;
}

void Form::text(std::string const& t, float scale) {
    auto l = CCLabelBMFont::create(t.c_str(), "chatFont.fnt", (this->getContentWidth() - 8.f) / scale, kCCTextAlignmentLeft);
    l->setAnchorPoint({0.f, 1.f});
    l->setScale(scale);
    l->setPosition({4.f, m_y + m_rowHeight / 2.f});
    this->addChild(l);
    m_y -= l->getContentHeight() * scale + 4.f;
}

} // namespace al::mod::ui
