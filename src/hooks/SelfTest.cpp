// In-game self test: plays a generated level in the real game with its designed clicks and logs
// the player at every physics step, so the simulator can be compared with GD. It only runs when
// selftest.txt is in the mod's save folder (first line: level string file, second: inputs, one
// "tick press" per line). It writes selftest-trace.tsv and selftest-result.txt next to it, and
// selftest-rec.txt with what the recorder caught of the run.
// First line "ui", second a song: opens the editor, presses the AutoLayout button, goes through every
// tab, generates, applies, undoes, and writes selftest-ui.txt and a tab-N.ppm screenshot per tab.
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "../bridge/EditorBridge.hpp"
#include "../recorder/LiveRecorder.hpp"
#include "../session/Session.hpp"
#include "../ui/AutoLayoutPopup.hpp"
#include "../ui/TapAlongPopup.hpp"

#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

using namespace geode::prelude;

namespace {

struct SelfTest {
    bool started = false;
    bool active = false;
    int attempts = 0;
    std::map<int, bool> inputs;
    std::ofstream trace;
    bool ui = false;
    int step = 0, wait = 0, before = 0;
    std::string song;
    std::ofstream log;
};

SelfTest& test() {
    static SelfTest t;
    return t;
}

void finish(const std::string& outcome, PlayerObject* player, int tick) {
    auto& t = test();
    if (!t.active) return;
    t.active = false;
    t.trace.close();
    std::ofstream out(Mod::get()->getSaveDir() / "selftest-result.txt");
    out << outcome << " tick " << tick;
    if (player) out << " x " << player->getPositionX() << " y " << player->getPositionY();
    out << "\n";
}

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

bool startTest() {
    auto& t = test();
    if (t.started) return false;
    const auto cfg = Mod::get()->getSaveDir() / "selftest.txt";
    std::ifstream f(cfg);
    if (!f) return false;
    t.started = true;
    std::string levelPath, inputsPath;
    std::getline(f, levelPath);
    std::getline(f, inputsPath);
    if (levelPath == "ui") {
        t.ui = true;
        t.song = inputsPath;
        t.log.open(Mod::get()->getSaveDir() / "selftest-ui.txt");
        auto level = GJGameLevel::create();
        level->m_levelName = "AutoLayout self test";
        level->m_levelType = GJLevelType::Editor;
        CCDirector::get()->replaceScene(LevelEditorLayer::scene(level, false));
        return true;
    }
    al::mod::LiveRecorder::get().setArmed(true);
    const std::string text = readAll(levelPath);
    if (text.empty()) {
        finish("no level", nullptr, 0);
        return false;
    }
    std::ifstream in(inputsPath);
    for (std::string line; std::getline(in, line);) {
        int tick = 0, press = 0;
        if (std::sscanf(line.c_str(), "%d %d", &tick, &press) == 2) t.inputs[tick] = press != 0;
    }
    t.trace.open(Mod::get()->getSaveDir() / "selftest-trace.tsv");
    t.trace << std::fixed << std::setprecision(3) << "tick\thold\tx\ty\tvy\n";
    t.active = true;
    auto level = GJGameLevel::create();
    level->m_levelName = "AutoLayout self test";
    level->m_levelType = GJLevelType::Editor;
    level->m_levelString = text;
    CCDirector::get()->replaceScene(PlayLayer::scene(level, false, false));
    return true;
}

void say(std::string const& text) {
    auto& t = test();
    t.log << text << std::endl;
    log::info("self test: {}", text);
}

void screenshot(std::string const& name) {
    auto size = CCDirector::get()->getWinSize();
    auto rt = CCRenderTexture::create(int(size.width), int(size.height));
    auto scene = CCDirector::get()->getRunningScene();
    if (!rt || !scene) return;
    rt->begin();
    scene->visit();
    rt->end();
    auto img = rt->newCCImage(true);
    if (!img) return;
    std::ofstream out(Mod::get()->getSaveDir() / (name + ".ppm"), std::ios::binary);
    const int w = img->getWidth(), h = img->getHeight();
    out << "P6\n" << w << ' ' << h << "\n255\n";
    const unsigned char* px = img->getData();
    for (int i = 0; i < w * h; ++i) out.write(reinterpret_cast<const char*>(px + i * 4), 3);
    delete img;
}

void reportRecording(GJBaseGameLayer* layer, bool completed) {
    auto& rec = al::mod::LiveRecorder::get();
    rec.endSession(layer, completed);
    std::ofstream out(Mod::get()->getSaveDir() / "selftest-rec.txt");
    if (!rec.last()) { out << "no recording\n"; return; }
    out << "completed " << rec.last()->completed << " events " << rec.last()->events.size() << "\n";
    for (auto const& e : rec.last()->events) out << e.tick << ' ' << int(e.press) << "\n";
}

// One step of the menu test, every half second on the editor.
void uiStep(EditorUI* editor) {
    auto& t = test();
    using al::mod::AutoLayoutPopup;
    auto& session = al::mod::Session::get();
    auto& bridge = al::mod::EditorBridge::get();
    auto popup = AutoLayoutPopup::current();
    const int s = t.step++;
    if (s == 0) {
        auto menu = editor->getChildByID("settings-menu");
        auto button = menu ? typeinfo_cast<CCMenuItemSpriteExtra*>(menu->getChildByID("autolayout-button"_spr)) : nullptr;
        say(fmt::format("editor button found: {}", button != nullptr));
        if (button) button->activate();
        return;
    }
    if (!popup) {
        say(fmt::format("no popup at step {}", s));
        finish("ui-failed", nullptr, s);
        t.ui = false;
        return;
    }
    if (s <= AutoLayoutPopup::kTabCount) {
        // a tab is drawn the step after it is selected
        if (s > 1) screenshot(fmt::format("tab-{}", s - 2));
        if (s < AutoLayoutPopup::kTabCount + 1) popup->testTab(s - 1);
        say(fmt::format("tab {}", s - 1));
        return;
    }
    if (s == AutoLayoutPopup::kTabCount + 1) {
        screenshot(fmt::format("tab-{}", AutoLayoutPopup::kTabCount - 1));
        popup->testTab(0);
        session.setCustomSong(t.song);
        session.options().variants = 2;
        popup->testGenerate();
        say(fmt::format("generating from {}", t.song));
        return;
    }
    if (s == AutoLayoutPopup::kTabCount + 2) {
        if (popup->testBusy() && ++t.wait < 240) { t.step = s; return; }
        auto cur = session.current();
        say(fmt::format("generated: {} variants, waited {} steps, beatable {}, objects {}", session.variants().size(), t.wait,
                        cur ? cur->report.beatable : false, cur ? cur->level.objects.size() : 0));
        screenshot("results");
        return;
    }
    if (s == AutoLayoutPopup::kTabCount + 3) {
        t.before = bridge.gameplayObjectCount(editor->m_editorLayer);
        popup->testApply();
        say(fmt::format("apply: {} gameplay objects before", t.before));
        return;
    }
    if (s == AutoLayoutPopup::kTabCount + 4) {
        const int after = bridge.gameplayObjectCount(editor->m_editorLayer);
        say(fmt::format("applied: {} gameplay objects, level string {} bytes", after, bridge.levelString(editor->m_editorLayer).size()));
        screenshot("applied");
        popup->testUndo();
        return;
    }
    if (s == AutoLayoutPopup::kTabCount + 5) {
        say(fmt::format("undone: {} gameplay objects", bridge.gameplayObjectCount(editor->m_editorLayer)));
        if (auto tap = al::mod::TapAlongPopup::create(editor->m_editorLayer, [] {})) tap->show();
        return;
    }
    if (s == AutoLayoutPopup::kTabCount + 6) {
        screenshot("tap-along");
        say("tap along opened");
        t.ui = false;
        t.active = true;
        finish("ui-done", nullptr, s);
    }
}

} // namespace

class $modify(ALTestEditor, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;
        if (test().ui) this->schedule(schedule_selector(ALTestEditor::onTestStep), 0.5f);
        return true;
    }

    void onTestStep(float) {
        if (test().ui) uiStep(this);
    }
};

class $modify(ALTestMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        Loader::get()->queueInMainThread([] { startTest(); });
        return true;
    }
};

class $modify(ALTestBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& t = test();
        if (t.active) {
            // the game counts two per physics step, and takes a button in the step after this one
            // (a spider or a swing on the ground reacts at once; a swing keeps the flag for a while
            // after it touched a bound, so only one standing still counts)
            const bool atOnce = m_player1 && m_player1->m_isOnGround && (m_player1->m_isSpider || (m_player1->m_isSwing && m_player1->m_yVelocity == 0));
            const int step = int(m_gameState.m_currentProgress / 2) + (atOnce ? 0 : 1);
            while (!t.inputs.empty() && t.inputs.begin()->first <= step) {
                handleButton(t.inputs.begin()->second, 1, true);
                t.inputs.erase(t.inputs.begin());
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        // a look at the level as it plays, every five seconds
        if (t.active && !t.ui && int(m_gameState.m_currentProgress) % 2400 == 0 && m_gameState.m_currentProgress > 0 && m_gameState.m_currentProgress <= 96000)
            screenshot(fmt::format("play-{}", int(m_gameState.m_currentProgress) / 2400));
        if (t.active && m_player1 && m_gameState.m_currentProgress >= 4) {
            t.trace << int(m_gameState.m_currentProgress / 2 - 1) << "\t0\t" << m_player1->getPositionX() << '\t' << m_player1->getPositionY() << '\t'
                    << m_player1->m_yVelocity << '\n';
        }
    }
};

class $modify(ALTestPlay, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        const bool probe = object && object == m_anticheatSpike;
        const bool running = test().active && !probe;
        if (!probe) finish("died", player, int(m_gameState.m_currentProgress / 2));
        PlayLayer::destroyPlayer(player, object);
        if (running) reportRecording(this, false);
    }

    void levelComplete() {
        const bool running = test().active;
        finish("complete", m_player1, int(m_gameState.m_currentProgress / 2));
        PlayLayer::levelComplete();
        if (running) reportRecording(this, true);
    }
};
