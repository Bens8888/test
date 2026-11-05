// Generator v2: gamemode segments. The clicks are cut into stretches, each with one gamemode,
// speed and size; the cut points prefer musical boundaries (section changes, downbeats, strong
// hits) and gaps long enough for a portal. How often the mode changes comes from the style and
// the music's intensity (drops switch more often).
#include <al/gen/path.hpp>
#include <al/gen/profile.hpp>

#include <algorithm>
#include <cmath>

namespace al::gen {

namespace {

const Part* partAt(const DesignPlan& plan, double t) {
    for (const auto& p : plan.parts) if (t >= p.t0 && t < p.t1) return &p;
    return plan.parts.empty() ? nullptr : &plan.parts.back();
}

bool nearDownbeat(const audio::MusicAnalysis& A, double t) {
    auto it = std::lower_bound(A.downbeats.begin(), A.downbeats.end(), t - 0.04);
    return it != A.downbeats.end() && std::fabs(*it - t) < 0.04;
}

// How well a gamemode expresses a stretch of clicks: median gap, shortest gap, share of holds.
float rhythmFit(Mode m, double med, double shortest, float holds, double cubeMin = 0.24) {
    auto band = [](double x, double lo, double hi) { return x >= lo && x <= hi ? 1.0f : float(std::exp(-std::pow((x < lo ? lo - x : x - hi) / 0.08, 2))); };
    switch (m) {
        // a cube cannot land between clicks much under a quarter second apart: every click would be
        // an orb, a chain no real layout has (faster rhythms are ufo, ball, wave or ship)
        case Mode::Cube: {
            const float fast = med < cubeMin ? float(std::exp(-std::pow((cubeMin - med) / 0.04, 2))) : 1.0f;
            return (0.25f + 0.75f * band(med, cubeMin, 0.8) - 0.3f * holds) * fast;
        }
        case Mode::Robot: return 0.2f + 0.5f * band(med, 0.26, 0.8) + 0.5f * holds;
        case Mode::Ball: return 0.2f + 0.7f * band(med, 0.22, 0.6);
        case Mode::Spider: return 0.2f + 0.7f * band(med, 0.16, 0.5);
        case Mode::Ufo: return 0.1f + 0.8f * band(med, 0.16, 0.42);
        case Mode::Ship: return 0.25f + 0.4f * band(med, 0.15, 0.6) + 0.6f * holds;
        // wave: long diagonal strokes between triangles (a click every quarter to half second)
        case Mode::Wave: return 0.15f + 0.85f * band(med, 0.2, 0.55) + 0.2f * holds - (shortest < 0.12 ? 0.3f : 0.0f);
        case Mode::Swing: return 0.2f + 0.7f * band(med, 0.14, 0.4);
        default: return 0.1f;
    }
}

// How much of a real layout each gamemode gets: cube and ship carry most of it, spider and swing
// are seasoning (cube, ship, ball, ufo, wave, robot, spider, swing).
constexpr float kModeShare[kModeCount] = {2.4f, 1.8f, 0.8f, 0.9f, 1.3f, 0.8f, 0.35f, 0.3f};

float styleSwitchRate(Style s) {
    switch (s) {
        case Style::Minimal: return 0.25f;
        case Style::Retro: return 0.2f;
        case Style::Flow: return 0.45f;
        case Style::Memory: return 0.55f;
        case Style::Tight: return 0.6f;
        case Style::Chaotic: return 0.75f;
        default: return 0.55f; // a mode portal every few seconds, as in real layouts
    }
}

} // namespace

std::vector<PathSegment> planSegments(const DesignPlan& plan, const audio::MusicAnalysis& A, const GenOptions& opt, float switchRate, Rng& rng,
                                      const std::vector<uint64_t>* salts) {
    std::vector<PathSegment> segs;
    const auto& C = plan.clicks;
    const int n = int(C.size());
    if (switchRate < 0) switchRate = styleSwitchRate(plan.style);
    switchRate = std::clamp(switchRate, 0.0f, 1.0f);
    // ---- cut points
    std::vector<std::pair<int, int>> ranges;
    for (int i = 0; i < n;) {
        const Part* P = partAt(plan, C[size_t(i)].time);
        const float intensity = P ? P->intensity : 0.5f;
        const float rate = std::clamp(switchRate * (0.6f + 0.8f * intensity), 0.0f, 1.0f);
        const double meanLen = 20.0 + (1.6 - 20.0) * rate;
        // (demons: a part every four to five seconds, an Extreme every three or four, as measured)
        const double demonL = plan.baseDifficulty >= 9.0f ? 1.15 : plan.baseDifficulty >= 5.0f ? 1.4 : 1.0;
        const double easierL = (1.0 + 0.22 * std::clamp(6.5 - double(plan.baseDifficulty), 0.0, 5.0)) * demonL;
        const int L = std::max(1, int(std::lround(meanLen * easierL * rng.uniform(0.6, 1.4))));
        // a stretch needs a few seconds: structures repeat over it, and it reads as a part
        // (longer below demon ratings: fewer portals to react to)
        const double easier = (1.0 + 0.27 * std::clamp(6.5 - double(plan.baseDifficulty), 0.0, 5.0)) * demonL;
        const double minLen = switchRate >= 1.0f ? 0.0 : (2.5 * (1.0 - switchRate) + 1.2) * easier;
        int lo = std::min(n - 1, i + std::max(0, L / 2 - 1));
        while (lo + 1 < n && C[size_t(lo)].time - C[size_t(i)].time < minLen) ++lo;
        const int hi = std::min(n - 1, std::max(lo, i + L + L / 2));
        int best = hi;
        double bestScore = -1e9;
        for (int e = lo; e <= hi; ++e) {
            double s;
            if (e == n - 1) s = 1.0;
            else {
                const double gap = C[size_t(e + 1)].time - C[size_t(e)].time;
                const Part* a = partAt(plan, C[size_t(e)].time);
                const Part* b = partAt(plan, C[size_t(e + 1)].time);
                s = std::min(gap, 1.0) * 2.0 + (nearDownbeat(A, C[size_t(e + 1)].time) ? 0.8 : 0.0) + (a != b ? 2.5 : 0.0) +
                    0.5 * C[size_t(e + 1)].strength - 0.15 * std::abs(e - (i + L - 1));
                if (gap < 0.24) s -= 5; // no room for a portal and a reaction
            }
            if (s > bestScore) { bestScore = s; best = e; }
        }
        ranges.push_back({i, best});
        i = best + 1;
    }
    if (ranges.empty()) ranges.push_back({0, -1});
    // ---- gamemode, speed, size, gravity per segment
    const float D = plan.baseDifficulty;
    // demons run at normal speed with the hazards packed close, as real ones do (Bloodbath, Sonic
    // Wave): fast speeds would need everything further from the path for the same timing window
    const int baseSpeed = D < 2.5f || D >= 5.0f ? 1 : 2;
    Mode prev = Mode::Count, prev2 = Mode::Count;
    // each gamemode's share of the time in real levels (about 700 of them, all ratings): cube near
    // half, ship a fifth, spider and swing rare; a mode behind its share so far is picked more often
    const float target[kModeCount] = {0.42f, 0.20f, 0.10f, 0.09f, D >= 5 ? 0.12f : 0.07f, 0.07f, 0.03f, 0.015f};
    double spent[kModeCount] = {0}, spentAll = 0;
    for (auto [a, b] : ranges) {
        // the segment's own choices come from its own stream (re-rolled with its salt)
        const size_t si = segs.size();
        const uint64_t salt = salts && si < salts->size() ? (*salts)[si] : 0;
        Rng own = rng.fork(combineSeed(0x5E6E0000ull + si, salt));
        PathSegment sg;
        sg.first = a;
        sg.last = b;
        const double t = a < n ? C[size_t(a)].time : 0.0;
        const Part* P = partAt(plan, t);
        sg.intensity = P ? P->intensity : 0.5f;
        std::vector<double> gaps;
        int holds = 0;
        for (int j = a; j <= b && j < n; ++j) {
            if (j + 1 < n) gaps.push_back(C[size_t(j + 1)].time - C[size_t(j)].time);
            if (C[size_t(j)].hold || C[size_t(j)].release - C[size_t(j)].time > 0.15) ++holds;
        }
        std::vector<double> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        const double med = sorted.empty() ? 0.5 : sorted[sorted.size() / 2];
        const double shortest = sorted.empty() ? 0.5 : sorted.front();
        // the longest wait for a click, the lead-in before the first one included
        double longest = sorted.empty() ? 0.0 : sorted.back();
        if (a < n) longest = std::max(longest, C[size_t(a)].time - (a > 0 ? C[size_t(a - 1)].time : 0.0));
        const float holdFrac = float(holds) / float(std::max(1, b - a + 1));
        // a cube in a fast part jumps on the main hits and lets the quick notes between go (as in
        // real Harder levels and demons, cube near 40% of the time): its rhythm is the thinned one
        double cubeMed = med;
        float cubeKeep = 1.0f;
        if (D >= 3 && b > a) {
            std::vector<double> kept;
            double last = -1.0;
            int dropped = 0;
            for (int j = a; j <= b && j < n; ++j) {
                const double tj = C[size_t(j)].time;
                if (last < 0 || tj - last >= kCubeGap || (C[size_t(j)].strength > 0.75f && tj - last >= 0.2)) {
                    if (last >= 0) kept.push_back(tj - last);
                    last = tj;
                } else ++dropped;
            }
            if (!kept.empty()) {
                std::sort(kept.begin(), kept.end());
                cubeMed = kept[kept.size() / 2];
            }
            // every other note let go is fine (a 16th-note part jumped on the 8ths), more is not
            cubeKeep = std::clamp(1.0f - 1.5f * std::max(0.0f, float(dropped) / float(b - a + 1) - 0.5f), 0.2f, 1.0f);
        }
        float w[kModeCount];
        float total = 0;
        for (int m = 0; m < kModeCount; ++m) {
            float user = opt.modeWeights[size_t(m)] < 0 ? 1.0f : opt.modeWeights[size_t(m)];
            float x = (Mode(m) == Mode::Cube ? std::max(rhythmFit(Mode::Cube, med, shortest, holdFrac), rhythmFit(Mode::Cube, cubeMed, shortest, holdFrac) * cubeKeep)
                                             : rhythmFit(Mode(m), med, shortest, holdFrac)) * user;
            if (opt.profile) x *= opt.profile->modeFactor(m);
            if (Mode(m) == prev) x *= 0.12f;
            if (Mode(m) == prev2) x *= 0.6f;
            if (D < 2 && (Mode(m) == Mode::Wave || Mode(m) == Mode::Swing || Mode(m) == Mode::Spider)) x *= 0.3f;
            // a wave with nothing to click slides along the floor, a swing falls onto it
            if (longest > 0.75 && (Mode(m) == Mode::Wave || Mode(m) == Mode::Swing)) x *= Mode(m) == Mode::Wave ? 0.1f : 0.4f;
            // below demon ratings a wave turns no faster than about four times a second
            if (Mode(m) == Mode::Wave && D < 5 && (med < 0.24 || shortest < 0.16)) x *= 0.2f;
            w[m] = x * x * kModeShare[m];
            if (spentAll > 10.0) w[m] *= std::clamp(float((target[m] + 0.02) / (spent[m] / spentAll + 0.02)), 0.25f, 4.0f);
            total += w[m];
        }
        if (total <= 0) { for (int m = 0; m < kModeCount; ++m) w[m] = opt.modeWeights[size_t(m)] != 0 ? 1.0f : 0.0f; total = 1; }
        auto draw = [&](Rng& from) {
            double r = from.uniform() * double(total);
            int p = 0;
            for (int m = 0; m < kModeCount; ++m) {
                if (w[m] <= 0) continue;
                p = m;
                if ((r -= w[m]) <= 0) break;
            }
            return p;
        };
        int pick = draw(own);
        // a re-rolled segment takes another gamemode than the one it had
        if (salt != 0) {
            Rng base = rng.fork(combineSeed(0x5E6E0000ull + si, 0));
            const int was = draw(base);
            if (pick == was && total > w[was] + 1e-6f) {
                total -= w[was];
                w[was] = 0;
                pick = draw(own);
            }
        }
        sg.mode = Mode(pick);
        {
            const double dur = (b + 1 < n ? C[size_t(b + 1)].time : C[size_t(std::max(a, std::min(b, n - 1)))].time + 1.0) - t;
            spent[pick] += std::max(0.0, dur);
            spentAll += std::max(0.0, dur);
        }
        // speed: the base speed, faster in intense parts, slower in breathers
        int sp = baseSpeed;
        const Intent intent = P ? P->intent : Intent::Groove;
        const SpeedChanges sc = opt.speedChanges == SpeedChanges::Auto ? SpeedChanges::Subtle : opt.speedChanges;
        if (sc == SpeedChanges::Subtle) {
            // real levels change speed often (Harder about 12 times, Extreme demons over 40)
            if (intent == Intent::Climax && sg.intensity > 0.7f && D >= 4) sp += 1;
            else if (intent == Intent::Build && D >= 3 && own.chance(0.35)) sp += 1;
            else if (D >= 2 && sg.intensity > 0.6f && own.chance(0.3)) sp += 1;
            if (intent == Intent::Breather || (sg.intensity < 0.35f && sg.mode != Mode::Spider && own.chance(0.4))) sp -= 1;
        } else if (sc == SpeedChanges::Aggressive) {
            if (intent == Intent::Climax) sp += sg.intensity > 0.75f ? 2 : 1;
            else if (intent == Intent::Build) sp += 1;
            else if (intent == Intent::Breather) sp -= 1;
        }
        // a long climax would run at one speed: real levels switch back and forth between parts
        if (sc != SpeedChanges::Off && !segs.empty() && sp == segs.back().speed && D >= 2 && sg.mode != Mode::Spider && own.chance(0.35)) sp += sp > baseSpeed ? -1 : 1;
        if (opt.startSpeed >= 0 && sc == SpeedChanges::Off) sp = opt.startSpeed;
        if (opt.speedStyle == 1) sp -= 1;
        else if (opt.speedStyle == 3) sp += 1;
        int cap = D >= 6 ? 3 : (D >= 3 ? 3 : 2);
        if ((sg.mode == Mode::Wave || sg.mode == Mode::Swing || sg.mode == Mode::Spider) && D < 6) cap = 2;
        // slow speed only in a breather
        // (not in demons: at half speed the portals into the next part need more room than the
        // quick clicks leave, and the switch never happens)
        const bool slowOk = D < 5 && sg.mode != Mode::Spider && sg.mode != Mode::Wave && sg.mode != Mode::Swing;
        sg.speed = std::clamp(sp, slowOk && (intent == Intent::Breather || sg.intensity < 0.35f) ? 0 : 1, cap);
        if (sg.mode == Mode::Wave) sg.speed = std::max(sg.speed, 2); // longer strokes, bigger triangles
        // size: mini for fast streams in flying modes
        const double miniChance = opt.sizeChanges == Tri::Off ? 0.0 : opt.sizeChanges == Tri::On ? 0.4 : (D >= 3 && med < 0.25 ? 0.3 : 0.08);
        sg.mini = ((sg.mode == Mode::Wave && opt.sizeChanges == Tri::On) || sg.mode == Mode::Ship || sg.mode == Mode::Ufo || sg.mode == Mode::Cube || sg.mode == Mode::Ball) && own.chance(miniChance);
        // hard wave parts are mini: steep strokes in tight corridors
        // (only for fast clicks: at half a second a stroke would run from the floor to the ceiling)
        if (sg.mode == Mode::Wave && opt.sizeChanges == Tri::Auto && D >= 6 && med < 0.26) sg.mini = own.chance(0.7);
        // gravity: some ground segments run upside down
        const double flipChance = opt.gravityFlips == Tri::Off ? 0.0 : opt.gravityFlips == Tri::On ? 0.35 : (D >= 5 ? 0.2 : D >= 3 ? 0.18 : D >= 1 ? 0.06 : 0.0);
        sg.flipped = (sg.mode == Mode::Cube || sg.mode == Mode::Robot || sg.mode == Mode::Ball || sg.mode == Mode::Spider || sg.mode == Mode::Ufo) && a > 0 && own.chance(flipChance);
        sg.why = std::string(modeName(sg.mode)) + ": median gap " + std::to_string(int(std::lround(med * 1000))) + " ms" +
                 (holdFrac > 0.3f ? ", holds" : "") + (P ? std::string(", ") + intentName(P->intent) : "");
        prev2 = prev;
        prev = sg.mode;
        segs.push_back(sg);
    }
    return segs;
}

DesignPlan planFromMusicV2(const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng) {
    DesignPlan plan;
    plan.duration = A.duration;
    plan.baseDifficulty = resolveDifficulty(A, opt);
    plan.style = resolveStyle(A, opt, rng);
    Rng partRng = rng.fork(hashStr("parts"));
    plan.parts = planParts(A, opt, plan.baseDifficulty, plan.style, partRng, &plan.baseSpeed);
    for (auto& p : plan.parts) {
        // choose the clicks as for a neutral mode (UFO: every musical gap from ~0.13 s works)
        Part neutral = p;
        neutral.mode = Mode::Ufo;
        Rng cr = rng.fork(hashStr("clicks") ^ uint64_t(p.index * 7919));
        auto clicks = selectClicks(A, neutral, opt, plan.style, cr);
        p.clicks.clear();
        for (auto& c : clicks) {
            p.clicks.push_back(int(plan.clicks.size()));
            plan.clicks.push_back(c);
        }
    }
    return plan;
}

} // namespace al::gen
