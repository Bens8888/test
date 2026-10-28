#include <al/gen/plan.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace al::gen {

using audio::EventType;
using audio::SectionType;

const char* roleName(Role r) {
    switch (r) {
        case Role::Downbeat: return "downbeat";
        case Role::Kick: return "kick";
        case Role::Snare: return "snare";
        case Role::HiHat: return "hi-hat";
        case Role::Melody: return "melody";
        case Role::Sustain: return "sustain";
        case Role::Syncopation: return "syncopation";
        case Role::Beat: return "beat";
        case Role::Fill: return "fill";
        case Role::Recorded: return "recorded";
    }
    return "?";
}

const char* intentName(Intent i) {
    switch (i) {
        case Intent::Intro: return "intro";
        case Intent::Groove: return "groove";
        case Intent::Build: return "build-up";
        case Intent::Climax: return "climax";
        case Intent::Breather: return "breather";
        case Intent::Outro: return "outro";
    }
    return "?";
}

double cubeAirtime(int speed, bool mini) {
    // from the simulator (fresh-press jumps, see tests/test_physics.cpp): ticks of airtime
    static const double normal[5] = {100, 103, 106, 103, 103};
    static const double small[5] = {80, 82, 84, 83, 83};
    int s = std::clamp(speed, 0, 4);
    return (mini ? small[s] : normal[s]) / 240.0;
}

float resolveDifficulty(const audio::MusicAnalysis& A, const GenOptions& opt) {
    if (opt.difficulty != Difficulty::Auto) return float(int(opt.difficulty));
    double energy = 0, dur = 0;
    for (auto& s : A.sections) { energy += s.energy * s.duration(); dur += s.duration(); }
    energy = dur > 0 ? energy / dur : 0.5;
    double bpm = A.mainBpm();
    double d = 3.4 + 2.0 * (energy - 0.5) + std::clamp((bpm - 130.0) / 40.0, -0.8, 1.0);
    return float(std::clamp(d, 1.5, 6.0));
}

Style resolveStyle(const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng) {
    if (opt.style != Style::Auto) return opt.style;
    double bpm = A.mainBpm();
    double dens = 0;
    int n = 0;
    for (auto& s : A.sections) { dens += s.density; ++n; }
    dens = n ? dens / n : 4;
    if (bpm >= 160 && dens > 6) return rng.chance(0.6) ? Style::Tight : Style::Chaotic;
    if (bpm < 110 || dens < 3) return rng.chance(0.7) ? Style::Flow : Style::Minimal;
    return rng.chance(0.7) ? Style::Modern : Style::Flow;
}

Intent intentFor(SectionType t) {
    switch (t) {
        case SectionType::Intro: return Intent::Intro;
        case SectionType::Verse: return Intent::Groove;
        case SectionType::Chorus: return Intent::Climax;
        case SectionType::BuildUp: return Intent::Build;
        case SectionType::Drop: return Intent::Climax;
        case SectionType::Break: return Intent::Breather;
        case SectionType::Outro: return Intent::Outro;
    }
    return Intent::Groove;
}

// Gamemode affinity per intent (rows) for cube, ship, ball, ufo, wave, robot, spider, swing.
static const float kAffinity[6][8] = {
    {1.00f, 0.35f, 0.25f, 0.15f, 0.05f, 0.25f, 0.10f, 0.10f}, // intro
    {0.85f, 0.50f, 0.60f, 0.50f, 0.35f, 0.60f, 0.45f, 0.35f}, // groove
    {0.60f, 0.75f, 0.35f, 0.45f, 0.65f, 0.30f, 0.30f, 0.55f}, // build
    {0.70f, 0.70f, 0.60f, 0.55f, 0.95f, 0.40f, 0.60f, 0.70f}, // climax
    {0.50f, 0.85f, 0.25f, 0.70f, 0.10f, 0.20f, 0.15f, 0.55f}, // breather
    {0.95f, 0.55f, 0.20f, 0.35f, 0.10f, 0.20f, 0.05f, 0.15f}, // outro
};

static int sectionOverrideDelta(const GenOptions& opt, int sectionType, int* forcedMode) {
    int delta = 0;
    for (auto& o : opt.sectionOverrides) {
        if (o.sectionType != sectionType) continue;
        delta += o.difficultyDelta;
        if (o.forcedMode >= 0 && forcedMode) *forcedMode = o.forcedMode;
    }
    return delta;
}

std::vector<Part> planParts(const audio::MusicAnalysis& A, const GenOptions& opt, float Dbase, Style style, Rng& rng, int* baseSpeedOut, const ModeFit* fit) {
    std::vector<Part> parts;
    const double dur = A.duration;
    // ---- 1. sections -> raw parts (merge tiny sections, split long ones on bar lines)
    struct Raw { double t0, t1; int section; SectionType type; float energy, trend; };
    std::vector<Raw> raw;
    for (size_t i = 0; i < A.sections.size(); ++i) {
        auto& s = A.sections[i];
        raw.push_back({s.start, s.end, int(i), s.type, s.energy, s.trend});
    }
    if (raw.empty()) raw.push_back({0, dur, -1, SectionType::Verse, 0.5f, 0});
    raw.front().t0 = 0;
    raw.back().t1 = dur;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i].t1 - raw[i].t0 < 4.0 && raw.size() > 1) {
            if (i > 0) { raw[i - 1].t1 = raw[i].t1; raw.erase(raw.begin() + long(i)); }
            else { raw[i + 1].t0 = raw[i].t0; raw.erase(raw.begin()); }
            continue;
        }
        ++i;
    }
    const float pf = opt.portalFrequency < 0 ? 0.5f : std::clamp(opt.portalFrequency, 0.0f, 1.0f);
    const float lenScale = (style == Style::Minimal ? 1.6f : 1.0f) * (1.45f - 0.9f * pf);
    std::vector<Raw> split;
    for (auto& r : raw) {
        double maxLen = (r.type == SectionType::Drop || r.type == SectionType::Chorus ? 13.0 : (r.type == SectionType::Verse ? 16.0 : 22.0)) * lenScale;
        int pieces = std::max(1, int(std::ceil((r.t1 - r.t0) / maxLen - 0.15)));
        double step = (r.t1 - r.t0) / pieces;
        double prev = r.t0;
        for (int k = 1; k <= pieces; ++k) {
            double cut = k == pieces ? r.t1 : r.t0 + step * k;
            if (k < pieces && !A.downbeats.empty()) {
                // snap the cut to the nearest downbeat
                auto it = std::lower_bound(A.downbeats.begin(), A.downbeats.end(), cut);
                double best = cut, bd = 1e9;
                if (it != A.downbeats.end() && std::fabs(*it - cut) < bd) { bd = std::fabs(*it - cut); best = *it; }
                if (it != A.downbeats.begin() && std::fabs(*(it - 1) - cut) < bd) { bd = std::fabs(*(it - 1) - cut); best = *(it - 1); }
                if (best - prev > 3.0 && r.t1 - best > 3.0) cut = best;
            }
            split.push_back({prev, cut, r.section, r.type, r.energy, r.trend});
            prev = cut;
        }
    }
    // ---- 2. intensity normalisation
    float eMin = 1, eMax = 0;
    for (auto& r : split) { eMin = std::min(eMin, r.energy); eMax = std::max(eMax, r.energy); }
    auto norm = [&](float e) { return eMax - eMin > 0.05f ? (e - eMin) / (eMax - eMin) : 0.5f; };

    // ---- 3. base speed
    int baseSpeed = opt.startSpeed >= 0 ? opt.startSpeed : 1;
    const auto prof = profileAt(Dbase);
    if (opt.startSpeed < 0 && Dbase >= 3.5f && rng.chance(0.35 * prof.speedBias) && A.mainBpm() >= 125) baseSpeed = 2;
    if (baseSpeedOut) *baseSpeedOut = baseSpeed;
    SpeedChanges sc = opt.speedChanges;
    if (sc == SpeedChanges::Auto) sc = Dbase < 1.5f ? SpeedChanges::Off : (style == Style::Chaotic || Dbase >= 6.5f ? SpeedChanges::Aggressive : SpeedChanges::Subtle);

    // ---- 4. per part: intent, difficulty, mode, speed, size, gravity, dual
    Mode prevMode = Mode::Cube;
    int prevSection = -2;
    for (size_t i = 0; i < split.size(); ++i) {
        const auto& r = split[i];
        Part p;
        p.index = int(i);
        p.t0 = r.t0;
        p.t1 = r.t1;
        p.section = r.section;
        p.intent = intentFor(r.type);
        if (i == 0 && p.intent != Intent::Climax) p.intent = Intent::Intro;
        if (i + 1 == split.size() && split.size() > 2 && p.intent != Intent::Climax) p.intent = Intent::Outro;
        p.intensity = std::clamp(norm(r.energy), 0.0f, 1.0f);
        int forced = -1;
        int delta = sectionOverrideDelta(opt, int(r.type), &forced);
        p.difficulty = std::clamp(Dbase + (p.intensity - 0.5f) * 1.6f + float(delta), std::max(0.0f, Dbase - 2.0f), std::min(9.0f, Dbase + 1.2f + std::max(0, delta)));
        // mode weights
        float w[kModeCount];
        for (int m = 0; m < kModeCount; ++m) {
            float a = kAffinity[int(p.intent)][m];
            float user = opt.modeWeights[size_t(m)];
            if (user >= 0) a *= 2.0f * user;
            Mode mm = Mode(m);
            if ((mm == Mode::Wave) && p.difficulty < 1.5f) a *= 0.15f;
            if ((mm == Mode::Spider || mm == Mode::Swing) && p.difficulty < 2.0f) a *= 0.3f;
            if (style == Style::Retro && (mm == Mode::Spider || mm == Mode::Swing || mm == Mode::Robot)) a *= 0.05f;
            bool sameSection = r.section == prevSection;
            if (i > 0 && mm == prevMode) a *= sameSection ? (style == Style::Minimal ? 1.8f : 0.55f) : (style == Style::Minimal ? 1.2f : 0.3f);
            if (style == Style::Chaotic && i > 0 && mm != prevMode) a *= 1.3f;
            if (fit && *fit) a *= (*fit)(p, mm);
            w[m] = std::max(0.0f, a);
        }
        int mode;
        if (forced >= 0) mode = forced;
        else if (i == 0) mode = (p.intent == Intent::Intro && w[0] > 0) ? 0 : rng.weighted(w, size_t(kModeCount));
        else {
            // sharpen: squared weights (the brain prefers the best fits but keeps variety)
            float w2[kModeCount];
            for (int m = 0; m < kModeCount; ++m) w2[m] = w[m] * w[m];
            mode = rng.weighted(w2, size_t(kModeCount));
        }
        if (mode < 0) mode = 0;
        p.mode = Mode(mode);
        // speed
        int sp = baseSpeed;
        if (sc == SpeedChanges::Subtle) {
            if (p.intent == Intent::Climax && p.intensity > 0.6f) sp = baseSpeed + 1;
            else if (p.intent == Intent::Breather) sp = std::max(baseSpeed - 1, 1);
        } else if (sc == SpeedChanges::Aggressive) {
            if (p.intent == Intent::Climax) sp = baseSpeed + (p.intensity > 0.75f ? 2 : 1);
            else if (p.intent == Intent::Build) sp = baseSpeed + 1;
            else if (p.intent == Intent::Breather) sp = std::max(baseSpeed - 1, 0);
        }
        int cap = p.difficulty >= 6 ? 4 : (p.difficulty >= 3.5f ? 3 : 2);
        if ((p.mode == Mode::Wave || p.mode == Mode::Spider || p.mode == Mode::Swing) && p.difficulty < 6) cap = std::min(cap, 2);
        p.speed = std::clamp(sp, 0, cap);
        // size
        double miniChance = opt.sizeChanges == Tri::Off ? 0 : (opt.sizeChanges == Tri::On ? 0.35 : (p.difficulty >= 3 ? 0.12 : 0));
        if (p.intent == Intent::Intro || p.intent == Intent::Outro) miniChance = 0;
        p.mini = rng.chance(miniChance);
        // gravity
        double flipChance = opt.gravityFlips == Tri::Off ? 0 : (opt.gravityFlips == Tri::On ? 0.3 : (p.difficulty >= 3 ? 0.12 : 0));
        if (p.mode == Mode::Ball || p.mode == Mode::Spider || p.mode == Mode::Swing || p.intent == Intent::Intro) flipChance = 0;
        p.flipped = rng.chance(flipChance);
        // dual (flying climax parts)
        double dualChance = opt.dual == Tri::Off ? 0 : (opt.dual == Tri::On ? 0.3 : (p.difficulty >= 4 ? 0.1 : 0));
        if (!(p.mode == Mode::Ship || p.mode == Mode::Wave || p.mode == Mode::Ufo) || p.intent != Intent::Climax) dualChance = 0;
        p.dual = rng.chance(dualChance);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (%s, energy %.2f): %s at %s%s%s%s, difficulty %.1f", intentName(p.intent),
                      r.section >= 0 ? audio::sectionTypeName(r.type) : "song", p.intensity, level::modeName(p.mode),
                      p.speed == 0 ? "0.5x" : (p.speed == 1 ? "1x" : (p.speed == 2 ? "2x" : (p.speed == 3 ? "3x" : "4x"))),
                      p.mini ? ", mini" : "", p.flipped ? ", upside down" : "", p.dual ? ", dual" : "", p.difficulty);
        p.why = buf;
        prevMode = p.mode;
        prevSection = r.section;
        parts.push_back(p);
    }
    return parts;
}

std::vector<Candidate> gatherCandidates(const audio::MusicAnalysis& A, double t0, double t1, float holdBias) {
    std::vector<Candidate> c;
    for (auto& e : A.events) {
        if (e.time < t0 || e.time >= t1) continue;
        float base = 0;
        Role role = Role::Beat;
        switch (e.type) {
            case EventType::Downbeat: base = 1.0f; role = Role::Downbeat; break;
            case EventType::Kick: base = 0.9f; role = Role::Kick; break;
            case EventType::Snare: base = 0.85f; role = Role::Snare; break;
            case EventType::Note: base = 0.7f; role = Role::Melody; break;
            case EventType::Sustain: base = 0.65f + 0.25f * std::max(0.0f, holdBias); role = Role::Sustain; break;
            case EventType::Onset: base = 0.55f; role = Role::Syncopation; break;
            case EventType::Beat: base = 0.5f; role = Role::Beat; break;
            case EventType::HiHat: base = 0.25f; role = Role::HiHat; break;
            default: continue;
        }
        float sal = base * (0.5f + 0.5f * std::min(1.0f, e.strength));
        if (e.type == EventType::Beat || e.type == EventType::Downbeat) sal = base * (0.6f + 0.4f * std::min(1.0f, e.strength));
        // grid bonus: on-beat or half-beat positions are stronger
        double grid4 = A.snapToGrid(e.time, 4);
        double grid2 = A.snapToGrid(e.time, 2);
        double grid1 = A.snapToGrid(e.time, 1);
        if (std::fabs(grid1 - e.time) < 0.025) sal *= 1.15f;
        else if (std::fabs(grid2 - e.time) < 0.025) sal *= 1.05f;
        else if (std::fabs(grid4 - e.time) > 0.03 && role != Role::Melody) { sal *= 0.85f; if (role != Role::Sustain) role = Role::Syncopation; }
        float pitch = e.type == EventType::Note || e.type == EventType::Sustain ? e.pitch : A.pitchAt(e.time);
        c.push_back({e.time, sal, role, pitch, e.type == EventType::Sustain ? double(e.duration) : 0.0});
    }
    // merge candidates within 35 ms: keep the most salient, combine sustain info
    std::sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) { return a.t < b.t; });
    std::vector<Candidate> m;
    for (auto& x : c) {
        if (!m.empty() && x.t - m.back().t < 0.035) {
            auto& y = m.back();
            double sus = std::max(y.sustain, x.sustain);
            if (x.sal > y.sal) { double tt = y.t; y = x; y.t = std::min(tt, x.t); }
            y.sal = std::min(1.5f, std::max(y.sal, x.sal) + 0.1f);
            y.sustain = sus;
            continue;
        }
        m.push_back(x);
    }
    return m;
}

double minGapFor(Mode mode, int speed, bool mini, const DifficultyProfile& prof) {
    // shortest musical gap between two clicks the gamemode can express fairly
    switch (mode) {
        case Mode::Cube: return prof.orbs ? std::max<double>(prof.minClickGap, 0.16) : cubeAirtime(speed, mini) + 0.02;
        case Mode::Robot: return prof.orbs ? std::max<double>(prof.minClickGap, 0.2) : 0.32;
        case Mode::Ball: return std::max<double>(prof.minClickGap, 0.22);
        case Mode::Spider: return std::max<double>(prof.minClickGap, 0.12);
        case Mode::Ufo: return std::max<double>(prof.minClickGap, 0.13);
        case Mode::Ship: return std::max<double>(prof.minClickGap * 1.4, 0.2);
        case Mode::Wave: return std::max<double>(prof.minClickGap * 0.8, 0.09);
        case Mode::Swing: return std::max<double>(prof.minClickGap, 0.16);
        default: return 0.3;
    }
}

std::vector<Click> selectClicks(const audio::MusicAnalysis& A, const Part& part, const GenOptions& opt, Style style, Rng& rng) {
    const auto prof = profileAt(part.difficulty);
    const double leadIn = part.index == 0 ? 1.0 : 0.45;  // room after a portal / the level start
    const double leadOut = 0.3;
    double t0 = part.t0 + leadIn, t1 = part.t1 - leadOut;
    std::vector<Click> out;
    if (t1 - t0 < 0.3) return out;
    auto cands = gatherCandidates(A, t0, t1, opt.holdBias);
    float rate = prof.clicksCalm + (prof.clicksIntense - prof.clicksCalm) * std::pow(part.intensity, 1.2f);
    if (part.intent == Intent::Intro || part.intent == Intent::Outro) rate *= 0.7f;
    if (part.intent == Intent::Breather) rate *= 0.65f;
    if (part.intent == Intent::Build) rate *= 1.05f;
    float densityMul = opt.density < 0 ? 1.0f : 0.45f + 1.1f * opt.density;
    static const float modeMul[8] = {1.0f, 0.75f, 0.9f, 1.0f, 1.25f, 0.85f, 0.9f, 0.9f};
    float styleMul = style == Style::Minimal ? 0.7f : style == Style::Tight ? 1.15f : style == Style::Chaotic ? 1.25f : style == Style::Flow ? 0.95f : style == Style::Retro ? 0.85f : 1.0f;
    rate *= densityMul * modeMul[int(part.mode)] * styleMul;
    int target = std::max(1, int(std::lround(rate * (t1 - t0))));
    double gap = minGapFor(part.mode, part.speed, part.mini, prof);
    // greedy: most salient first (small random jitter so variants differ), respecting the min gap
    std::vector<size_t> order(cands.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::vector<float> key(cands.size());
    for (size_t i = 0; i < cands.size(); ++i) key[i] = cands[i].sal * float(rng.uniform(0.85, 1.15));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key[a] > key[b]; });
    std::vector<size_t> chosen;
    auto fits = [&](double t) {
        for (size_t c : chosen) if (std::fabs(cands[c].t - t) < gap) return false;
        return true;
    };
    for (size_t i : order) {
        if (int(chosen.size()) >= target) break;
        if (cands[i].sal < 0.3f && int(chosen.size()) >= target / 2) continue;
        if (fits(cands[i].t)) chosen.push_back(i);
    }
    std::sort(chosen.begin(), chosen.end(), [&](size_t a, size_t b) { return cands[a].t < cands[b].t; });
    // no dead sections: fill gaps longer than maxGap with the best candidate inside
    const double maxGap = 3.0 - 1.8 * part.intensity;
    for (int pass = 0; pass < 8; ++pass) {
        bool added = false;
        std::vector<double> times{t0 - 0.01};
        for (size_t c : chosen) times.push_back(cands[c].t);
        times.push_back(t1);
        for (size_t k = 0; k + 1 < times.size(); ++k) {
            if (times[k + 1] - times[k] <= maxGap) continue;
            double lo = times[k] + gap, hi = times[k + 1] - gap;
            size_t best = SIZE_MAX;
            for (size_t i = 0; i < cands.size(); ++i)
                if (cands[i].t >= lo && cands[i].t <= hi && (best == SIZE_MAX || cands[i].sal > cands[best].sal)) best = i;
            if (best != SIZE_MAX && fits(cands[best].t)) {
                chosen.push_back(best);
                added = true;
            }
        }
        if (!added) break;
        std::sort(chosen.begin(), chosen.end(), [&](size_t a, size_t b) { return cands[a].t < cands[b].t; });
    }
    // ground modes: a click faster than the jump airtime needs an orb, and orb chains climb, so
    // at most two fast clicks in a row (the weaker one of a third goes)
    if (part.mode == Mode::Cube || part.mode == Mode::Robot) {
        const double air = (part.mode == Mode::Robot ? 0.9 : 0.95) * cubeAirtime(part.speed, part.mini);
        std::vector<size_t> kept;
        int fastRun = 0;
        for (size_t c : chosen) {
            bool fast = !kept.empty() && cands[c].t - cands[kept.back()].t < air;
            if (fast && fastRun >= 2) {
                if (cands[c].sal > cands[kept.back()].sal * 1.2f && kept.size() >= 2 && cands[c].t - cands[kept[kept.size() - 2]].t >= gap) kept.back() = c;
                continue;
            }
            fastRun = fast ? fastRun + 1 : 0;
            kept.push_back(c);
        }
        chosen = std::move(kept);
    }
    for (size_t idx = 0; idx < chosen.size(); ++idx) {
        const auto& c = cands[chosen[idx]];
        Click k;
        k.time = c.t;
        k.role = c.role;
        k.strength = std::min(1.0f, c.sal);
        k.pitch = c.pitch;
        double next = idx + 1 < chosen.size() ? cands[chosen[idx + 1]].t : t1;
        double tap = 0.05;
        if (part.mode == Mode::Robot && c.sustain > 0.12) { k.hold = true; tap = std::min(0.3, c.sustain); }
        if (part.mode == Mode::Ship) {
            // climb for the length of a sustained note, else roughly half the gap to the next click
            double len = c.sustain > 0.1 ? c.sustain : 0.5 * (next - c.t) * (1.0 + 0.4 * opt.holdBias);
            tap = std::clamp(len, 0.08, std::max(0.08, next - c.t - 0.06));
            k.hold = true;
        }
        k.release = c.t + std::min(tap, std::max(0.02, next - c.t - 0.02));
        k.part = part.index;
        out.push_back(k);
    }
    return out;
}

DesignPlan planFromMusic(const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng) {
    DesignPlan plan;
    plan.duration = A.duration;
    plan.baseDifficulty = resolveDifficulty(A, opt);
    plan.style = resolveStyle(A, opt, rng);
    Rng partRng = rng.fork(hashStr("parts"));
    plan.parts = planParts(A, opt, plan.baseDifficulty, plan.style, partRng, &plan.baseSpeed);
    for (auto& p : plan.parts) {
        Rng cr = rng.fork(hashStr("clicks") ^ uint64_t(p.index * 7919));
        auto clicks = selectClicks(A, p, opt, plan.style, cr);
        for (auto& c : clicks) {
            p.clicks.push_back(int(plan.clicks.size()));
            plan.clicks.push_back(c);
        }
    }
    return plan;
}

void replanPart(DesignPlan& plan, int partIndex, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng) {
    if (partIndex < 0 || partIndex >= int(plan.parts.size())) return;
    auto& p = plan.parts[size_t(partIndex)];
    p.salt = rng.next() | (uint64_t(rng.next()) << 32);
    // new click selection (and possibly a different gamemode) for this part only
    float w[kModeCount];
    for (int m = 0; m < kModeCount; ++m) {
        float a = kAffinity[int(p.intent)][m];
        if (opt.modeWeights[size_t(m)] >= 0) a *= 2.0f * opt.modeWeights[size_t(m)];
        if (Mode(m) == Mode::Wave && p.difficulty < 1.5f) a *= 0.15f;
        w[m] = a * a;
    }
    if (rng.chance(0.6)) {
        int m = rng.weighted(w, size_t(kModeCount));
        if (m >= 0) p.mode = Mode(m);
    }
    std::vector<Click> keep;
    std::vector<int> remap(plan.clicks.size(), -1);
    for (size_t i = 0; i < plan.clicks.size(); ++i) if (plan.clicks[i].part != partIndex) { remap[i] = int(keep.size()); keep.push_back(plan.clicks[i]); }
    auto fresh = selectClicks(A, p, opt, plan.style, rng);
    plan.clicks = std::move(keep);
    for (auto& c : fresh) plan.clicks.push_back(c);
    std::stable_sort(plan.clicks.begin(), plan.clicks.end(), [](const Click& a, const Click& b) { return a.time < b.time; });
    for (auto& part : plan.parts) part.clicks.clear();
    for (size_t i = 0; i < plan.clicks.size(); ++i) {
        int pi = plan.clicks[i].part;
        if (pi >= 0 && pi < int(plan.parts.size())) plan.parts[size_t(pi)].clicks.push_back(int(i));
    }
}

std::vector<Moment> DesignPlan::moments() const {
    std::vector<Moment> out;
    for (auto& p : parts) {
        for (int ci : p.clicks) {
            const auto& c = clicks[size_t(ci)];
            Moment m;
            m.time = c.time;
            const char* verb = "jump";
            switch (p.mode) {
                case Mode::Ship: verb = c.hold ? "climb" : "tap"; break;
                case Mode::Wave: verb = "zig-zag"; break;
                case Mode::Ufo: verb = "hop"; break;
                case Mode::Ball: verb = "switch gravity"; break;
                case Mode::Spider: verb = "teleport"; break;
                case Mode::Swing: verb = "flip"; break;
                case Mode::Robot: verb = c.hold ? "high jump" : "jump"; break;
                default: break;
            }
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s %s", level::modeName(p.mode), verb);
            m.action = buf;
            std::snprintf(buf, sizeof(buf), "%s (%.2f)", roleName(c.role), c.strength);
            m.music = buf;
            m.intensity = p.intensity;
            const char* intent = p.intent == Intent::Climax ? (c.strength > 0.8f ? "hit the drop hard" : "keep the energy up")
                               : p.intent == Intent::Build ? "tighten towards the drop"
                               : p.intent == Intent::Breather ? "calm breather"
                               : p.intent == Intent::Intro ? "ease the player in"
                               : p.intent == Intent::Outro ? "wind down" : "ride the groove";
            m.intent = intent;
            out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end(), [](const Moment& a, const Moment& b) { return a.time < b.time; });
    return out;
}

} // namespace al::gen
