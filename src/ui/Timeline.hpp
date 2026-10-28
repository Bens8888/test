#pragma once
// Preview of a generated layout: an overview of the whole song (waveform, sections, beats, the
// clicks coloured by musical role, dropped clicks in grey) and a detail view of a time window:
// the level itself (rendered by the core renderer) with the bot's path through it.
#include <Geode/Geode.hpp>

#include <al/audio/analysis.hpp>
#include <al/gen/generator.hpp>

#include <memory>
#include <vector>

namespace al::mod::ui {

class Timeline : public cocos2d::CCNode {
public:
    static Timeline* create(cocos2d::CCSize size);
    void setData(std::shared_ptr<const audio::MusicAnalysis> A, gen::GenResult const* result);
    // Detail window: start time (s) and length (s).
    void setView(double t0, double span);
    double viewStart() const { return m_t0; }
    double viewSpan() const { return m_span; }
    double duration() const;
    void setHighlightedPart(int part);
    static cocos2d::ccColor4F roleColor(gen::Role r);

private:
    bool init(cocos2d::CCSize size);
    void redraw();
    void rebuildDetailImage();
    float xAtTime(double t) const;  // level x of the player at song time t (from the designed run)

    cocos2d::CCDrawNode* m_overview = nullptr;
    cocos2d::CCDrawNode* m_overlay = nullptr;
    cocos2d::CCNode* m_detailHolder = nullptr;
    cocos2d::CCLabelBMFont* m_info = nullptr;
    std::shared_ptr<const audio::MusicAnalysis> m_A;
    gen::GenResult const* m_result = nullptr;
    std::vector<float> m_xAtTick;   // player x per tick of the designed run
    double m_t0 = 0, m_span = 12;
    int m_highlight = -1;
    float m_overviewH = 0, m_detailH = 0;
};

} // namespace al::mod::ui
