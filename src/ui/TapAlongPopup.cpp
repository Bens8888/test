#include "TapAlongPopup.hpp"

#include "../recorder/TapAlong.hpp"
#include "../session/Session.hpp"
#include "Widgets.hpp"

using namespace geode::prelude;

namespace al::mod {

namespace {
constexpr float kW = 360.f, kH = 250.f;

bool isTapKey(enumKeyCodes key) {
    return key == enumKeyCodes::KEY_Space || key == enumKeyCodes::KEY_Up || key == enumKeyCodes::KEY_W;
}
} // namespace

TapAlongPopup* TapAlongPopup::create(LevelEditorLayer* lel, std::function<void()> onChanged) {
    auto ret = new TapAlongPopup();
    if (ret->init(lel, std::move(onChanged))) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool TapAlongPopup::init(LevelEditorLayer* lel, std::function<void()> onChanged) {
    if (!Popup::init(kW, kH)) return false;
    m_editor = lel;
    m_onChanged = std::move(onChanged);
    this->setTitle("Tap Along");
    TapAlong::get().track(lel);

    // ---- what to do (shown while not recording)
    m_idleItems = CCNode::create();
    m_idleItems->setContentSize({kW, kH});
    m_mainLayer->addChildAtPosition(m_idleItems, Anchor::BottomLeft, {0.f, 0.f}, false);
    const char* lines[] = {
        "- Press Record and tap along to the song (click, tap, Space or Up).",
        "- Each tap becomes a click, shown as a line in the editor.",
        "- Hold to make a hold. Takes are merged into one track;",
        "  re-tapping a spot corrects it.",
        "- Clear resets the track and the lines.",
    };
    float y = kH - 52.f;
    for (auto l : lines) {
        auto lab = ui::label(l, 0.36f, "chatFont.fnt");
        lab->setAnchorPoint({0.f, 0.5f});
        lab->setPosition({22.f, y});
        m_idleItems->addChild(lab);
        y -= 15.f;
    }
    // start offset (seconds of the level)
    auto offLabel = ui::label("Start Offset", 0.45f, "goldFont.fnt");
    offLabel->setPosition({kW / 2.f - 70.f, 92.f});
    m_idleItems->addChild(offLabel);
    m_offsetInput = TextInput::create(70.f, "0", "bigFont.fnt");
    m_offsetInput->setCommonFilter(CommonFilter::Float);
    m_offsetInput->setScale(0.7f);
    m_offsetInput->setPosition({kW / 2.f + 20.f, 92.f});
    m_offsetInput->setCallback([this](std::string const& s) {
        try { m_startOffset = s.empty() ? 0.0 : std::max(0.0, std::stod(s)); } catch (...) { m_startOffset = 0; }
    });
    m_idleItems->addChild(m_offsetInput);
    auto secLabel = ui::label("Seconds", 0.35f, "bigFont.fnt");
    secLabel->setOpacity(160);
    secLabel->setPosition({kW / 2.f + 82.f, 92.f});
    m_idleItems->addChild(secLabel);

    // ---- buttons
    auto menu = CCMenu::create();
    menu->setContentSize({kW, 50.f});
    menu->ignoreAnchorPointForPosition(false);
    m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 36.f});
    m_recordBtn = ui::button("Record", [this] { onRecord(); }, 0.7f, "GJ_button_01.png");
    m_recordBtn->setPosition({kW / 2.f, 25.f});
    menu->addChild(m_recordBtn);
    auto clearBtn = ui::button("Clear", [this] {
        if (TapAlong::get().recording()) return;
        TapAlong::get().clear(m_editor);
        refresh();
        if (m_onChanged) m_onChanged();
    }, 0.6f, "GJ_button_02.png");
    clearBtn->setPosition({kW / 2.f - 110.f, 25.f});
    menu->addChild(clearBtn);
    auto undoBtn = ui::button("Undo take", [this] {
        if (TapAlong::get().recording()) return;
        if (!TapAlong::get().undoTake(m_editor)) Notification::create("No take to undo", NotificationIcon::Info)->show();
        refresh();
        if (m_onChanged) m_onChanged();
    }, 0.5f, "GJ_button_04.png");
    undoBtn->setPosition({kW / 2.f + 115.f, 25.f});
    menu->addChild(undoBtn);

    // ---- status
    m_info = ui::label("", 0.34f, "bigFont.fnt");
    m_mainLayer->addChildAtPosition(m_info, Anchor::Bottom, {0.f, 72.f});
    m_time = ui::label("", 0.8f, "bigFont.fnt");
    m_mainLayer->addChildAtPosition(m_time, Anchor::Center, {0.f, 30.f});
    m_circle = CCDrawNode::create();
    m_circle->drawDot({0.f, 0.f}, 26.f, ccc4f(0.3f, 1.f, 0.4f, 1.f));
    m_circle->setVisible(false);
    m_mainLayer->addChildAtPosition(m_circle, Anchor::Center, {0.f, -18.f});

    this->schedule(schedule_selector(TapAlongPopup::onTick), 0.f);
    refresh();
    return true;
}

void TapAlongPopup::refresh() {
    auto& ta = TapAlong::get();
    const bool rec = ta.recording();
    m_idleItems->setVisible(!rec);
    m_time->setVisible(rec);
    m_recordBtn->setNormalImage(ButtonSprite::create(rec ? "Stop" : "Record", "goldFont.fnt", rec ? "GJ_button_06.png" : "GJ_button_01.png", 0.7f));
    auto& tr = ta.track(m_editor);
    std::string info = rec ? fmt::format("Recording: {} taps in this take", ta.takeSize())
                           : fmt::format("{} taps, {} take(s)", tr.taps().size(), tr.takes());
    m_info->setString(info.c_str());
}

void TapAlongPopup::flash() {
    m_circle->setVisible(true);
    m_circle->stopAllActions();
    m_circle->setScale(1.f);
    m_circle->setOpacity(255);
    m_circle->runAction(CCSequence::create(CCScaleTo::create(0.12f, 0.6f), CCHide::create(), nullptr));
}

void TapAlongPopup::onRecord() {
    auto& ta = TapAlong::get();
    if (ta.recording()) {
        int n = ta.stop();
        Notification::create(fmt::format("Take merged: {} taps", n), NotificationIcon::Success)->show();
        if (m_onChanged) m_onChanged();
    } else {
        auto song = Session::get().song(m_editor ? m_editor->m_level : nullptr);
        if (song.isErr()) {
            Notification::create(song.unwrapErr(), NotificationIcon::Error)->show();
            return;
        }
        if (auto r = ta.start(m_editor, song.unwrap().path, m_startOffset); r.isErr()) {
            Notification::create(r.unwrapErr(), NotificationIcon::Error)->show();
            return;
        }
    }
    refresh();
}

void TapAlongPopup::onTick(float) {
    auto& ta = TapAlong::get();
    if (!ta.recording()) return;
    if (!ta.update()) {
        // the song ended
        refresh();
        if (m_onChanged) m_onChanged();
        return;
    }
    const double t = ta.songTime();
    const int m = int(t / 60.0);
    m_time->setString(fmt::format("{}:{:04.1f}", m, t - m * 60.0).c_str());
    m_info->setString(fmt::format("Recording: {} taps in this take", ta.takeSize()).c_str());
}

bool TapAlongPopup::ccTouchBegan(CCTouch* touch, CCEvent* event) {
    if (TapAlong::get().recording()) {
        TapAlong::get().press();
        m_touchTap = true;
        flash();
        return true;
    }
    m_touchTap = false;
    return Popup::ccTouchBegan(touch, event);
}

void TapAlongPopup::ccTouchEnded(CCTouch* touch, CCEvent* event) {
    if (m_touchTap) {
        TapAlong::get().release();
        m_touchTap = false;
        return;
    }
    Popup::ccTouchEnded(touch, event);
}

void TapAlongPopup::ccTouchCancelled(CCTouch* touch, CCEvent* event) {
    if (m_touchTap) {
        TapAlong::get().release();
        m_touchTap = false;
        return;
    }
    Popup::ccTouchCancelled(touch, event);
}

void TapAlongPopup::keyDown(enumKeyCodes key, double timestamp) {
    auto& ta = TapAlong::get();
    if (ta.recording()) {
        if (isTapKey(key)) {
            ta.press();
            flash();
            return;
        }
    }
    Popup::keyDown(key, timestamp);
}

void TapAlongPopup::keyUp(enumKeyCodes key, double timestamp) {
    if (TapAlong::get().recording() && isTapKey(key)) {
        TapAlong::get().release();
        return;
    }
    Popup::keyUp(key, timestamp);
}

void TapAlongPopup::onClose(CCObject* sender) {
    if (TapAlong::get().recording()) {
        TapAlong::get().stop();
        if (m_onChanged) m_onChanged();
    }
    Popup::onClose(sender);
}

} // namespace al::mod
