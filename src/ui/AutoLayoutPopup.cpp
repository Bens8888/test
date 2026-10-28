#include "AutoLayoutPopup.hpp"

#include "../bridge/EditorBridge.hpp"
#include "../recorder/LiveRecorder.hpp"
#include "../session/Session.hpp"
#include "Ghost.hpp"
#include "Timeline.hpp"
#include "Widgets.hpp"

#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <al/audio/analysis.hpp>

#include <chrono>
#include <filesystem>

using namespace geode::prelude;

namespace al::mod {

static AutoLayoutPopup* s_current = nullptr;
static constexpr float kWidth = 480.f;
static constexpr float kHeight = 300.f;

namespace {

char const* kTabNames[] = {"Generate", "Style", "Modes", "Sections", "Playtest", "Training", "Settings"};
char const* kModeNames[] = {"Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing"};
char const* kSectionNames[] = {"Intro", "Verse", "Chorus", "Build-up", "Drop", "Break", "Outro"};

enum PickWhat { PickSong = 0, PickRecording = 1 };

Session& S() { return Session::get(); }
gen::GenOptions& O() { return Session::get().options(); }
void save() { Session::get().saveOptions(); }

std::vector<std::string> difficultyLabels() {
    std::vector<std::string> v{"Auto (AI decides)"};
    for (int d = 0; d <= 9; ++d) v.push_back(difficultyLabel(gen::Difficulty(d)));
    return v;
}
int difficultyIndex() { return O().difficulty == gen::Difficulty::Auto ? 0 : int(O().difficulty) + 1; }

std::string pct(float v) { return fmt::format("{:.0f}%", v * 100.f); }

void openFolder(std::filesystem::path const& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    utils::file::openFolder(p);
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

// Rebuilding a page from inside one of its own buttons would destroy the button while cocos is
// still dispatching its touch: such rebuilds run on the next frame instead (if still open).
void AutoLayoutPopup::defer(std::function<void()> f) {
    geode::queueInMainThread([self = this, f = std::move(f)]() {
        if (s_current == self) f();
    });
}

bool AutoLayoutPopup::init(LevelEditorLayer* lel) {
    if (!Popup::init(kWidth, kHeight)) return false;
    m_editor = lel;
    s_current = this;
    this->setTitle("AutoLayout");
    auto tabMenu = CCMenu::create();
    tabMenu->setContentSize({kWidth - 20.f, 30.f});
    const int n = int(Tab::Count);
    const float tabW = (kWidth - 30.f) / n;
    for (int i = 0; i < n; ++i) {
        auto btn = TabButton::create(kTabNames[i], this, menu_selector(AutoLayoutPopup::onTab));
        btn->setTag(i);
        btn->setScale(std::min(1.f, tabW / std::max(1.f, btn->getContentWidth())));
        btn->setPosition({tabW * (i + 0.5f), 15.f});
        tabMenu->addChild(btn);
        m_tabButtons[size_t(i)] = btn;
    }
    m_mainLayer->addChildAtPosition(tabMenu, Anchor::Top, {-(kWidth - 30.f) / 2.f, -62.f});
    m_contentSize = CCSize(kWidth - 24.f, kHeight - 92.f);
    for (int i = 0; i < n; ++i) rebuildTab(Tab(i));
    selectTab(Tab::Generate);
    // a job started earlier (popup closed meanwhile) keeps being shown
    if (auto job = S().job(); job && !job->done) m_watching = job;
    if (S().current()) showResults(true);
    this->schedule(schedule_selector(AutoLayoutPopup::onTick), 0.05f);
    return true;
}

void AutoLayoutPopup::rebuildTab(Tab tab) {
    size_t i = size_t(tab);
    if (m_tabPages[i]) m_tabPages[i]->removeFromParent();
    CCNode* page = nullptr;
    switch (tab) {
        case Tab::Generate: page = buildGenerateTab(); break;
        case Tab::Style: page = buildStyleTab(); break;
        case Tab::Gamemodes: page = buildGamemodesTab(); break;
        case Tab::Sections: page = buildSectionsTab(); break;
        case Tab::Playtest: page = buildPlaytestTab(); break;
        case Tab::Training: page = buildTrainingTab(); break;
        case Tab::Settings: page = buildSettingsTab(); break;
        default: page = CCNode::create(); break;
    }
    page->setContentSize(m_contentSize);
    page->setAnchorPoint({0.5f, 0.5f});
    m_mainLayer->addChildAtPosition(page, Anchor::Center, {0.f, -24.f});
    page->setVisible(tab == m_tab);
    m_tabPages[i] = page;
}

void AutoLayoutPopup::onClose(CCObject* sender) {
    if (s_current == this) s_current = nullptr;
    save();
    Popup::onClose(sender);
}

void AutoLayoutPopup::onTab(CCObject* sender) { selectTab(Tab(static_cast<CCNode*>(sender)->getTag())); }

void AutoLayoutPopup::selectTab(Tab tab) {
    m_tab = tab;
    for (int i = 0; i < int(Tab::Count); ++i) {
        bool on = i == int(tab);
        if (m_tabButtons[size_t(i)]) {
            m_tabButtons[size_t(i)]->toggle(on);
            m_tabButtons[size_t(i)]->setEnabled(!on);
        }
        if (m_tabPages[size_t(i)]) m_tabPages[size_t(i)]->setVisible(on);
    }
    if (tab == Tab::Training || tab == Tab::Playtest) rebuildTab(tab); // live info
}

void AutoLayoutPopup::setStatus(std::string const& text) {
    if (m_status) m_status->setString(text.c_str());
}

// ============================================================== Generate

CCNode* AutoLayoutPopup::buildGenerateTab() {
    auto page = CCNode::create();
    page->setContentSize(m_contentSize);
    const float W = m_contentSize.width, H = m_contentSize.height;

    // ---------------- setup view
    m_setupView = CCNode::create();
    m_setupView->setContentSize(m_contentSize);
    page->addChild(m_setupView);
    auto form = ui::Form::create({W, H - 50.f}, 150.f, 23.f);
    form->setPosition({0.f, 50.f});
    m_setupView->addChild(form);

    // song
    auto songRow = CCNode::create();
    songRow->setContentSize({W - 160.f, 20.f});
    m_songLabel = ui::label("", 0.3f, "bigFont.fnt");
    m_songLabel->setAnchorPoint({0.f, 0.5f});
    m_songLabel->setPosition({0.f, 10.f});
    songRow->addChild(m_songLabel);
    auto songMenu = CCMenu::create();
    songMenu->setContentSize({120.f, 20.f});
    songMenu->ignoreAnchorPointForPosition(false);
    songMenu->setAnchorPoint({1.f, 0.5f});
    songMenu->setPosition({W - 160.f, 10.f});
    auto levelSongBtn = ui::button("Level song", [this] { S().clearCustomSong(); refreshSongLabel(); }, 0.45f, "GJ_button_04.png");
    levelSongBtn->setPosition({30.f, 10.f});
    songMenu->addChild(levelSongBtn);
    auto pickBtn = ui::button("File...", [] {
        file::FilePickOptions opts;
        opts.filters.push_back({"Audio", {"*.mp3", "*.ogg", "*.wav"}});
        async::spawn(file::pick(file::PickMode::OpenFile, std::move(opts)), [](file::PickResult res) {
            if (res.isErr()) { Notification::create(res.unwrapErr(), NotificationIcon::Error)->show(); return; }
            auto p = std::move(res).unwrap();
            if (!p) return;
            if (auto ui = AutoLayoutPopup::current()) ui->onPicked(*p, PickSong);
            else S().setCustomSong(*p);
        });
    }, 0.45f, "GJ_button_04.png");
    pickBtn->setPosition({92.f, 10.f});
    songMenu->addChild(pickBtn);
    songRow->addChild(songMenu);
    form->row("Song", songRow);

    // input source
    form->row("Clicks come from", ui::CycleControl::create({"The music (automatic)", "My recording"}, int(O().input), 170.f, [this](int i) {
        O().input = gen::InputSource(i);
        save();
        refreshRecordingLabel();
    }));
    auto recRow = CCNode::create();
    recRow->setContentSize({W - 160.f, 20.f});
    m_recordingLabel = ui::label("", 0.28f, "bigFont.fnt");
    m_recordingLabel->setAnchorPoint({0.f, 0.5f});
    m_recordingLabel->setPosition({0.f, 10.f});
    recRow->addChild(m_recordingLabel);
    auto recMenu = CCMenu::create();
    recMenu->setContentSize({60.f, 20.f});
    recMenu->ignoreAnchorPointForPosition(false);
    recMenu->setAnchorPoint({1.f, 0.5f});
    recMenu->setPosition({W - 160.f, 10.f});
    auto loadRec = ui::button("Load...", [] {
        file::FilePickOptions opts;
        opts.defaultPath = LiveRecorder::folder();
        opts.filters.push_back({"Recordings and macros", {"*.alrec", "*.json", "*.gdr", "*.txt"}});
        async::spawn(file::pick(file::PickMode::OpenFile, std::move(opts)), [](file::PickResult res) {
            if (res.isErr()) { Notification::create(res.unwrapErr(), NotificationIcon::Error)->show(); return; }
            auto p = std::move(res).unwrap();
            if (!p) return;
            if (auto ui = AutoLayoutPopup::current()) ui->onPicked(*p, PickRecording);
            else (void)LiveRecorder::get().load(*p);
        });
    }, 0.45f, "GJ_button_04.png");
    loadRec->setPosition({30.f, 10.f});
    recMenu->addChild(loadRec);
    recRow->addChild(recMenu);
    form->row("Recording", recRow);
    form->row("Faithful to my clicks", ui::SliderControl::create(O().faithfulness, 200.f,
        [](float v) { return v >= 0.95f ? std::string("strict") : v <= 0.1f ? std::string("creative") : pct(v); },
        [](float v) { O().faithfulness = v; save(); }));

    form->row("Difficulty", ui::CycleControl::create(difficultyLabels(), difficultyIndex(), 170.f, [](int i) {
        O().difficulty = i == 0 ? gen::Difficulty::Auto : gen::Difficulty(i - 1);
        save();
    }));

    // seed + variants
    auto seedRow = CCMenu::create();
    seedRow->setContentSize({W - 160.f, 20.f});
    seedRow->ignoreAnchorPointForPosition(false);
    m_seedInput = TextInput::create(110.f, "seed", "bigFont.fnt");
    m_seedInput->setCommonFilter(CommonFilter::Uint);
    m_seedInput->setString(std::to_string(O().seed));
    m_seedInput->setScale(0.7f);
    m_seedInput->setPosition({40.f, 10.f});
    m_seedInput->setCallback([](std::string const& s) {
        if (s.empty()) return;
        try { O().seed = std::stoull(s); } catch (...) {}
        save();
    });
    seedRow->addChild(m_seedInput);
    auto dice = ui::button("Random", [this] {
        O().seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007ull);
        m_seedInput->setString(std::to_string(O().seed));
        save();
    }, 0.4f, "GJ_button_04.png");
    dice->setPosition({112.f, 10.f});
    seedRow->addChild(dice);
    form->row("Seed (same seed = same layout)", seedRow);
    std::vector<std::string> vlabels;
    for (int v = 1; v <= 8; ++v) vlabels.push_back(fmt::format("{} variant{}", v, v > 1 ? "s" : ""));
    form->row("Variants", ui::CycleControl::create(vlabels, std::clamp(O().variants, 1, 8) - 1, 130.f, [](int i) { O().variants = i + 1; save(); }));

    // generate button + progress
    m_generateMenu = CCMenu::create();
    m_generateMenu->setContentSize({W, 40.f});
    m_generateMenu->ignoreAnchorPointForPosition(false);
    m_generateMenu->setAnchorPoint({0.5f, 0.f});
    m_generateMenu->setPosition({W / 2.f, 4.f});
    auto gen = ui::button("Generate", [this] { startGenerate(); }, 0.9f, "GJ_button_01.png");
    gen->setPosition({W / 2.f - 50.f, 20.f});
    m_generateMenu->addChild(gen);
    auto results = ui::button("Results", [this] { if (S().current()) showResults(true); }, 0.55f, "GJ_button_02.png");
    results->setPosition({W / 2.f + 70.f, 20.f});
    m_generateMenu->addChild(results);
    m_setupView->addChild(m_generateMenu);

    m_progressNode = CCNode::create();
    m_progressNode->setContentSize({W, 40.f});
    m_progressNode->setPosition({0.f, 4.f});
    auto barBg = CCLayerColor::create({0, 0, 0, 120}, W - 120.f, 10.f);
    barBg->setPosition({10.f, 24.f});
    m_progressNode->addChild(barBg);
    m_progressBar = CCLayerColor::create({90, 230, 120, 255}, W - 120.f, 10.f);
    m_progressBar->setPosition({10.f, 24.f});
    m_progressBar->setScaleX(0.f);
    m_progressBar->ignoreAnchorPointForPosition(false);
    m_progressBar->setAnchorPoint({0.f, 0.f});
    m_progressNode->addChild(m_progressBar);
    m_progressLabel = ui::label("", 0.3f);
    m_progressLabel->setAnchorPoint({0.f, 0.5f});
    m_progressLabel->setPosition({10.f, 10.f});
    m_progressNode->addChild(m_progressLabel);
    auto cancelMenu = CCMenu::create();
    cancelMenu->setPosition({0.f, 0.f});
    auto cancel = ui::button("Cancel", [] { S().cancel(); }, 0.5f, "GJ_button_06.png");
    cancel->setPosition({W - 55.f, 22.f});
    cancelMenu->addChild(cancel);
    m_progressNode->addChild(cancelMenu);
    m_progressNode->setVisible(false);
    m_setupView->addChild(m_progressNode);

    // ---------------- results view
    m_resultView = CCNode::create();
    m_resultView->setContentSize(m_contentSize);
    m_resultView->setVisible(false);
    page->addChild(m_resultView);
    m_timeline = ui::Timeline::create({W, 104.f});
    m_timeline->setAnchorPoint({0.f, 1.f});
    m_timeline->setPosition({0.f, H});
    m_resultView->addChild(m_timeline);
    m_viewSlider = ui::SliderControl::create(0.f, 200.f, [](float) { return std::string("scroll"); }, [this](float v) {
        if (!m_timeline) return;
        double span = m_timeline->viewSpan();
        m_timeline->setView(v * std::max(0.0, m_timeline->duration() - span), span);
    });
    m_viewSlider->setPosition({W / 2.f - 90.f, H - 116.f});
    m_resultView->addChild(m_viewSlider);

    auto rmenu = CCMenu::create();
    rmenu->setContentSize(m_contentSize);
    rmenu->setPosition({0.f, 0.f});
    m_resultView->addChild(rmenu);
    m_variantCycle = ui::CycleControl::create({"-"}, 0, 150.f, [this](int i) {
        S().select(i);
        defer([this] { refreshResults(); });
    });
    m_variantCycle->setPosition({4.f, H - 136.f});
    m_resultView->addChild(m_variantCycle);
    m_sectionCycle = ui::CycleControl::create({"-"}, 0, 170.f, [this](int i) {
        m_rerollPart = i;
        if (m_timeline) m_timeline->setHighlightedPart(i);
    });
    m_sectionCycle->setPosition({160.f, H - 136.f});
    m_resultView->addChild(m_sectionCycle);
    auto reroll = ui::button("Re-roll section", [this] {
        if (S().busy()) return;
        m_watching = S().startReroll(m_rerollPart);
        setStatus("Re-rolling...");
    }, 0.4f, "GJ_button_04.png");
    reroll->setPosition({W - 90.f, H - 136.f});
    rmenu->addChild(reroll);
    auto rerollAll = ui::button("Re-roll all", [this] {
        if (S().busy()) return;
        m_watching = S().startReroll(-1);
        setStatus("Re-rolling the whole layout with a new seed...");
    }, 0.4f, "GJ_button_04.png");
    rerollAll->setPosition({W - 30.f, H - 136.f});
    rmenu->addChild(rerollAll);

    m_reportLabel = CCLabelBMFont::create("", "chatFont.fnt", (W - 10.f) / 0.5f, kCCTextAlignmentLeft);
    m_reportLabel->setScale(0.5f);
    m_reportLabel->setAnchorPoint({0.f, 1.f});
    m_reportLabel->setPosition({4.f, H - 150.f});
    m_resultView->addChild(m_reportLabel);

    auto apply = ui::button("Apply to level", [this] { applySelected(); }, 0.6f, "GJ_button_01.png");
    apply->setPosition({W / 2.f - 120.f, 14.f});
    rmenu->addChild(apply);
    auto undo = ui::button("Undo", [this] { undoLast(); }, 0.55f, "GJ_button_06.png");
    undo->setPosition({W / 2.f - 20.f, 14.f});
    rmenu->addChild(undo);
    auto ghost = ui::button("Ghost", [this] {
        auto cur = S().current();
        if (!cur || !m_editor) return;
        if (ui::ghostVisible(m_editor)) ui::hideGhost(m_editor);
        else if (ui::showGhost(m_editor, cur->level, cur->inputs)) ui::playGhost(m_editor);
    }, 0.55f, "GJ_button_02.png");
    ghost->setPosition({W / 2.f + 55.f, 14.f});
    rmenu->addChild(ghost);
    auto back = ui::button("Setup", [this] { showResults(false); }, 0.55f, "GJ_button_05.png");
    back->setPosition({W / 2.f + 130.f, 14.f});
    rmenu->addChild(back);

    m_status = ui::label("", 0.28f);
    m_status->setPosition({W / 2.f, -6.f});
    page->addChild(m_status);
    refreshSongLabel();
    refreshRecordingLabel();
    return page;
}

void AutoLayoutPopup::refreshSongLabel() {
    if (!m_songLabel) return;
    auto res = S().song(m_editor ? m_editor->m_level : nullptr);
    std::string text = res.isOk() ? res.unwrap().label : "No song: " + res.unwrapErr();
    m_songLabel->setString(text.c_str());
    m_songLabel->setScale(std::min(0.32f, (m_contentSize.width - 290.f) / std::max(1.f, m_songLabel->getContentWidth())));
    m_songLabel->setColor(res.isOk() ? ccColor3B{255, 255, 255} : ccColor3B{255, 120, 120});
}

void AutoLayoutPopup::refreshRecordingLabel() {
    if (!m_recordingLabel) return;
    auto& last = LiveRecorder::get().last();
    std::string text;
    if (last) {
        text = fmt::format("{} clicks, {:.1f} s ({})", last->pressTimes().size(), last->duration,
                           LiveRecorder::get().lastPath().filename().string());
    } else {
        text = "None yet: pause menu > Rec, or the record hotkey, then play along";
    }
    m_recordingLabel->setString(text.c_str());
    m_recordingLabel->setScale(std::min(0.3f, (m_contentSize.width - 230.f) / std::max(1.f, m_recordingLabel->getContentWidth())));
    m_recordingLabel->setOpacity(O().input == gen::InputSource::Recording ? 255 : 130);
}

void AutoLayoutPopup::onPicked(std::filesystem::path const& p, int what) {
    if (what == PickSong) {
        S().setCustomSong(p);
        refreshSongLabel();
    } else {
        auto r = LiveRecorder::get().load(p);
        if (r.isErr()) Notification::create("Cannot load the recording: " + r.unwrapErr(), NotificationIcon::Error)->show();
        else {
            O().input = gen::InputSource::Recording;
            save();
            rebuildTab(Tab::Generate);
        }
        refreshRecordingLabel();
    }
}

void AutoLayoutPopup::startGenerate() {
    if (S().busy()) return;
    if (m_editor) O().groupBase = EditorBridge::get().firstFreeGroup(m_editor);
    save();
    m_watching = S().startGenerate(m_editor ? m_editor->m_level : nullptr);
    setStatus("");
}

void AutoLayoutPopup::onTick(float) {
    auto job = m_watching;
    bool running = job && !job->done;
    if (m_progressNode) m_progressNode->setVisible(running && job->kind != Job::Kind::Playtest);
    if (m_generateMenu) m_generateMenu->setVisible(!running);
    if (running) {
        if (m_progressBar) m_progressBar->setScaleX(std::clamp(job->progress.load(), 0.f, 1.f));
        if (m_progressLabel) m_progressLabel->setString(job->stageText().c_str());
        if (job->kind == Job::Kind::Reroll) setStatus(job->stageText());
        if (job->kind == Job::Kind::Playtest && m_playtestLabel) m_playtestLabel->setString(job->stageText().c_str());
    }
    if (job && job->done) {
        m_watching.reset();
        onJobFinished(job);
    }
}

void AutoLayoutPopup::onJobFinished(std::shared_ptr<Job> const& job) {
    auto err = job->errorText();
    if (job->kind == Job::Kind::Playtest) {
        S().collect(job);
        std::string text;
        if (!err.empty()) text = "Playtest: " + err;
        else if (job->beatable) text = fmt::format("The bot beat the level ({} clicks, {:.1f} s of solving).", job->solution.pressCount(), job->seconds);
        else
            text = fmt::format("NOT beatable: the bot dies at {:.2f} s (x {:.0f}, y {:.0f}, {}).", job->failure.tick / 240.0, job->failure.x, job->failure.y,
                               phys::deathCauseName(job->failure.cause));
        if (m_playtestLabel) m_playtestLabel->setString(text.c_str());
        if (job->beatable && m_editor) {
            auto lvl = level::Level::fromLevelString(EditorBridge::get().levelString(m_editor));
            if (lvl) ui::showGhost(m_editor, *lvl, job->solution);
        }
        return;
    }
    if (!err.empty()) {
        Notification::create("AutoLayout: " + err, err == "cancelled" ? NotificationIcon::Info : NotificationIcon::Error)->show();
        setStatus(err);
        return;
    }
    S().collect(job);
    showResults(true);
    if (auto cur = S().current()) {
        setStatus(fmt::format("{} variant(s) in {:.1f} s. Best first.", S().variants().size(), job->seconds));
        (void)cur;
    }
}

void AutoLayoutPopup::showResults(bool on) {
    if (m_setupView) m_setupView->setVisible(!on);
    if (m_resultView) m_resultView->setVisible(on);
    if (on) refreshResults();
}

void AutoLayoutPopup::refreshResults() {
    auto cur = S().current();
    if (!cur) return;
    std::vector<std::string> vl;
    for (size_t i = 0; i < S().variants().size(); ++i)
        vl.push_back(fmt::format("Variant {} ({:+.1f})", i + 1, S().variants()[i].score));
    auto rebuildCycle = [&](ui::CycleControl*& cyc, std::vector<std::string> labels, int idx, float w, std::function<void(int)> cb) {
        CCPoint pos = cyc->getPosition();
        auto parent = cyc->getParent();
        cyc->removeFromParent();
        cyc = ui::CycleControl::create(std::move(labels), idx, w, std::move(cb));
        cyc->setPosition(pos);
        parent->addChild(cyc);
    };
    rebuildCycle(m_variantCycle, vl, S().selected(), 150.f, [this](int i) { S().select(i); defer([this] { refreshResults(); }); });
    std::vector<std::string> sl;
    for (auto& p : cur->plan.parts) sl.push_back(fmt::format("{}: {} {:.0f}-{:.0f}s", p.index + 1, level::modeName(p.mode), p.t0, p.t1));
    m_rerollPart = std::clamp(m_rerollPart, 0, std::max(0, int(sl.size()) - 1));
    rebuildCycle(m_sectionCycle, sl, m_rerollPart, 170.f, [this](int i) {
        m_rerollPart = i;
        if (m_timeline) m_timeline->setHighlightedPart(i);
    });
    m_timeline->setData(S().analysis(), cur);
    m_timeline->setHighlightedPart(m_rerollPart);
    // report summary (full report in the Playtest tab)
    auto const& r = cur->report;
    std::string text = fmt::format(
        "{} | {} clicks ({} forced, {} left out) | windows min {:.0f} ms, mean {:.0f} ms\n"
        "Longest gap without input {:.1f} s at {:.1f} s | music alignment {:.0f}% | difficulty follows the music r = {:.2f}\n"
        "Style {} | model: {} | {} objects",
        r.beatable ? (r.intendedInputs ? "Beatable (bot-verified with the designed clicks)" : "Beatable (bot found a route)") : "NOT BEATABLE",
        r.clicks, r.forced, r.dropped, r.minWindowMs, r.meanWindowMs, r.longestGap, r.longestGapAt, r.musicAlignment * 100.f, r.curveCorrelation,
        gen::styleName(cur->plan.style), cur->model, r.objects);
    if (S().fusion()) text += "\nYour clicks: " + S().fusion()->summary();
    m_reportLabel->setString(text.c_str());
}

void AutoLayoutPopup::applySelected() {
    auto cur = S().current();
    if (!cur || !m_editor) return;
    int existing = EditorBridge::get().gameplayObjectCount(m_editor);
    auto doApply = [this]() {
        auto cur = S().current();
        if (!cur) return;
        auto res = EditorBridge::get().applyLevel(m_editor, cur->level, fmt::format("AutoLayout (seed {})", cur->seed));
        if (res.isErr()) {
            Notification::create(res.unwrapErr(), NotificationIcon::Error)->show();
            return;
        }
        Notification::create(fmt::format("AutoLayout: placed {} objects on a new editor layer", res.unwrap()), NotificationIcon::Success)->show();
        setStatus("Applied. Use Undo to remove it again.");
    };
    if (existing > 0) {
        createQuickPopup("AutoLayout",
                         fmt::format("This level already has <cy>{}</c> objects. The layout is placed from the start of the song on a "
                                     "<cg>new editor layer</c>; your objects stay. Continue?", existing),
                         "Cancel", "Apply", [doApply](FLAlertLayer*, bool yes) {
                             if (yes) doApply();
                         });
    } else {
        doApply();
    }
}

void AutoLayoutPopup::undoLast() {
    if (!m_editor) return;
    if (EditorBridge::get().undoLast(m_editor)) setStatus("Removed the last AutoLayout layout.");
    else setStatus("Nothing to undo.");
    ui::hideGhost(m_editor);
}

// ============================================================== Style

CCNode* AutoLayoutPopup::buildStyleTab() {
    auto form = ui::Form::create(m_contentSize, 150.f, 22.f);
    std::vector<std::string> styles{"Auto (AI decides)"};
    for (int i = 1; i < int(gen::Style::Count); ++i) styles.push_back(gen::styleName(gen::Style(i)));
    form->row("Style", ui::CycleControl::create(styles, int(O().style), 170.f, [](int i) { O().style = gen::Style(i); save(); }));
    form->row("Density", ui::SliderControl::create(O().density < 0 ? 0.5f : O().density, 200.f,
        [](float v) { return O().density < 0 ? std::string("auto") : pct(v); },
        [](float v) { O().density = v; save(); }), "drag = set");
    form->row("Density: let the AI decide", ui::ToggleControl::create(O().density < 0, [](bool on) { O().density = on ? -1.f : 0.5f; save(); }));
    form->row("Block theme", ui::CycleControl::create({"Auto", "Classic", "Colored", "Outline", "Grid", "Bricks"}, int(O().theme), 150.f,
                                                      [](int i) { O().theme = gen::BlockTheme(i); save(); }));
    form->row("Spike types", ui::CycleControl::create({"Auto", "Classic", "Colored", "Mixed"}, int(O().spikes), 150.f,
                                                      [](int i) { O().spikes = gen::SpikeSet(i); save(); }));
    form->row("Decoration", ui::CycleControl::create({"Auto", "None", "Light", "Medium", "Heavy"}, O().decoLevel + 1, 150.f,
                                                     [](int i) { O().decoLevel = i - 1; save(); }));
    form->heading("Triggers");
    form->row("Camera (zoom / offset)", ui::ToggleControl::create(O().triggerCamera, [](bool on) { O().triggerCamera = on; save(); }));
    form->row("Beat pulse and move", ui::ToggleControl::create(O().triggerPulse, [](bool on) { O().triggerPulse = on; save(); }));
    form->row("Colours per section", ui::ToggleControl::create(O().triggerColors, [](bool on) { O().triggerColors = on; save(); }));
    form->row("Shake on drops", ui::ToggleControl::create(O().triggerShake, [](bool on) { O().triggerShake = on; save(); }));
    form->row("Use the trained model", ui::ToggleControl::create(S().useModel(), [](bool on) { S().setUseModel(on); save(); }),
              S().models().empty() ? "no model found: rules" : "off = rule-based");
    return form;
}

// ============================================================== Gamemodes

CCNode* AutoLayoutPopup::buildGamemodesTab() {
    auto page = CCNode::create();
    page->setContentSize(m_contentSize);
    const float half = m_contentSize.width / 2.f;
    auto left = ui::Form::create({half, m_contentSize.height}, 64.f, 22.f);
    page->addChild(left);
    left->heading("Gamemode weights (auto = AI)");
    for (int m = 0; m < 8; ++m) {
        float w = O().modeWeights[size_t(m)];
        left->row(kModeNames[m], ui::SliderControl::create(w < 0 ? 0.5f : w, 150.f,
            [m](float v) { return O().modeWeights[size_t(m)] < 0 ? std::string("auto") : (v < 0.01f ? std::string("never") : pct(v)); },
            [m](float v) { O().modeWeights[size_t(m)] = v; save(); }));
    }
    auto resetMenu = CCMenu::create();
    resetMenu->setPosition({0.f, 0.f});
    auto reset = ui::button("All auto", [this] {
        O().modeWeights.fill(-1.f);
        save();
        defer([this] { rebuildTab(Tab::Gamemodes); });
    }, 0.4f, "GJ_button_04.png");
    reset->setPosition({half / 2.f, 12.f});
    resetMenu->addChild(reset);
    left->addChild(resetMenu);

    auto right = ui::Form::create({half, m_contentSize.height}, 110.f, 22.f);
    right->setPosition({half, 0.f});
    page->addChild(right);
    right->heading("Changes");
    right->row("Speed changes", ui::CycleControl::create({"Auto", "Off", "Subtle", "Aggressive"}, int(O().speedChanges), 110.f,
                                                         [](int i) { O().speedChanges = gen::SpeedChanges(i); save(); }));
    right->row("Start speed", ui::CycleControl::create({"Auto", "0.5x", "1x", "2x", "3x", "4x"}, O().startSpeed + 1, 110.f,
                                                       [](int i) { O().startSpeed = i - 1; save(); }));
    right->row("Portal frequency", ui::SliderControl::create(O().portalFrequency < 0 ? 0.5f : O().portalFrequency, 120.f,
        [](float v) { return O().portalFrequency < 0 ? std::string("auto") : pct(v); },
        [](float v) { O().portalFrequency = v; save(); }));
    std::vector<std::string> tri{"Auto", "Off", "On"};
    right->row("Gravity flips", ui::CycleControl::create(tri, int(O().gravityFlips), 110.f, [](int i) { O().gravityFlips = gen::Tri(i); save(); }));
    right->row("Mini / big", ui::CycleControl::create(tri, int(O().sizeChanges), 110.f, [](int i) { O().sizeChanges = gen::Tri(i); save(); }));
    right->row("Dual", ui::CycleControl::create(tri, int(O().dual), 110.f, [](int i) { O().dual = gen::Tri(i); save(); }));
    right->row("Hold vs tap", ui::SliderControl::create((O().holdBias + 1.f) / 2.f, 120.f,
        [](float v) { return v < 0.45f ? std::string("taps") : v > 0.55f ? std::string("holds") : std::string("balanced"); },
        [](float v) { O().holdBias = v * 2.f - 1.f; save(); }));
    return page;
}

// ============================================================== Sections

CCNode* AutoLayoutPopup::buildSectionsTab() {
    auto form = ui::Form::create(m_contentSize, 90.f, 24.f);
    form->heading("Per section: difficulty and gamemode (auto = AI)");
    auto find = [](int type) -> gen::SectionOverride& {
        for (auto& o : O().sectionOverrides) if (o.sectionType == type) return o;
        O().sectionOverrides.push_back({type, 0, -1});
        return O().sectionOverrides.back();
    };
    std::vector<std::string> deltas{"-3", "-2", "-1", "same", "+1", "+2", "+3"};
    std::vector<std::string> modes{"Auto"};
    for (auto m : kModeNames) modes.push_back(m);
    for (int t = 0; t < 7; ++t) {
        auto row = CCNode::create();
        row->setContentSize({300.f, 20.f});
        int delta = 0, forced = -1;
        for (auto& o : O().sectionOverrides) if (o.sectionType == t) { delta = o.difficultyDelta; forced = o.forcedMode; }
        auto dc = ui::CycleControl::create(deltas, delta + 3, 100.f, [t, find](int i) { find(t).difficultyDelta = i - 3; save(); });
        dc->setPosition({0.f, 10.f});
        row->addChild(dc);
        auto mc = ui::CycleControl::create(modes, forced + 1, 110.f, [t, find](int i) { find(t).forcedMode = i - 1; save(); });
        mc->setPosition({112.f, 10.f});
        row->addChild(mc);
        form->row(kSectionNames[t], row);
    }
    form->text("Sections are found in the song automatically (intro, verse, chorus, build-up, drop, break, outro). "
               "\"+2\" on Drop makes every drop two levels harder than the rest.");
    return form;
}

// ============================================================== Playtest

CCNode* AutoLayoutPopup::buildPlaytestTab() {
    auto page = CCNode::create();
    page->setContentSize(m_contentSize);
    const float W = m_contentSize.width, H = m_contentSize.height;
    auto menu = CCMenu::create();
    menu->setPosition({0.f, 0.f});
    page->addChild(menu);
    auto verify = ui::button("Bot-verify the editor level", [this] {
        if (!m_editor || S().busy()) return;
        m_watching = S().startPlaytest(EditorBridge::get().levelString(m_editor));
        if (m_playtestLabel) m_playtestLabel->setString("Starting...");
    }, 0.5f);
    verify->setPosition({W / 2.f - 110.f, H - 14.f});
    menu->addChild(verify);
    auto ghost = ui::button("Ghost on / off", [this] {
        auto cur = S().current();
        if (!m_editor) return;
        if (ui::ghostVisible(m_editor)) ui::hideGhost(m_editor);
        else if (cur) ui::showGhost(m_editor, cur->level, cur->inputs);
    }, 0.5f, "GJ_button_02.png");
    ghost->setPosition({W / 2.f + 60.f, H - 14.f});
    menu->addChild(ghost);
    auto play = ui::button("Play ghost", [this] { if (m_editor) ui::playGhost(m_editor); }, 0.5f, "GJ_button_02.png");
    play->setPosition({W / 2.f + 160.f, H - 14.f});
    menu->addChild(play);
    m_playtestLabel = ui::label("Verify runs the bot on the live editor level (your edits included).", 0.3f);
    m_playtestLabel->setPosition({W / 2.f, H - 34.f});
    page->addChild(m_playtestLabel);

    auto cur = S().current();
    std::string text;
    if (!cur) {
        text = "Generate a layout to see its playability report here.";
    } else {
        auto const& r = cur->report;
        text = fmt::format("Playability report (variant {}, seed {}):\n", S().selected() + 1, cur->seed);
        text += fmt::format("{} | min window {:.0f} ms | longest gap without input {:.1f} s at {:.1f} s\n",
                            r.beatable ? "beatable" : "NOT beatable", r.minWindowMs, r.longestGap, r.longestGapAt);
        text += "Risky spots (tightest timing):";
        int n = 0;
        for (auto& rs : r.risky) {
            if (n++ >= 6) break;
            text += fmt::format("\n   {:6.2f} s   x {:6.0f}   window {:3.0f} ms   {}", rs.time, rs.x, rs.windowMs, rs.pattern);
        }
        if (r.risky.empty()) text += " none";
        for (auto& note : r.notes) text += "\n" + note;
    }
    auto report = CCLabelBMFont::create(text.c_str(), "chatFont.fnt", (W * 0.58f) / 0.5f, kCCTextAlignmentLeft);
    report->setScale(0.5f);
    report->setAnchorPoint({0.f, 1.f});
    report->setPosition({4.f, H - 48.f});
    page->addChild(report);

    // difficulty curve vs music intensity
    const float gx = W * 0.6f, gw = W * 0.4f - 6.f, gy = 10.f, gh = H - 70.f;
    auto bg = CCLayerColor::create({0, 0, 0, 90}, gw, gh);
    bg->setPosition({gx, gy});
    page->addChild(bg);
    m_curve = CCDrawNode::create();
    page->addChild(m_curve);
    auto title = ui::label("difficulty (red) vs music intensity (blue)", 0.22f);
    title->setPosition({gx + gw / 2.f, gy + gh + 6.f});
    page->addChild(title);
    if (cur && !cur->report.difficultyCurve.empty()) {
        auto plot = [&](std::vector<std::pair<double, float>> const& c, ccColor4F col) {
            double tmax = std::max(1.0, c.back().first);
            for (size_t i = 1; i < c.size(); ++i) {
                CCPoint a{gx + float(c[i - 1].first / tmax) * gw, gy + 4.f + c[i - 1].second * (gh - 8.f)};
                CCPoint b{gx + float(c[i].first / tmax) * gw, gy + 4.f + c[i].second * (gh - 8.f)};
                m_curve->drawSegment(a, b, 0.8f, col);
            }
        };
        plot(cur->report.intensityCurve, {0.35f, 0.6f, 1.f, 0.9f});
        plot(cur->report.difficultyCurve, {1.f, 0.35f, 0.35f, 1.f});
    }
    return page;
}

// ============================================================== Training

CCNode* AutoLayoutPopup::buildTrainingTab() {
    auto form = ui::Form::create(m_contentSize, 150.f, 22.f);
    form->heading("Models");
    form->text(S().modelSummary());
    auto modelMenu = CCMenu::create();
    modelMenu->setContentSize({260.f, 20.f});
    modelMenu->ignoreAnchorPointForPosition(false);
    auto reload = ui::button("Reload models", [this] {
        if (S().busy()) return;
        int n = S().reloadModels();
        Notification::create(fmt::format("AutoLayout: {} model(s) loaded", n), NotificationIcon::Info)->show();
        defer([this] { rebuildTab(Tab::Training); });
    }, 0.4f, "GJ_button_04.png");
    reload->setPosition({45.f, 10.f});
    modelMenu->addChild(reload);
    auto modelsFolder = ui::button("Model folder", [] { openFolder(Mod::get()->getSaveDir() / "models"); }, 0.4f, "GJ_button_04.png");
    modelsFolder->setPosition({140.f, 10.f});
    modelMenu->addChild(modelsFolder);
    form->row("Retrained models", modelMenu);

    form->heading("Dataset (style learning from real levels)");
    static int s_diff = 4, s_style = 0;
    int count = 0;
    std::error_code ec;
    auto dir = EditorBridge::datasetDir();
    for (auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) if (e.path().extension() == ".gmd") ++count;
    form->text(fmt::format("{} level(s) in the dataset folder. Aim for 60 (30 minimum, 100 better), spread over the difficulties.", count));
    auto diffs = difficultyLabels();
    diffs.erase(diffs.begin()); // no "auto" for a label
    form->row("This level's difficulty", ui::CycleControl::create(diffs, s_diff, 150.f, [](int i) { s_diff = i; }));
    std::vector<std::string> styles{"Unlabelled"};
    for (int i = 1; i < int(gen::Style::Count); ++i) styles.push_back(gen::styleName(gen::Style(i)));
    form->row("This level's style", ui::CycleControl::create(styles, s_style, 150.f, [](int i) { s_style = i; }));
    auto dsMenu = CCMenu::create();
    dsMenu->setContentSize({260.f, 20.f});
    dsMenu->ignoreAnchorPointForPosition(false);
    auto add = ui::button("Add this level", [this] {
        if (!m_editor) return;
        auto song = S().levelSong(m_editor->m_level);
        auto res = EditorBridge::get().addToDataset(m_editor, song.isOk() ? song.unwrap().path : std::filesystem::path(), s_diff, s_style);
        if (res.isErr()) Notification::create(res.unwrapErr(), NotificationIcon::Error)->show();
        else Notification::create("Added to the dataset: " + res.unwrap().filename().string(), NotificationIcon::Success)->show();
        defer([this] { rebuildTab(Tab::Training); });
    }, 0.4f, "GJ_button_01.png");
    add->setPosition({45.f, 10.f});
    dsMenu->addChild(add);
    auto openDs = ui::button("Open folder", [] { openFolder(EditorBridge::datasetDir()); }, 0.4f, "GJ_button_04.png");
    openDs->setPosition({140.f, 10.f});
    dsMenu->addChild(openDs);
    form->row("Dataset", dsMenu);
    form->text("Retrain on a computer with Python 3 + PyTorch (see training/RETRAINING.md):\n"
               "  aldata extract-dir <dataset folder> --out real.jsonl\n"
               "  python3 training/train_patterns.py --data real.jsonl selfplay.jsonl --per-style\n"
               "then copy the .alm files into the model folder and press Reload models.", 0.45f);
    return form;
}

// ============================================================== Settings

CCNode* AutoLayoutPopup::buildSettingsTab() {
    auto form = ui::Form::create(m_contentSize, 150.f, 22.f);
    form->row("Back up the level before applying",
              ui::ToggleControl::create(Mod::get()->getSettingValue<bool>("backup-before-apply"),
                                        [](bool on) { Mod::get()->setSettingValue<bool>("backup-before-apply", on); }));
    form->row("Retries per section", ui::CycleControl::create({"8", "16", "24", "40"}, O().maxRetriesPerSection <= 8 ? 0 : O().maxRetriesPerSection <= 16 ? 1 : O().maxRetriesPerSection <= 24 ? 2 : 3,
                                                              110.f, [](int i) { static const int v[] = {8, 16, 24, 40}; O().maxRetriesPerSection = v[i]; save(); }));
    form->heading("Presets");
    auto name = TextInput::create(140.f, "preset name", "bigFont.fnt");
    name->setCommonFilter(CommonFilter::Name);
    name->setScale(0.7f);
    auto presetRow = CCMenu::create();
    presetRow->setContentSize({280.f, 20.f});
    presetRow->ignoreAnchorPointForPosition(false);
    name->setPosition({50.f, 10.f});
    presetRow->addChild(name);
    auto savePreset = ui::button("Save", [name, this] {
        auto r = S().savePreset(std::string(name->getString()));
        if (r.isErr()) Notification::create(r.unwrapErr(), NotificationIcon::Error)->show();
        else Notification::create("Preset saved", NotificationIcon::Success)->show();
        defer([this] { rebuildTab(Tab::Settings); });
    }, 0.4f, "GJ_button_01.png");
    savePreset->setPosition({130.f, 10.f});
    presetRow->addChild(savePreset);
    form->row("Save current options", presetRow);
    auto list = S().presets();
    static int s_preset = 0;
    s_preset = std::clamp(s_preset, 0, std::max(0, int(list.size()) - 1));
    auto loadRow = CCNode::create();
    loadRow->setContentSize({280.f, 20.f});
    auto cyc = ui::CycleControl::create(list.empty() ? std::vector<std::string>{"(no presets)"} : list, s_preset, 140.f, [](int i) { s_preset = i; });
    cyc->setPosition({0.f, 10.f});
    loadRow->addChild(cyc);
    auto lm = CCMenu::create();
    lm->setPosition({0.f, 0.f});
    auto loadBtn = ui::button("Load", [this, list] {
        if (list.empty()) return;
        auto r = S().loadPreset(list[size_t(s_preset)]);
        if (r.isErr()) Notification::create(r.unwrapErr(), NotificationIcon::Error)->show();
        else {
            Notification::create("Preset loaded", NotificationIcon::Success)->show();
            defer([this] { for (int t = 0; t < int(Tab::Count); ++t) rebuildTab(Tab(t)); });
        }
    }, 0.4f, "GJ_button_01.png");
    loadBtn->setPosition({170.f, 10.f});
    lm->addChild(loadBtn);
    loadRow->addChild(lm);
    form->row("Load a preset", loadRow);
    auto misc = CCMenu::create();
    misc->setContentSize({280.f, 20.f});
    misc->ignoreAnchorPointForPosition(false);
    auto reset = ui::button("Reset all options", [this] {
        uint64_t seed = O().seed;
        O() = gen::GenOptions();
        O().seed = seed;
        save();
        defer([this] { for (int t = 0; t < int(Tab::Count); ++t) rebuildTab(Tab(t)); });
    }, 0.4f, "GJ_button_06.png");
    reset->setPosition({50.f, 10.f});
    misc->addChild(reset);
    auto saveDir = ui::button("Save folder", [] { openFolder(Mod::get()->getSaveDir()); }, 0.4f, "GJ_button_04.png");
    saveDir->setPosition({150.f, 10.f});
    misc->addChild(saveDir);
    form->row("Options", misc);
    form->text("Backups, recordings, presets, the dataset and retrained models live in the save folder.");
    return form;
}

} // namespace al::mod
