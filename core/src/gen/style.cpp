#include <al/gen/style.hpp>

#include "pattern_params.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace al::gen {

using namespace pp;

namespace {

const char* kObName[ObCount] = {"none", "spike1", "spike2", "spike3", "half1", "half2", "mix2", "block1", "slab1", "saw"};
const char* kTrName[TrCount] = {"flat", "up1", "up2", "down1", "down2", "gap2", "gap3", "gap4"};
const float kObCx[ObCount] = {0, 0, 1.5f, 3.2f, 0, 1.0f, 2.0f, 0.5f, 0.4f, 1.5f};
const float kTrCx[TrCount] = {0, 0.5f, 2.0f, 0.4f, 1.0f, 1.0f, 2.0f, 3.0f};
const char* kFlName[FlCount] = {"open", "tight", "spiked", "saw", "stairs"};
const char* kSuName[SuCount] = {"spikes", "blocks", "gaps"};

void addGround(std::vector<PatternDef>& L, Mode m, const char* prefix) {
    for (int ob = 0; ob < ObCount; ++ob) {
        for (int tr = 0; tr < TrCount; ++tr) {
            if (ob == ObNone && (tr == TrFlat || tr == TrDown1 || tr == TrDown2)) continue; // nothing forces the click
            if (ob == ObSpike3 && (tr == TrUp2 || tr == TrGap4 || tr == TrGap3)) continue;       // not humanly fair
            if (ob != ObNone && tr >= TrGap2) continue;                                        // a pit is the obstacle (the builder places none on top)
            if (ob == ObMix2 && tr == TrUp2) continue;
            PatternDef d;
            d.id = std::string(prefix) + ":" + kObName[ob] + "+" + kTrName[tr];
            d.mode = m;
            d.a = ob;
            d.b = tr;
            d.c = SpNone;
            d.complexity = kObCx[ob] + kTrCx[tr];
            uint32_t t = 0;
            if (ob >= ObSpike1 && ob <= ObMix2) t |= TagSpike;
            if (ob == ObBlock1 || ob == ObSlab1) t |= TagBlock;
            if (ob == ObSaw) t |= TagSaw | TagModern;
            if (tr == TrUp1 || tr == TrUp2 || tr == TrDown1 || tr == TrDown2) t |= TagStep | TagFlow;
            if (tr >= TrGap2) t |= TagGap;
            if (ob == ObHalf1 || ob == ObHalf2 || ob == ObSlab1) t |= TagModern;
            if (ob == ObSpike1 || ob == ObSpike2 || ob == ObSpike3 || ob == ObBlock1) t |= TagClassic;
            if (d.complexity <= 1.0f) t |= TagCalm;
            if (d.complexity >= 2.0f) t |= TagIntense;
            if (ob == ObSpike3 || tr == TrGap4 || ob == ObMix2) t |= TagTight;
            if (ob != ObNone && tr != TrFlat) t |= TagMulti;
            d.tags = t;
            d.minDiff = std::max(0.0f, d.complexity * 0.9f - 0.6f);
            d.maxDiff = 9;
            d.baseWeight = (ob == ObSpike1 || ob == ObSpike2) ? 1.2f : 1.0f;
            L.push_back(d);
        }
    }
    // orbs over hazards and pads (special patterns)
    for (int tr : {int(TrFlat), int(TrGap2), int(TrGap3), int(TrGap4), int(TrUp1)}) {
        for (int sp : {int(SpOrbYellow), int(SpOrbPink)}) {
            PatternDef d;
            d.id = std::string(prefix) + ":" + (sp == SpOrbYellow ? "orb-yellow" : "orb-pink") + "+" + kTrName[tr];
            d.mode = m;
            d.a = ObNone;
            d.b = tr;
            d.c = sp;
            d.complexity = 1.0f + kTrCx[tr] * 0.5f + (sp == SpOrbPink ? 0.5f : 0.0f);
            d.tags = TagOrb | TagModern | (tr >= TrGap2 ? TagGap : 0u) | TagMulti;
            d.minDiff = sp == SpOrbPink ? 2.0f : 1.0f;
            d.maxDiff = 9;
            d.baseWeight = 0.8f;
            L.push_back(d);
        }
    }
    for (int tr : {int(TrFlat), int(TrUp1), int(TrUp2), int(TrGap3)}) {
        PatternDef d;
        d.id = std::string(prefix) + ":pad+" + kTrName[tr];
        d.mode = m;
        d.a = ObNone;
        d.b = tr;
        d.c = SpPadYellow;
        d.complexity = 0.8f + kTrCx[tr] * 0.4f;
        d.tags = TagPad | TagFlow | TagModern;
        d.minDiff = 0.5f;
        d.maxDiff = 9;
        d.baseWeight = 0.35f;
        L.push_back(d);
    }
}

void addFlying(std::vector<PatternDef>& L, Mode m, const char* prefix) {
    for (int f = 0; f < FlCount; ++f) {
        for (int dens = 0; dens < 2; ++dens) {
            if (m == Mode::Wave && f == FlStairs) continue;
            PatternDef d;
            d.id = std::string(prefix) + ":" + kFlName[f] + (dens ? "-dense" : "-sparse");
            d.mode = m;
            d.a = f;
            d.b = dens;
            d.complexity = float(f == FlOpen ? 0 : f == FlTight ? 2.0 : f == FlSpiked ? 1.5 : f == FlSaw ? 2.0 : 1.0) + dens * 0.8f;
            uint32_t t = TagCorridor;
            if (f == FlTight) t |= TagTight | TagIntense;
            if (f == FlSpiked) t |= TagSpike | TagClassic;
            if (f == FlSaw) t |= TagSaw | TagModern | TagIntense;
            if (f == FlStairs) t |= TagBlock | TagFlow | TagClassic;
            if (f == FlOpen) t |= TagCalm | TagFlow;
            if (dens) t |= TagDense;
            d.tags = t;
            d.minDiff = std::max(0.0f, d.complexity - 0.8f);
            d.maxDiff = 9;
            d.baseWeight = 1.0f;
            L.push_back(d);
        }
    }
}

void addSurface(std::vector<PatternDef>& L, Mode m, const char* prefix) {
    for (int f = 0; f < SuCount; ++f) {
        for (int dens = 0; dens < 2; ++dens) {
            PatternDef d;
            d.id = std::string(prefix) + ":" + kSuName[f] + (dens ? "-dense" : "-sparse");
            d.mode = m;
            d.a = f;
            d.b = dens;
            d.complexity = float(f) * 0.7f + dens * 0.8f + 0.5f;
            d.tags = (f == SuSpikes ? TagSpike | TagClassic : f == SuBlocks ? TagBlock | TagFlow : TagGap | TagIntense) | (dens ? TagDense : TagCalm);
            d.minDiff = std::max(0.0f, d.complexity - 1.0f);
            d.maxDiff = 9;
            d.baseWeight = 1.0f;
            L.push_back(d);
        }
    }
}

std::vector<PatternDef> buildLibrary() {
    std::vector<PatternDef> L;
    addGround(L, Mode::Cube, "cube");
    addGround(L, Mode::Robot, "robot");
    addFlying(L, Mode::Ship, "ship");
    addFlying(L, Mode::Ufo, "ufo");
    addFlying(L, Mode::Wave, "wave");
    addFlying(L, Mode::Swing, "swing");
    addSurface(L, Mode::Ball, "ball");
    addSurface(L, Mode::Spider, "spider");
    return L;
}

} // namespace

const std::vector<PatternDef>& patternLibrary() {
    static const std::vector<PatternDef> lib = buildLibrary();
    return lib;
}

const PatternDef* findPattern(const std::string& id) {
    static const std::unordered_map<std::string, size_t> index = [] {
        std::unordered_map<std::string, size_t> m;
        const auto& L = patternLibrary();
        for (size_t i = 0; i < L.size(); ++i) m[L[i].id] = i;
        return m;
    }();
    auto it = index.find(id);
    return it == index.end() ? nullptr : &patternLibrary()[it->second];
}

int patternToken(const PatternDef* d) {
    if (!d) return -1;
    const auto& L = patternLibrary();
    if (d < L.data() || d >= L.data() + L.size()) return patternToken(d->id);
    return int(d - L.data());
}

int patternToken(const std::string& id) { return patternToken(findPattern(id)); }

int vocabSize() { return int(patternLibrary().size()); }

std::vector<const PatternDef*> patternsFor(Mode m, float difficulty) {
    std::vector<const PatternDef*> out;
    for (auto& d : patternLibrary())
        if (d.mode == m && difficulty + 0.3f >= d.minDiff && difficulty <= d.maxDiff) out.push_back(&d);
    return out;
}

void contextFeatures(const PatternContext& c, float f[kContextFeatures]) {
    for (int i = 0; i < kContextFeatures; ++i) f[i] = 0;
    f[int(c.mode)] = 1;                                  // 0..7
    f[8] = c.intensity;
    f[9] = c.difficulty / 9.0f;
    f[10 + int(c.intent)] = 1;                           // 10..15
    f[16] = c.strength;
    f[17] = float(std::min(2.0, c.gapPrev) / 2.0);
    f[18] = float(std::min(2.0, c.gapNext) / 2.0);
    f[19] = float(c.speed) / 4.0f;
    f[20] = c.mini ? 1.0f : 0.0f;
    f[21] = std::clamp(c.pitchDelta / 12.0f, -1.0f, 1.0f);
    f[22] = c.hold ? 1.0f : 0.0f;
    f[23] = (c.role == Role::Downbeat || c.role == Role::Kick) ? 1.0f : 0.0f;
}

void RuleStyleModel::score(const PatternContext& ctx, const std::vector<const PatternDef*>& cands, std::vector<float>& w) const {
    w.assign(cands.size(), 0.0f);
    const float target = 0.15f + ctx.difficulty * 0.38f * (0.65f + 0.7f * ctx.intensity); // desired complexity
    for (size_t i = 0; i < cands.size(); ++i) {
        const PatternDef& d = *cands[i];
        float x = d.baseWeight;
        uint32_t t = d.tags;
        if (t & TagCalm) x *= 1.25f - ctx.intensity * 0.6f;
        if (t & TagIntense) x *= 0.45f + ctx.intensity;
        switch (ctx.style) {
            case Style::Flow:
                if (t & (TagStep | TagFlow)) x *= 1.7f;
                if (t & TagSpike) x *= 0.8f;
                if (t & TagTight) x *= 0.5f;
                break;
            case Style::Tight:
                if (t & TagTight) x *= 1.9f;
                if (t & TagSpike) x *= 1.3f;
                if (t & TagCalm) x *= 0.6f;
                break;
            case Style::Minimal:
                if (t & TagMulti) x *= 0.35f;
                if (t & TagDense) x *= 0.4f;
                if (d.complexity <= 1.0f) x *= 1.5f;
                break;
            case Style::Retro:
                if (t & TagClassic) x *= 1.6f;
                if (t & TagModern) x *= 0.3f;
                break;
            case Style::Modern:
                if (t & TagModern) x *= 1.4f;
                if (t & TagOrb) x *= 1.2f;
                break;
            case Style::Chaotic:
                x = std::sqrt(std::max(0.01f, x)) * (t & TagMulti ? 1.4f : 1.0f);
                break;
            case Style::Memory:
                if (t & TagGap) x *= 1.4f;
                if (t & TagOrb) x *= 1.3f;
                break;
            default: break;
        }
        if (ctx.intent == Intent::Build && (t & TagStep)) x *= 1.3f;
        if (ctx.intent == Intent::Breather && (t & TagIntense)) x *= 0.4f;
        if (ctx.intent == Intent::Climax && (t & TagCalm)) x *= 0.6f;
        // difficulty fit
        float dc = d.complexity - target;
        x *= std::exp(-dc * dc / 3.0f);
        // musical emphasis: strong hits deserve bigger obstacles
        x *= 0.6f + 0.6f * ctx.strength * (d.complexity >= 1.0f ? 1.0f : 0.5f);
        if (ctx.hold && (d.b == TrUp1 || d.b == TrUp2 || d.a == ObSpike3)) x *= 1.5f;
        // variety: avoid identical patterns in a row
        if (d.id == ctx.prevPattern) x *= ctx.style == Style::Minimal ? 0.6f : 0.3f;
        if (d.id == ctx.prevPattern && d.id == ctx.prevPattern2) x *= 0.15f;
        w[i] = std::max(0.0f, x);
    }
}

uint32_t motifKey(double gapPrev, double gapNext, Role role, float pitchDelta, bool downbeat) {
    auto q = [](double g) { return uint32_t(std::clamp(int(std::lround(g * 8.0)), 0, 31)); };
    uint32_t pd = pitchDelta > 0.5f ? 2u : (pitchDelta < -0.5f ? 0u : 1u);
    return (q(gapPrev) << 0) | (q(gapNext) << 5) | (uint32_t(role) << 10) | (pd << 14) | (uint32_t(downbeat) << 16);
}

} // namespace al::gen
