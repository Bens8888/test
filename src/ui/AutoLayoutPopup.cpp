#include "AutoLayoutPopup.hpp"

#include "../bridge/EditorBridge.hpp"
#include "../recorder/LiveRecorder.hpp"
#include "../recorder/TapAlong.hpp"
#include "../session/Session.hpp"
#include "TapAlongPopup.hpp"
#include "Widgets.hpp"

#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <chrono>
#include <filesystem>
#include <unordered_set>

using namespace geode::prelude;

namespace al::mod {

static AutoLayoutPopup* s_current = nullptr;
static constexpr float kWidth = 400.f;
static constexpr float kHeight = 270.f;

namespace {

enum PickWhat { PickSong = 0, PickRecording = 1 };

Session& S() { return Session::get(); }
gen::GenOptions& O() { return Session::get().options(); }
void save() { Session::get().saveOptions(); }

void pickFile(std::unordered_set<std::string> patterns, char const* what, std::filesystem::path dir, int kind) {
    file::FilePickOptions opts;
    if (!dir.empty()) opts.defaultPath = dir;
    opts.filters.push_back({what, std::move(patterns)});
    async::spawn(file::pick(file::PickMode::OpenFile, std::move(opts)), [kind](file::PickResult res) {
        if (res.isErr()) { Notification::create(res.unwrapErr(), NotificationIcon::Error)->show(); return; }
        auto p = std::move(res).unwrap();
        if (!p) return;
        if (auto ui = AutoLayoutPopup::current()) ui->onPicked(*p, kind);
        else if (kind == PickSong) S().setCustomSong(*p);
        else (void)LiveRecorder::get().load(*p);
    });
}

// a label and a menu side by side, as one row of the form
CCNode* labelRow(float width, CCLabelBMFont*& label, CCMenu*& menu) {
    auto row = CCNode::create();
    row->setContentSize({width, 20.f});
    label = ui::label("", 0.3f);
    label->setAnchorPoint({0.f, 0.5f});
    label->setPosition({0.f, 10.f});
    row->addChild(label);
    menu = CCMenu::create();
    menu->setContentSize({width, 20.f});
    menu->setPosition({0.f, 0.f});
    row->addChild(menu);
    return row;
}

} // namespace

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

// Rebuilding from inside one of its own buttons would destroy the button while cocos is still
// dispatching its touch: such rebuilds run on the next frame instead (if still open).
void AutoLayoutPopup::defer(std::function<void()> f) {
    geode::queueInMainThread([self = this, f = std::move(f)]() {
        if (s_current == self) f();
    });
}

bool AutoLayoutPopup::init(LevelEditorLayer* lel) {
    if (!Popup::init(kWidth, kHeight)) return false;
    m_editor = lel;
    s_current = this;
    this->setTitle("Auto Layout");
    build();
    // a job started earlier (popup closed meanwhile) keeps being shown
    if (auto job = S().job(); job && !job->done) m_watching = job;
    this->schedule(schedule_selector(AutoLayoutPopup::onTick), 0.05f);
    return true;
}

void AutoLayoutPopup::build() {
    if (m_page) m_page->removeFromParent();
    const float W = kWidth - 30.f;
    m_page = CCNode::create();
    m_page->setContentSize({W, kHeight - 50.f});
    m_page->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(m_page, Anchor::Center, {0.f, -14.f});

    auto form = ui::Form::create({W, 160.f}, 100.f, 26.f);
    form->setPosition({0.f, kHeight - 50.f - 160.f});
    m_page->addChild(form);
    const float cw = W - 110.f;

    // song
    CCMenu* songMenu = nullptr;
    form->row("Song", labelRow(cw, m_songLabel, songMenu));
    auto change = ui::button("Change", [] { pickFile({"*.mp3", "*.ogg", "*.wav"}, "Audio", {}, PickSong); }, 0.4f, "GJ_button_04.png");
    change->setPosition({cw - 28.f, 10.f});
    songMenu->addChild(change);
    if (S().customSong()) {
        auto reset = ui::button("Level song", [this] { S().clearCustomSong(); defer([this] { build(); }); }, 0.4f, "GJ_button_04.png");
        reset->setPosition({cw - 92.f, 10.f});
        songMenu->addChild(reset);
    }

    // clicks
    static const gen::InputSource kSources[] = {gen::InputSource::Music, gen::InputSource::Taps, gen::InputSource::Recording};
    int source = 0;
    for (int i = 0; i < 3; ++i) if (kSources[i] == O().input) source = i;
    form->row("Clicks", ui::CycleControl::create({"Follow the music", "My tap-along", "A recording / macro"}, source, 170.f, [this](int i) {
        O().input = kSources[std::clamp(i, 0, 2)];
        save();
        defer([this] { refreshClicks(); });
    }));
    form->row("", labelRow(cw, m_clicksLabel, m_clicksMenu));

    // difficulty, speed, structures
    std::vector<std::string> diffs{"Auto"};
    for (int d = 0; d <= 9; ++d) diffs.push_back(difficultyLabel(gen::Difficulty(d)));
    const int di = O().difficulty == gen::Difficulty::Auto ? 0 : int(O().difficulty) + 1;
    form->row("Difficulty", ui::CycleControl::create(diffs, di, 170.f, [](int i) {
        O().difficulty = i == 0 ? gen::Difficulty::Auto : gen::Difficulty(i - 1);
        save();
    }));
    form->row("Speed", ui::CycleControl::create({"Auto", "Slow", "Normal", "Fast"}, std::clamp(O().speedStyle, 0, 3), 170.f, [](int i) {
        O().speedStyle = i;
        save();
    }));
    form->row("Structures", ui::CycleControl::create({"Auto", "Rails", "Slopes", "Compact", "Spiky", "Classic"}, std::clamp(O().structureStyle, 0, 5), 170.f, [](int i) {
        O().structureStyle = i;
        save();
    }));

    // status, buttons, progress
    m_status = ui::label("", 0.3f);
    m_status->setPosition({W / 2.f, 48.f});
    m_page->addChild(m_status);
    m_actions = CCMenu::create();
    m_actions->setContentSize({W, 30.f});
    m_actions->setPosition({0.f, 6.f});
    m_page->addChild(m_actions);

    m_progress = CCNode::create();
    m_progress->setContentSize({W, 30.f});
    m_progress->setPosition({0.f, 6.f});
    auto barBg = CCLayerColor::create({0, 0, 0, 120}, W - 110.f, 10.f);
    barBg->setPosition({10.f, 18.f});
    m_progress->addChild(barBg);
    m_progressBar = CCLayerColor::create({90, 230, 120, 255}, W - 110.f, 10.f);
    m_progressBar->setPosition({10.f, 18.f});
    m_progressBar->ignoreAnchorPointForPosition(false);
    m_progressBar->setAnchorPoint({0.f, 0.f});
    m_progressBar->setScaleX(0.f);
    m_progress->addChild(m_progressBar);
    m_progressLabel = ui::label("", 0.28f);
    m_progressLabel->setAnchorPoint({0.f, 0.5f});
    m_progressLabel->setPosition({10.f, 6.f});
    m_progress->addChild(m_progressLabel);
    auto cancelMenu = CCMenu::create();
    cancelMenu->setPosition({0.f, 0.f});
    auto cancel = ui::button("Cancel", [] { S().cancel(); }, 0.45f, "GJ_button_06.png");
    cancel->setPosition({W - 50.f, 18.f});
    cancelMenu->addChild(cancel);
    m_progress->addChild(cancelMenu);
    m_progress->setVisible(false);
    m_page->addChild(m_progress);

    refreshSong();
    refreshClicks();
    refreshState();
}

void AutoLayoutPopup::refreshSong() {
    if (!m_songLabel) return;
    auto res = S().song(m_editor ? m_editor->m_level : nullptr);
    std::string text = res.isOk() ? res.unwrap().label : "No song: " + res.unwrapErr();
    m_songLabel->setString(text.c_str());
    const float room = kWidth - 30.f - 110.f - (S().customSong() ? 130.f : 65.f);
    m_songLabel->setScale(std::min(0.32f, room / std::max(1.f, m_songLabel->getContentWidth())));
    m_songLabel->setColor(res.isOk() ? ccColor3B{255, 255, 255} : ccColor3B{255, 120, 120});
}

void AutoLayoutPopup::refreshClicks() {
    if (!m_clicksLabel || !m_clicksMenu) return;
    m_clicksMenu->removeAllChildren();
    const float cw = kWidth - 30.f - 110.f;
    std::string text;
    if (O().input == gen::InputSource::Taps) {
        auto& tr = TapAlong::get().track(m_editor);
        text = tr.empty() ? std::string("No taps yet") : fmt::format("{} taps", tr.taps().size());
        auto tap = ui::button("Tap along", [this] {
            if (auto p = TapAlongPopup::create(m_editor, [] {
                    if (auto ui = AutoLayoutPopup::current()) ui->defer([ui] { ui->refreshClicks(); });
                }))
                p->show();
        }, 0.4f, "GJ_button_01.png");
        tap->setPosition({cw - 34.f, 10.f});
        m_clicksMenu->addChild(tap);
    } else if (O().input == gen::InputSource::Recording) {
        auto& last = LiveRecorder::get().last();
        text = last ? fmt::format("{} clicks ({})", last->pressTimes().size(), LiveRecorder::get().lastPath().filename().string())
                    : std::string("None yet: record a run or load a macro");
        auto load = ui::button("Load", [] { pickFile({"*.alrec", "*.json", "*.gdr", "*.txt"}, "Recordings and macros", LiveRecorder::folder(), PickRecording); },
                               0.4f, "GJ_button_04.png");
        load->setPosition({cw - 22.f, 10.f});
        m_clicksMenu->addChild(load);
    } else {
        text = "Clicks on the beats and hits of the song";
    }
    m_clicksLabel->setString(text.c_str());
    m_clicksLabel->setScale(std::min(0.28f, (cw - 80.f) / std::max(1.f, m_clicksLabel->getContentWidth())));
}

void AutoLayoutPopup::onPicked(std::filesystem::path const& p, int what) {
    if (what == PickSong) {
        S().setCustomSong(p);
        build();
        return;
    }
    auto r = LiveRecorder::get().load(p);
    if (r.isErr()) {
        Notification::create("Cannot load the recording: " + r.unwrapErr(), NotificationIcon::Error)->show();
    } else {
        O().input = gen::InputSource::Recording;
        save();
        build();
    }
}

// The buttons for where things stand: nothing yet, a layout to place, or a placed one.
void AutoLayoutPopup::refreshState() {
    if (!m_actions) return;
    m_actions->removeAllChildren();
    const float W = kWidth - 30.f;
    auto cur = S().current();
    if (!cur) {
        auto gen = ui::button("Generate", [this] { startGenerate(); }, 0.75f, "GJ_button_01.png");
        gen->setPosition({W / 2.f, 15.f});
        m_actions->addChild(gen);
        return;
    }
    if (!m_placed) {
        auto place = ui::button("Place in level", [this] { applyLayout(); }, 0.6f, "GJ_button_01.png");
        place->setPosition({W / 2.f - 70.f, 15.f});
        m_actions->addChild(place);
    } else {
        auto remove = ui::button("Remove", [this] { removeLayout(); }, 0.6f, "GJ_button_06.png");
        remove->setPosition({W / 2.f - 70.f, 15.f});
        m_actions->addChild(remove);
    }
    auto another = ui::button("New layout", [this] {
        if (m_placed) removeLayout();
        startGenerate();
    }, 0.6f, "GJ_button_02.png");
    another->setPosition({W / 2.f + 75.f, 15.f});
    m_actions->addChild(another);
}

void AutoLayoutPopup::setStatus(std::string const& text) {
    if (!m_status) return;
    m_status->setString(text.c_str());
    m_status->setScale(std::min(0.3f, (kWidth - 40.f) / std::max(1.f, m_status->getContentWidth())));
}

void AutoLayoutPopup::onClose(CCObject* sender) {
    if (s_current == this) s_current = nullptr;
    save();
    Popup::onClose(sender);
}

void AutoLayoutPopup::startGenerate() {
    if (S().busy()) return;
    // one layout, from a new seed every time
    O().engine = 2;
    O().variants = 1;
    O().seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ull);
    save();
    m_watching = S().startGenerate(m_editor ? m_editor->m_level : nullptr);
    setStatus("");
}

void AutoLayoutPopup::onTick(float) {
    auto job = m_watching;
    const bool running = job && !job->done;
    if (m_progress) m_progress->setVisible(running);
    if (m_actions) m_actions->setVisible(!running);
    if (running) {
        if (m_progressBar) m_progressBar->setScaleX(std::clamp(job->progress.load(), 0.f, 1.f));
        if (m_progressLabel) m_progressLabel->setString(job->stageText().c_str());
    }
    if (job && job->done) {
        m_watching.reset();
        onJobFinished(job);
    }
}

void AutoLayoutPopup::onJobFinished(std::shared_ptr<Job> const& job) {
    auto err = job->errorText();
    if (!err.empty()) {
        if (err != "cancelled") Notification::create("Auto Layout: " + err, NotificationIcon::Error)->show();
        setStatus(err == "cancelled" ? "" : err);
        refreshState();
        return;
    }
    S().collect(job);
    m_placed = false;
    if (auto cur = S().current()) {
        auto const& r = cur->report;
        setStatus(fmt::format("{}: {} clicks, {} objects", r.beatable ? "Ready, checked beatable" : "Ready (could not be checked)", r.clicks, r.objects));
    }
    refreshState();
}

void AutoLayoutPopup::applyLayout() {
    auto cur = S().current();
    if (!cur || !m_editor) return;
    auto doApply = [this]() {
        auto cur = S().current();
        if (!cur) return;
        auto res = EditorBridge::get().applyLevel(m_editor, cur->level, fmt::format("AutoLayout (seed {})", cur->seed));
        if (res.isErr()) {
            Notification::create(res.unwrapErr(), NotificationIcon::Error)->show();
            return;
        }
        m_placed = true;
        setStatus(fmt::format("Placed {} objects on a new editor layer.", res.unwrap()));
        refreshState();
    };
    const int existing = EditorBridge::get().gameplayObjectCount(m_editor);
    if (existing > 0) {
        createQuickPopup("Auto Layout",
                         fmt::format("This level already has <cy>{}</c> objects. The layout goes on a <cg>new editor layer</c>; your objects stay. Continue?", existing),
                         "Cancel", "Place", [doApply](FLAlertLayer*, bool yes) {
                             if (yes) doApply();
                         });
    } else {
        doApply();
    }
}

void AutoLayoutPopup::removeLayout() {
    if (!m_editor) return;
    if (EditorBridge::get().undoLast(m_editor)) setStatus("Removed the layout.");
    m_placed = false;
    refreshState();
}

} // namespace al::mod
