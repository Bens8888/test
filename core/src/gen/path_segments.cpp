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
float rhythmFit(Mode m, double med, double shortest, float holds) {
    auto band = [](double x, double lo, double hi) { return x >= lo && x <= hi ? 1.0f : float(std::exp(-std::pow((x < lo ? lo - x : x - hi) / 0.08, 2))); };
    switch (m) {
        case Mode::Cube: return 0.25f + 0.75f * band(med, 0.24, 0.8) - 0.3f * holds;
        case Mode::Robot: return 0.2f + 0.5f * band(med, 0.26, 0.8) + 0.5f * holds;
        case Mode::Ball: return 0.2f + 0.7f * band(med, 0.22, 0.6);
        case Mode::Spider: return 0.2f + 0.7f * band(med, 0.16, 0.5);
        case Mode::Ufo: return 0.1f + 0.8f * band(med, 0.16, 0.42);
        case Mode::Ship: return 0.25f + 0.4f * band(med, 0.15, 0.6) + 0.6f * holds;
        case Mode::Wave: return 0.15f + 0.85f * band(med, 0.08, 0.28) + 0.3f * holds + (shortest < 0.14 ? 0.3f : 0.0f);
        case Mode::Swing: return 0.2f + 0.7f * band(med, 0.14, 0.4);
        default: return 0.1f;
    }
}

float styleSwitchRate(Style s) {
    switch (s) {
        case Style::Minimal: return 0.25f;
        case Style::Retro: return 0.2f;
        case Style::Flow: return 0.45f;
        case Style::Memory: return 0.55f;
        case Style::Tight: return 0.6f;
        case Style::Chaotic: return 0.9f;
        default: return 0.7f; // modern layouts switch often
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
        const int L = std::max(1, int(std::lround(meanLen * rng.uniform(0.6, 1.4))));
        const int lo = std::min(n - 1, i + std::max(0, L / 2 - 1)), hi = std::min(n - 1, i + L + L / 2);
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
    const int baseSpeed = D < 2.5f ? 1 : 2;
    Mode prev = Mode::Count, prev2 = Mode::Count;
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
        const float holdFrac = float(holds) / float(std::max(1, b - a + 1));
        float w[kModeCount];
        float total = 0;
        for (int m = 0; m < kModeCount; ++m) {
            float user = opt.modeWeights[size_t(m)] < 0 ? 1.0f : opt.modeWeights[size_t(m)];
            float x = rhythmFit(Mode(m), med, shortest, holdFrac) * user;
            if (opt.profile) x *= opt.profile->modeFactor(m);
            if (Mode(m) == prev) x *= 0.12f;
            if (Mode(m) == prev2) x *= 0.6f;
            if (D < 2 && (Mode(m) == Mode::Wave || Mode(m) == Mode::Swing || Mode(m) == Mode::Spider)) x *= 0.3f;
            w[m] = x * x;
            total += w[m];
        }
        if (total <= 0) { for (int m = 0; m < kModeCount; ++m) w[m] = opt.modeWeights[size_t(m)] != 0 ? 1.0f : 0.0f; total = 1; }
        double r = own.uniform() * double(total);
        int pick = 0;
        for (int m = 0; m < kModeCount; ++m) {
            if (w[m] <= 0) continue;
            pick = m;
            if ((r -= w[m]) <= 0) break;
        }
        sg.mode = Mode(pick);
        // speed: the base speed, faster in intense parts, slower in breathers
        int sp = baseSpeed;
        const Intent intent = P ? P->intent : Intent::Groove;
        const SpeedChanges sc = opt.speedChanges == SpeedChanges::Auto ? SpeedChanges::Subtle : opt.speedChanges;
        if (sc == SpeedChanges::Subtle) {
            if (intent == Intent::Climax && sg.intensity > 0.7f && D >= 4) sp += 1;
            if (intent == Intent::Breather) sp -= 1;
        } else if (sc == SpeedChanges::Aggressive) {
            if (intent == Intent::Climax) sp += sg.intensity > 0.75f ? 2 : 1;
            else if (intent == Intent::Build) sp += 1;
            else if (intent == Intent::Breather) sp -= 1;
        }
        if (opt.startSpeed >= 0 && sc == SpeedChanges::Off) sp = opt.startSpeed;
        int cap = D >= 6 ? 3 : (D >= 3 ? 3 : 2);
        if ((sg.mode == Mode::Wave || sg.mode == Mode::Swing || sg.mode == Mode::Spider) && D < 6) cap = 2;
        sg.speed = std::clamp(sp, 1, cap);
        // size: mini for fast streams in flying modes
        const double miniChance = opt.sizeChanges == Tri::Off ? 0.0 : opt.sizeChanges == Tri::On ? 0.4 : (D >= 3 && med < 0.25 ? 0.3 : 0.08);
        sg.mini = (sg.mode == Mode::Wave || sg.mode == Mode::Ship || sg.mode == Mode::Ufo || sg.mode == Mode::Cube || sg.mode == Mode::Ball) && own.chance(miniChance);
        // gravity: some ground segments run upside down
        const double flipChance = opt.gravityFlips == Tri::Off ? 0.0 : opt.gravityFlips == Tri::On ? 0.35 : (D >= 3 ? 0.18 : 0.0);
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
