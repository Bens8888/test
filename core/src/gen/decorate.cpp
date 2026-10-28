// Decoration pass: block theme, interior fill, background deco, colours per section and
// music-synced triggers. It never changes gameplay hitboxes: only solid pieces with identical
// 30x30 / 15x15 hitboxes are swapped, interior cells that cannot be touched become decoration,
// and everything else added is decoration or triggers (the generator re-verifies afterwards).
#include <al/gen/generator.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace al::gen {

using level::LevelObject;
namespace oid = level::id;

namespace {

struct Palette {
    level::Color bg, g1, line, block, detail, accent;
};

// Base palettes (night neon, sunset, toxic, ice, monochrome, classic blue, royal)
const Palette kPalettes[] = {
    {{18, 10, 40}, {40, 15, 90}, {255, 255, 255}, {60, 30, 140}, {140, 90, 255}, {0, 230, 255}},
    {{255, 110, 60}, {120, 30, 60}, {255, 230, 200}, {90, 20, 45}, {255, 150, 90}, {255, 200, 0}},
    {{10, 30, 10}, {20, 70, 20}, {180, 255, 120}, {20, 60, 25}, {120, 255, 80}, {120, 255, 0}},
    {{140, 200, 255}, {40, 90, 160}, {255, 255, 255}, {40, 80, 150}, {200, 240, 255}, {120, 220, 255}},
    {{15, 15, 15}, {35, 35, 35}, {255, 255, 255}, {30, 30, 30}, {255, 60, 60}, {255, 60, 60}},
    {{40, 125, 255}, {0, 102, 255}, {255, 255, 255}, {0, 60, 160}, {120, 200, 255}, {0, 200, 255}},
    {{30, 20, 70}, {70, 30, 120}, {255, 220, 120}, {50, 25, 95}, {255, 200, 60}, {255, 180, 40}},
};

level::Color shade(level::Color c, float k) {
    auto f = [&](uint8_t v) { return uint8_t(std::clamp(int(std::lround(v * k)), 0, 255)); };
    return {f(c.r), f(c.g), f(c.b)};
}

level::Color mix(level::Color a, level::Color b, float t) {
    auto f = [&](uint8_t x, uint8_t y) { return uint8_t(std::clamp(int(std::lround(x + (y - x) * t)), 0, 255)); };
    return {f(a.r, b.r), f(a.g, b.g), f(a.b, b.b)};
}

// Mood adjustment by intent: calmer parts darker/desaturated, climaxes brighter.
Palette moodFor(const Palette& p, Intent i, float intensity) {
    Palette q = p;
    float k = 1.0f;
    switch (i) {
        case Intent::Intro: case Intent::Outro: k = 0.7f; break;
        case Intent::Breather: k = 0.6f; break;
        case Intent::Build: k = 0.85f + 0.2f * intensity; break;
        case Intent::Climax: k = 1.05f + 0.15f * intensity; break;
        default: k = 0.9f; break;
    }
    q.bg = shade(p.bg, k);
    q.g1 = shade(p.g1, k);
    q.block = shade(p.block, std::min(1.2f, k));
    q.detail = shade(p.detail, std::min(1.2f, k));
    if (i == Intent::Climax) q.bg = mix(q.bg, p.accent, 0.15f);
    return q;
}

LevelObject trig(int id, double x, double y) {
    LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.setProp(36, std::string("1"));
    return o;
}

void colorTrigger(level::Level& L, double x, int channel, level::Color c, double dur, bool blending = false) {
    LevelObject o = trig(oid::TrigColor, x, 750 + 30 * (channel % 7));
    o.setProp(23, double(channel));
    o.setProp(7, double(c.r));
    o.setProp(8, double(c.g));
    o.setProp(9, double(c.b));
    o.setProp(10, dur);
    o.setProp(35, 1.0);
    if (blending) o.setProp(17, std::string("1"));
    o.editorLayer = 0;
    L.objects.push_back(o);
}

} // namespace

void decorate(level::Level& L, const bot::InputTrack& inputs, const DesignPlan& plan, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng, int groupBase) {
    // ---- player x at song time (gameplay-only level; x is independent of inputs except snapping)
    std::vector<float> xAt;
    {
        phys::World W(L);
        W.setEndX(1e9f);
        phys::GameState s = phys::initialState(W);
        int ticks = int(std::ceil(plan.duration * phys::kTPS)) + 2;
        xAt.reserve(size_t(ticks));
        // follow the intended inputs (speed portals decide x); after the end, extrapolate
        float lastX = s.p[0].x;
        float vx = float(phys::speedParams(s.p[0].speed).unitsPerStep());
        size_t ei = 0;
        bool held = false;
        for (int t = 0; t < ticks; ++t) {
            while (ei < inputs.events.size() && inputs.events[ei].tick <= s.tick + 1) { held = inputs.events[ei].press; ++ei; }
            if (!s.dead) {
                phys::step(W, s, held);
                if (!s.dead) { lastX = s.p[0].x; vx = float(phys::speedParams(s.p[0].speed).unitsPerStep()); }
            } else {
                lastX += vx;
            }
            xAt.push_back(lastX);
        }
    }
    auto xOf = [&](double t) {
        if (xAt.empty()) return float(t * 311.58);
        int k = std::clamp(int(std::lround(t * phys::kTPS)), 0, int(xAt.size()) - 1);
        return xAt[size_t(k)];
    };

    // ---- theme
    BlockTheme theme = opt.theme;
    if (theme == BlockTheme::Auto) {
        static const BlockTheme pick[] = {BlockTheme::Colored, BlockTheme::Colored, BlockTheme::Outline, BlockTheme::Classic, BlockTheme::Grid};
        theme = plan.style == Style::Retro ? BlockTheme::Classic : pick[rng.below(5)];
    }
    int fullId = oid::Block, smallId = 195, innerId = -1;
    switch (theme) {
        case BlockTheme::Colored: fullId = oid::ColBlock; smallId = oid::ColSmallSquare; innerId = oid::ColInner; break;
        case BlockTheme::Outline: fullId = oid::OutlineSquare; smallId = 661; break;
        case BlockTheme::Grid: fullId = 83; smallId = 195; break;
        case BlockTheme::Bricks: fullId = 69; smallId = 195; break;
        default: break;
    }
    const int blockChannel = 1, detailChannel = 2;
    // occupancy grid of full blocks (30-grid cells)
    std::set<std::pair<long, long>> cells;
    for (auto& o : L.objects) {
        if (o.id != oid::Block) continue;
        cells.insert({long(std::floor(o.x / 30.0)), long(std::floor(o.y / 30.0))});
    }
    auto filled = [&](long cx, long cy) { return cells.count({cx, cy}) > 0; };
    const int decoLevel = opt.decoLevel >= 0 ? opt.decoLevel : (plan.style == Style::Minimal ? 0 : (plan.style == Style::DecorHeavy ? 3 : 2));
    for (auto& o : L.objects) {
        if (o.id == oid::Block) {
            long cx = long(std::floor(o.x / 30.0)), cy = long(std::floor(o.y / 30.0));
            bool interior = true;
            for (int dx = -1; dx <= 1 && interior; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if (!filled(cx + dx, cy + dy)) { interior = false; break; }
            if (interior && innerId > 0) {
                o.id = innerId; // decoration: unreachable because all 8 neighbours are solid
            } else {
                o.id = fullId;
            }
            if (theme == BlockTheme::Colored) { o.colorMain = blockChannel; o.colorDetail = detailChannel; }
        } else if (o.id == 195) {
            o.id = smallId;
            if (theme == BlockTheme::Colored) { o.colorMain = blockChannel; o.colorDetail = detailChannel; }
        }
    }

    // ---- colours
    const Palette& base = kPalettes[rng.below(uint32_t(std::size(kPalettes)))];
    auto& st = L.settings;
    Palette first = plan.parts.empty() ? base : moodFor(base, plan.parts.front().intent, plan.parts.front().intensity);
    st.colors.clear();
    st.colors.push_back(level::makeChannel(1000, first.bg));
    st.colors.push_back(level::makeChannel(1001, first.g1));
    st.colors.push_back(level::makeChannel(1009, shade(first.g1, 0.8f)));
    st.colors.push_back(level::makeChannel(1002, first.line, true));
    st.colors.push_back(level::makeChannel(1004, {255, 255, 255}));
    st.colors.push_back(level::makeChannel(blockChannel, first.block));
    st.colors.push_back(level::makeChannel(detailChannel, first.detail));
    st.colors.push_back(level::makeChannel(3, base.accent, true));
    st.background = 1 + int(rng.below(20));
    st.ground = 1 + int(rng.below(10));
    st.groundLine = 1 + int(rng.below(2));
    if (opt.triggerColors) {
        for (size_t i = 1; i < plan.parts.size(); ++i) {
            const auto& p = plan.parts[i];
            Palette m = moodFor(base, p.intent, p.intensity);
            double x = xOf(p.t0) - 30;
            double dur = p.intent == Intent::Climax ? 0.15 : 0.8;
            colorTrigger(L, x, 1000, m.bg, dur);
            colorTrigger(L, x, 1001, m.g1, dur);
            colorTrigger(L, x, blockChannel, m.block, dur);
            colorTrigger(L, x, detailChannel, m.detail, dur);
        }
    }
    // ---- beat pulses on downbeats in intense parts
    if (opt.triggerPulse) {
        for (const auto& p : plan.parts) {
            if (p.intent != Intent::Climax && !(p.intent == Intent::Build && p.intensity > 0.5f)) continue;
            Palette m = moodFor(base, p.intent, p.intensity);
            for (double t : A.downbeats) {
                if (t < p.t0 + 0.05 || t >= p.t1) continue;
                LevelObject o = trig(oid::TrigPulse, xOf(t) - 5, 990);
                o.setProp(51, 1000.0);   // BG channel
                o.setProp(52, 0.0);      // target type: channel
                o.setProp(48, 0.0);      // colour mode
                level::Color c = mix(m.bg, m.detail, 0.55f);
                o.setProp(7, double(c.r));
                o.setProp(8, double(c.g));
                o.setProp(9, double(c.b));
                o.setProp(45, 0.0);
                o.setProp(46, 0.03);
                o.setProp(47, std::min(0.4, A.beatPeriodAt(t) * 0.8));
                L.objects.push_back(o);
            }
        }
    }
    // ---- camera: zoom out a little in fast/intense parts, back in calm parts; shake on drops
    if (opt.triggerCamera || opt.triggerShake) {
        float zoom = 1.0f;
        for (const auto& p : plan.parts) {
            float want = p.intent == Intent::Climax ? (p.speed >= 3 ? 0.85f : 0.92f) : (p.intent == Intent::Breather ? 1.05f : 1.0f);
            if (opt.triggerCamera && std::fabs(want - zoom) > 0.01f) {
                LevelObject o = trig(oid::TrigCamZoom, xOf(p.t0) - 60, 1020);
                o.setProp(371, double(want));
                o.setProp(10, p.intent == Intent::Climax ? 0.4 : 1.2);
                o.setProp(30, 2.0);
                o.setProp(85, 2.0);
                L.objects.push_back(o);
                zoom = want;
            }
            if (opt.triggerShake && p.intent == Intent::Climax && p.intensity > 0.6f && p.index > 0 && plan.parts[size_t(p.index - 1)].intent == Intent::Build) {
                LevelObject o = trig(oid::TrigShake, xOf(p.t0), 1050);
                o.setProp(75, 1.5);
                o.setProp(84, 0.03);
                o.setProp(10, 0.35);
                L.objects.push_back(o);
            }
        }
    }
    // ---- background deco that moves with the beat (well away from gameplay, on its own groups)
    if (decoLevel > 0 && opt.triggerPulse) {
        static const int decoIds[] = {50, 51, 54, 60, 148, 149, 495, 497};
        int group = groupBase;
        for (const auto& p : plan.parts) {
            if (p.intent == Intent::Breather && decoLevel < 2) continue;
            int g = group++;
            double x0 = xOf(p.t0), x1 = xOf(p.t1);
            float spacing = decoLevel >= 3 ? 150.0f : (decoLevel == 2 ? 240.0f : 420.0f);
            int decoId = decoIds[rng.below(uint32_t(std::size(decoIds)))];
            for (double x = x0 + 60; x < x1 - 60; x += spacing) {
                LevelObject o;
                o.id = decoId;
                o.x = std::round(x / 15.0) * 15.0;
                o.y = 420 + 60 * rng.below(4);
                o.scale = 0.6 + 0.2 * rng.below(3);
                o.zLayer = -3; // B4: far background
                o.colorMain = 3;
                o.groups = {g};
                L.objects.push_back(o);
            }
            // bob the group on every beat of the part
            bool up = true;
            for (double t : A.beats) {
                if (t < p.t0 || t >= p.t1) continue;
                LevelObject m = trig(oid::TrigMove, xOf(t) - 5, 1080);
                m.setProp(51, double(g));
                m.setProp(28, 0.0);
                m.setProp(29, up ? 9.0 : -9.0);
                m.setProp(10, A.beatPeriodAt(t) * 0.5);
                m.setProp(30, 2.0);
                m.setProp(85, 2.0);
                L.objects.push_back(m);
                up = !up;
            }
            if (!up) {
                // return to rest at the end of the part
                LevelObject m = trig(oid::TrigMove, xOf(p.t1) - 10, 1080);
                m.setProp(51, double(g));
                m.setProp(29, -9.0);
                m.setProp(10, 0.1);
                L.objects.push_back(m);
            }
        }
    }
}

} // namespace al::gen
