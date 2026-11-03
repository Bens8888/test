#pragma once
// The AutoLayout popup (editor): one page. The song, where the clicks come from, difficulty, speed
// and structure style; Generate, then Place in level (or a new layout, or remove the placed one).
// Everything else is decided by the generator.
#include <Geode/Geode.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace al::mod {

struct Job;

class AutoLayoutPopup : public geode::Popup {
public:
    static AutoLayoutPopup* create(LevelEditorLayer* lel);
    static AutoLayoutPopup* current();
    void onPicked(std::filesystem::path const& p, int what); // file picker results (main thread)

    // driven by the in-game self test (hooks/SelfTest.cpp)
    void testGenerate() { startGenerate(); }
    void testApply() { applyLayout(); }
    void testUndo() { removeLayout(); }
    bool testBusy() const { return m_watching != nullptr; }

protected:
    bool init(LevelEditorLayer* lel);
    void onClose(cocos2d::CCObject* sender) override;
    void onTick(float dt);
    void build();
    void defer(std::function<void()> f);

    void startGenerate();
    void applyLayout();
    void removeLayout();
    void refreshSong();
    void refreshClicks();
    void refreshState();
    void setStatus(std::string const& text);
    void onJobFinished(std::shared_ptr<Job> const& job);

    LevelEditorLayer* m_editor = nullptr;
    cocos2d::CCNode* m_page = nullptr;
    cocos2d::CCLabelBMFont* m_songLabel = nullptr;
    cocos2d::CCLabelBMFont* m_clicksLabel = nullptr;
    cocos2d::CCMenu* m_clicksMenu = nullptr;
    cocos2d::CCMenu* m_actions = nullptr;
    cocos2d::CCNode* m_progress = nullptr;
    cocos2d::CCLayerColor* m_progressBar = nullptr;
    cocos2d::CCLabelBMFont* m_progressLabel = nullptr;
    cocos2d::CCLabelBMFont* m_status = nullptr;
    bool m_placed = false;
    std::shared_ptr<Job> m_watching;
};

} // namespace al::mod
