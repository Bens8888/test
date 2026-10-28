#include "Timeline.hpp"

#include <al/bot/bot.hpp>
#include <al/physics/sim.hpp>
#include <al/report/render.hpp>

using namespace geode::prelude;

namespace al::mod::ui {

namespace {
ccColor4F rgba(float r, float g, float b, float a = 1.f) { return {r, g, b, a}; }

ccColor4F sectionColor(audio::SectionType t) {
    switch (t) {
        case audio::SectionType::Intro: return rgba(0.35f, 0.55f, 0.85f, 0.35f);
        case audio::SectionType::Verse: return rgba(0.30f, 0.75f, 0.45f, 0.35f);
        case audio::SectionType::Chorus: return rgba(0.85f, 0.65f, 0.25f, 0.35f);
        case audio::SectionType::BuildUp: return rgba(0.95f, 0.75f, 0.20f, 0.40f);
        case audio::SectionType::Drop: return rgba(0.95f, 0.30f, 0.30f, 0.45f);
        case audio::SectionType::Break: return rgba(0.55f, 0.45f, 0.85f, 0.35f);
        case audio::SectionType::Outro: return rgba(0.45f, 0.45f, 0.55f, 0.35f);
    }
    return rgba(0.5f, 0.5f, 0.5f, 0.3f);
}
} // namespace

ccColor4F Timeline::roleColor(gen::Role r) {
    switch (r) {
        case gen::Role::Downbeat: return rgba(1.f, 0.25f, 0.25f);
        case gen::Role::Kick: return rgba(1.f, 0.55f, 0.15f);
        case gen::Role::Snare: return rgba(1.f, 0.9f, 0.2f);
        case gen::Role::HiHat: return rgba(0.3f, 0.9f, 1.f);
        case gen::Role::Melody: return rgba(0.35f, 1.f, 0.45f);
        case gen::Role::Sustain: return rgba(0.35f, 0.55f, 1.f);
        case gen::Role::Syncopation: return rgba(0.8f, 0.4f, 1.f);
        case gen::Role::Beat: return rgba(0.95f, 0.95f, 0.95f);
        case gen::Role::Fill: return rgba(0.65f, 0.65f, 0.65f);
        case gen::Role::Recorded: return rgba(1.f, 0.45f, 0.8f);
    }
    return rgba(1, 1, 1);
}

Timeline* Timeline::create(CCSize size) {
    auto r = new Timeline();
    if (r->init(size)) {
        r->autorelease();
        return r;
    }
    delete r;
    return nullptr;
}

bool Timeline::init(CCSize size) {
    if (!CCNode::init()) return false;
    this->setContentSize(size);
    this->setAnchorPoint({0.5f, 0.5f});
    m_overviewH = std::floor(size.height * 0.38f);
    m_detailH = size.height - m_overviewH - 4.f;
    auto bg = CCLayerColor::create({0, 0, 0, 90}, size.width, size.height);
    this->addChild(bg, -1);
    m_detailHolder = CCNode::create();
    m_detailHolder->setContentSize({size.width, m_detailH});
    this->addChild(m_detailHolder, 0);
    m_overview = CCDrawNode::create();
    this->addChild(m_overview, 1);
    m_overlay = CCDrawNode::create();
    this->addChild(m_overlay, 2);
    m_info = CCLabelBMFont::create("Generate a layout to see the preview.", "chatFont.fnt");
    m_info->setScale(0.55f);
    m_info->setPosition({size.width / 2.f, m_detailH / 2.f});
    this->addChild(m_info, 3);
    return true;
}

double Timeline::duration() const { return m_A ? std::max(1.0, m_A->duration) : 1.0; }

void Timeline::setData(std::shared_ptr<const audio::MusicAnalysis> A, gen::GenResult const* result) {
    m_A = std::move(A);
    m_result = result;
    m_xAtTick.clear();
    if (m_result) {
        phys::World W(m_result->level);
        bot::RunOptions ro;
        ro.recordTrace = true;
        auto run = bot::run(W, m_result->inputs, ro);
        m_xAtTick.reserve(run.trace.size());
        for (auto& p : run.trace) m_xAtTick.push_back(p.x);
    }
    m_span = std::min(m_span, duration());
    m_t0 = std::clamp(m_t0, 0.0, std::max(0.0, duration() - m_span));
    redraw();
    rebuildDetailImage();
}

void Timeline::setView(double t0, double span) {
    m_span = std::clamp(span, 2.0, duration());
    m_t0 = std::clamp(t0, 0.0, std::max(0.0, duration() - m_span));
    redraw();
    rebuildDetailImage();
}

void Timeline::setHighlightedPart(int part) {
    m_highlight = part;
    redraw();
}

float Timeline::xAtTime(double t) const {
    if (m_xAtTick.empty()) return float(t * 311.58);
    int k = int(std::lround(t * phys::kTPS)) - 1;
    k = std::clamp(k, 0, int(m_xAtTick.size()) - 1);
    float x = m_xAtTick[size_t(k)];
    int over = int(std::lround(t * phys::kTPS)) - 1 - int(m_xAtTick.size() - 1);
    if (over > 0) x += float(over) * 1.3f;
    return x;
}

void Timeline::redraw() {
    m_overview->clear();
    m_overlay->clear();
    const float W = this->getContentWidth();
    const float y0 = m_detailH + 4.f, H = m_overviewH;
    if (!m_A) return;
    const double dur = duration();
    auto X = [&](double t) { return float(t / dur) * W; };
    // sections
    for (auto& s : m_A->sections) {
        CCPoint v[4] = {{X(s.start), y0}, {X(s.end), y0}, {X(s.end), y0 + H}, {X(s.start), y0 + H}};
        m_overview->drawPolygon(v, 4, sectionColor(s.type), 0, rgba(0, 0, 0, 0));
    }
    if (m_result && m_highlight >= 0 && m_highlight < int(m_result->plan.parts.size())) {
        auto& p = m_result->plan.parts[size_t(m_highlight)];
        CCPoint v[4] = {{X(p.t0), y0}, {X(p.t1), y0}, {X(p.t1), y0 + H}, {X(p.t0), y0 + H}};
        m_overview->drawPolygon(v, 4, rgba(1, 1, 1, 0.12f), 0.6f, rgba(1, 1, 1, 0.9f));
    }
    // waveform (min/max envelope)
    const auto& mn = m_A->waveformMin;
    const auto& mx = m_A->waveformMax;
    if (!mn.empty() && mn.size() == mx.size()) {
        const int cols = std::max(1, int(W / 1.5f));
        const float mid = y0 + H * 0.5f;
        for (int c = 0; c < cols; ++c) {
            size_t a = size_t(double(c) / cols * double(mn.size())), b = std::max(a + 1, size_t(double(c + 1) / cols * double(mn.size())));
            float lo = 0, hi = 0;
            for (size_t i = a; i < std::min(b, mn.size()); ++i) { lo = std::min(lo, mn[i]); hi = std::max(hi, mx[i]); }
            float x = (float(c) + 0.5f) * W / float(cols);
            m_overview->drawSegment({x, mid + lo * H * 0.45f}, {x, mid + hi * H * 0.45f}, 0.45f, rgba(0.75f, 0.8f, 1.f, 0.55f));
        }
    }
    // beats (downbeats stronger)
    for (double b : m_A->downbeats) m_overview->drawSegment({X(b), y0}, {X(b), y0 + 4.f}, 0.4f, rgba(1, 1, 1, 0.6f));
    // clicks coloured by role (dropped ones grey)
    if (m_result) {
        std::vector<char> dropped(m_result->plan.clicks.size(), 0);
        for (auto& cr : m_result->clicks) if (cr.dropped && cr.click >= 0 && size_t(cr.click) < dropped.size()) dropped[size_t(cr.click)] = 1;
        for (size_t i = 0; i < m_result->plan.clicks.size(); ++i) {
            auto& c = m_result->plan.clicks[i];
            ccColor4F col = dropped[i] ? rgba(0.4f, 0.4f, 0.4f, 0.9f) : roleColor(c.role);
            float x = X(c.time);
            float top = y0 + H - 2.f, bot = y0 + H * (0.62f - 0.25f * c.strength);
            m_overview->drawSegment({x, bot}, {x, top}, 0.5f, col);
            if (c.hold) m_overview->drawSegment({x, top}, {X(c.release), top}, 0.5f, col);
        }
    }
    // the detail window
    CCPoint v[4] = {{X(m_t0), y0}, {X(m_t0 + m_span), y0}, {X(m_t0 + m_span), y0 + H}, {X(m_t0), y0 + H}};
    m_overlay->drawPolygon(v, 4, rgba(1, 1, 1, 0.06f), 0.8f, rgba(0.3f, 1.f, 0.5f, 0.9f));
}

void Timeline::rebuildDetailImage() {
    m_detailHolder->removeAllChildren();
    if (!m_result) {
        m_info->setVisible(true);
        m_info->setString(m_A ? "Song analysed: generate a layout to see it here." : "Generate a layout to see the preview.");
        return;
    }
    m_info->setVisible(false);
    const float W = this->getContentWidth();
    report::RenderOptions ro;
    ro.x0 = xAtTime(m_t0) - 60.f;
    ro.x1 = xAtTime(m_t0 + m_span) + 60.f;
    ro.y0 = -30.f;
    ro.y1 = 420.f;
    const float targetPx = 900.f; // render at a moderate resolution, then scale to the node
    ro.scale = targetPx / std::max(60.f, ro.x1 - ro.x0);
    ro.maxWidth = int(targetPx) + 4;
    ro.drawDeco = false;
    std::string png = report::renderPng(m_result->level, &m_result->inputs, ro);
    if (png.empty()) return;
    auto img = new CCImage();
    if (!img->initWithImageData(png.data(), int(png.size()), CCImage::kFmtPng)) {
        img->release();
        return;
    }
    auto tex = new CCTexture2D();
    bool ok = tex->initWithImage(img);
    img->release();
    if (!ok) {
        tex->release();
        return;
    }
    auto spr = CCSprite::createWithTexture(tex);
    tex->release();
    if (!spr) return;
    float sx = W / spr->getContentWidth(), sy = (m_detailH - 2.f) / spr->getContentHeight();
    spr->setScaleX(sx);
    spr->setScaleY(std::min(sy, sx * 1.6f));
    spr->setAnchorPoint({0.f, 0.f});
    spr->setPosition({0.f, 1.f});
    m_detailHolder->addChild(spr);
}

} // namespace al::mod::ui
