#pragma once
// The AutoLayout popup (editor): Generate (song, input source, difficulty, seed, variants,
// progress/cancel, preview, variants, re-roll, apply/undo), Style, Gamemodes, Sections, Playtest,
// Training and Settings. Every option defaults to "let the AI decide".
#include <Geode/Geode.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace al::mod {

struct Job;
namespace ui {
class Timeline;
class CycleControl;
class SliderControl;
} // namespace ui

class AutoLayoutPopup : public geode::Popup {
public:
    static AutoLayoutPopup* create(LevelEditorLayer* lel);
    static AutoLayoutPopup* current();
    void onPicked(std::filesystem::path const& p, int what); // file picker results (main thread)

protected:
    enum class Tab { Generate = 0, Style, Gamemodes, Sections, Playtest, Training, Settings, Count };

    bool init(LevelEditorLayer* lel);
    void onClose(cocos2d::CCObject* sender) override;
    void onTab(cocos2d::CCObject* sender);
    void selectTab(Tab tab);
    void onTick(float dt);

    cocos2d::CCNode* buildGenerateTab();
    cocos2d::CCNode* buildStyleTab();
    cocos2d::CCNode* buildGamemodesTab();
    cocos2d::CCNode* buildSectionsTab();
    cocos2d::CCNode* buildPlaytestTab();
    cocos2d::CCNode* buildTrainingTab();
    cocos2d::CCNode* buildSettingsTab();
    void rebuildTab(Tab tab);
    void defer(std::function<void()> f);

    // generate tab
    void startGenerate();
    void showResults(bool on);
    void refreshResults();
    void refreshSongLabel();
    void refreshRecordingLabel();
    void applySelected();
    void undoLast();
    void setStatus(std::string const& text);
    void onJobFinished(std::shared_ptr<Job> const& job);

    LevelEditorLayer* m_editor = nullptr;
    cocos2d::CCSize m_contentSize;
    std::array<geode::TabButton*, size_t(Tab::Count)> m_tabButtons{};
    std::array<cocos2d::CCNode*, size_t(Tab::Count)> m_tabPages{};
    Tab m_tab = Tab::Generate;

    // generate: setup view
    cocos2d::CCNode* m_setupView = nullptr;
    cocos2d::CCLabelBMFont* m_songLabel = nullptr;
    cocos2d::CCLabelBMFont* m_recordingLabel = nullptr;
    cocos2d::CCLabelBMFont* m_tapsLabel = nullptr;
    geode::TextInput* m_seedInput = nullptr;
    // generate: progress
    cocos2d::CCNode* m_progressNode = nullptr;
    cocos2d::CCLayerColor* m_progressBar = nullptr;
    cocos2d::CCLabelBMFont* m_progressLabel = nullptr;
    cocos2d::CCMenu* m_generateMenu = nullptr;
    // generate: results view
    cocos2d::CCNode* m_resultView = nullptr;
    ui::Timeline* m_timeline = nullptr;
    ui::CycleControl* m_variantCycle = nullptr;
    ui::CycleControl* m_sectionCycle = nullptr;
    ui::SliderControl* m_viewSlider = nullptr;
    cocos2d::CCLabelBMFont* m_reportLabel = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    int m_rerollPart = 0;

    // playtest
    cocos2d::CCLabelBMFont* m_playtestLabel = nullptr;
    cocos2d::CCDrawNode* m_curve = nullptr;

    std::shared_ptr<Job> m_watching;
};

} // namespace al::mod
