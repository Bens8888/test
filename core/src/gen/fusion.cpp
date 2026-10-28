#include <al/gen/fusion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace al::gen {

const char* figureName(Figure f) {
    switch (f) {
        case Figure::Tap: return "tap";
        case Figure::Hold: return "hold";
        case Figure::Double: return "double";
        case Figure::Stream: return "stream";
    }
    return "?";
}

namespace {

// nearest candidate to t (index), or -1
int nearest(const std::vector<Candidate>& c, double t) {
    auto it = std::lower_bound(c.begin(), c.end(), t, [](const Candidate& a, double v) { return a.t < v; });
    int best = -1;
    double bd = 1e9;
    for (auto j : {it - 1, it}) {
        if (j < c.begin() || j >= c.end()) continue;
        double d = std::fabs(j->t - t);
        if (d < bd) { bd = d; best = int(j - c.begin()); }
    }
    return best;
}

} // namespace

std::string ClickFusion::summary() const {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%zu clicks: %.0f%% on the music (mean offset %.0f ms, your latency %+.0f ms removed), %d holds, %d doubles, %d streams, %zu motifs%s",
                  clicks.size(), matchedFraction * 100.0, meanAbsOffsetMs, latencyMs, holds, doubles, streams, motifs.size(),
                  hasModes ? ", gamemodes recorded" : "");
    return buf;
}

ClickFusion fuseClicks(const rec::Recording& rec, const audio::MusicAnalysis& A) {
    ClickFusion F;
    // ---- presses with their releases and states (player 1 jump button)
    const rec::RecEvent* open = nullptr;
    for (const auto& e : rec.events) {
        if (e.button != rec::Button::Jump || e.player2) {
            continue;
        }
        TrajectoryPoint tp;
        tp.time = rec.songOffset + e.time;
        tp.x = e.p1.x;
        tp.y = e.p1.y;
        tp.mode = Mode(std::min<int>(e.p1.mode, 7));
        tp.speed = e.p1.speed;
        tp.mini = e.p1.mini;
        tp.upsideDown = e.p1.upsideDown;
        F.trajectory.push_back(tp);
        if (e.press) {
            ClickInfo c;
            c.rawTime = c.time = rec.songOffset + e.time;
            c.release = c.time + 0.05;
            c.state = e.p1;
            c.hasState = rec.source == "recorded";
            F.clicks.push_back(c);
            open = &e;
        } else if (open && !F.clicks.empty()) {
            F.clicks.back().release = std::max(F.clicks.back().time + 0.01, rec.songOffset + e.time);
            open = nullptr;
        }
    }
    if (F.clicks.empty()) return F;
    // ---- gamemode information
    {
        int counts[8] = {};
        for (auto& c : F.clicks) if (c.hasState) ++counts[std::min<int>(c.state.mode, 7)];
        int distinct = 0;
        for (int m = 0; m < 8; ++m) distinct += counts[m] > 0 ? 1 : 0;
        F.hasModes = distinct > 1 || (distinct == 1 && counts[0] == 0);
    }
    // ---- timing bias: median offset to strong musical events
    auto cands = gatherCandidates(A, 0, A.duration + 1, 0.0f);
    std::vector<double> offs;
    for (auto& c : F.clicks) {
        int j = nearest(cands, c.rawTime);
        if (j >= 0 && cands[size_t(j)].sal >= 0.6f && std::fabs(cands[size_t(j)].t - c.rawTime) < 0.09) offs.push_back(c.rawTime - cands[size_t(j)].t);
    }
    if (offs.size() >= 5) {
        std::nth_element(offs.begin(), offs.begin() + long(offs.size() / 2), offs.end());
        F.latencyMs = std::clamp(offs[offs.size() / 2] * 1000.0, -60.0, 60.0);
    }
    // ---- musical role per click
    double absSum = 0;
    int matched = 0;
    for (auto& c : F.clicks) {
        c.time = c.rawTime - F.latencyMs / 1000.0;
        c.release -= F.latencyMs / 1000.0;
        int j = nearest(cands, c.time);
        if (j >= 0 && std::fabs(cands[size_t(j)].t - c.time) <= 0.07) {
            const auto& m = cands[size_t(j)];
            c.role = m.role;
            c.strength = std::min(1.0f, m.sal);
            c.musicTime = m.t;
            c.offsetMs = float((c.time - m.t) * 1000.0);
            c.pitch = m.pitch;
            absSum += std::fabs(c.offsetMs);
            ++matched;
        } else {
            c.role = Role::Recorded;
            c.pitch = A.pitchAt(c.time);
        }
    }
    F.matchedFraction = float(matched) / float(F.clicks.size());
    F.meanAbsOffsetMs = matched ? float(absSum / matched) : 0.0f;
    // ---- figures
    for (auto& c : F.clicks) if (c.release - c.time >= 0.15) { c.hold = true; c.figure = Figure::Hold; ++F.holds; }
    for (size_t i = 0; i < F.clicks.size();) {
        size_t j = i;
        while (j + 1 < F.clicks.size() && F.clicks[j + 1].time - F.clicks[j].time < 0.22) ++j;
        size_t n = j - i + 1;
        if (n == 2) {
            for (size_t k = i; k <= j; ++k) if (!F.clicks[k].hold) F.clicks[k].figure = Figure::Double;
            ++F.doubles;
        } else if (n >= 3) {
            for (size_t k = i; k <= j; ++k) if (!F.clicks[k].hold) F.clicks[k].figure = Figure::Stream;
            ++F.streams;
        }
        i = j + 1;
    }
    // ---- motifs: identical click patterns (in 16ths) in two or more bars
    if (A.downbeats.size() >= 2) {
        std::map<std::vector<int>, std::vector<size_t>> byPattern;
        std::vector<std::vector<size_t>> barClicks(A.downbeats.size());
        size_t ci = 0;
        for (size_t b = 0; b + 1 < A.downbeats.size(); ++b) {
            double d0 = A.downbeats[b], d1 = A.downbeats[b + 1];
            double step = (d1 - d0) / 16.0;
            std::vector<int> pat;
            while (ci < F.clicks.size() && F.clicks[ci].time < d0 - step * 0.5) ++ci;
            for (size_t k = ci; k < F.clicks.size() && F.clicks[k].time < d1 - step * 0.5; ++k) {
                pat.push_back(int(std::lround((F.clicks[k].time - d0) / step)));
                barClicks[b].push_back(k);
            }
            if (pat.size() >= 2) byPattern[pat].push_back(b);
        }
        for (auto& [pat, bars] : byPattern) {
            if (bars.size() < 2) continue;
            Motif m;
            m.id = int(F.motifs.size());
            m.pattern = pat;
            for (size_t b : bars) {
                m.barStarts.push_back(A.downbeats[b]);
                for (size_t k = 0; k < barClicks[b].size(); ++k) {
                    F.clicks[barClicks[b][k]].motif = m.id;
                    F.clicks[barClicks[b][k]].motifPos = int(k);
                }
            }
            F.motifs.push_back(m);
        }
    }
    return F;
}

double levelStartFor(const rec::Recording& r) {
    auto presses = r.pressTimes();
    if (presses.empty()) return 0;
    const double first = presses.front();
    // a short run-up before the first click; nothing to trim for a click in the first seconds
    return first > 6.0 ? first - 2.5 : 0.0;
}

rec::Recording shiftRecording(const rec::Recording& r, double songStart) {
    rec::Recording out = r;
    out.songOffset -= songStart;
    return out;
}

DesignPlan planFromRecording(const ClickFusion& F, const audio::MusicAnalysis& A, const GenOptions& opt, Rng& rng) {
    const double f = std::clamp(double(opt.faithfulness), 0.0, 1.0);
    DesignPlan plan;
    // the level ends shortly after the last recorded click (a recording of part of the song
    // makes a level of that part, not a long empty run to the end of the song)
    plan.duration = A.duration;
    if (!F.clicks.empty()) {
        double last = 0;
        for (const auto& c : F.clicks) last = std::max(last, c.release);
        plan.duration = std::min(A.duration, last + 2.0);
    }
    plan.baseDifficulty = resolveDifficulty(A, opt);
    plan.style = resolveStyle(A, opt, rng);

    // ---- gamemodes that fit the recorded rhythm (or the recorded gamemodes themselves)
    auto clicksIn = [&](double t0, double t1) {
        std::vector<const ClickInfo*> v;
        for (auto& c : F.clicks) if (c.time >= t0 && c.time < t1) v.push_back(&c);
        return v;
    };
    ModeFit fit = [&](const Part& p, Mode m) -> float {
        auto v = clicksIn(p.t0, p.t1);
        if (v.empty()) return 1.0f;
        if (F.hasModes && f >= 0.6) {
            int counts[8] = {};
            for (auto* c : v) if (c->hasState) ++counts[std::min<int>(c->state.mode, 7)];
            int major = int(std::max_element(counts, counts + 8) - counts);
            return int(m) == major ? 6.0f : 0.2f;
        }
        std::vector<double> gaps;
        int holds = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i > 0) gaps.push_back(v[i]->time - v[i - 1]->time);
            holds += v[i]->hold ? 1 : 0;
        }
        float w = 1.0f;
        if (!gaps.empty()) {
            std::sort(gaps.begin(), gaps.end());
            double g10 = gaps[gaps.size() / 10];                      // the fast end of the rhythm
            double need = minGapFor(m, p.speed, p.mini, profileAt(p.difficulty));
            if (need > g10 * 1.05) w *= float(std::max(0.08, std::pow(g10 / need, 3.0)));
        }
        float holdFrac = float(holds) / float(v.size());
        if (holdFrac > 0.25f && (m == Mode::Ship || m == Mode::Robot)) w *= 1.0f + 3.0f * holdFrac;
        if (holdFrac < 0.05f && m == Mode::Ship) w *= 0.6f;
        return w;
    };
    Rng partRng = rng.fork(hashStr("parts"));
    plan.parts = planParts(A, opt, plan.baseDifficulty, plan.style, partRng, &plan.baseSpeed, &fit);
    while (plan.parts.size() > 1 && plan.parts.back().t0 >= plan.duration - 0.5) plan.parts.pop_back();
    if (!plan.parts.empty()) plan.parts.back().t1 = plan.duration;

    // ---- clicks from the recording
    auto partAt = [&](double t) {
        for (auto& p : plan.parts) if (t >= p.t0 && t < p.t1) return p.index;
        return plan.parts.empty() ? -1 : plan.parts.back().index;
    };
    const double snapTol = (1.0 - f) * 0.08;
    std::vector<Click> clicks;
    for (const auto& c : F.clicks) {
        // the player's measured timing bias is always removed (tapping to what you hear is late
        // by the audio latency plus your reaction); snapping to the music only below strict
        double t = c.time;
        if (c.musicTime >= 0 && std::fabs(t - c.musicTime) <= snapTol) t = c.musicTime;
        bool keepAlways = c.figure == Figure::Stream || c.motif >= 0 || c.hold;
        if (f < 0.5 && c.strength == 0 && !keepAlways && rng.chance((0.5 - f) * 1.6)) continue;
        Click k;
        k.time = t;
        k.release = t + std::max(0.02, c.release - c.time);
        k.hold = c.hold;
        k.role = c.strength > 0 ? c.role : Role::Recorded;
        k.strength = c.strength > 0 ? c.strength : 0.5f;
        k.pitch = c.pitch;
        k.recorded = true;
        k.motif = c.motif;
        k.motifPos = c.motifPos;
        k.part = partAt(t);
        clicks.push_back(k);
    }
    std::stable_sort(clicks.begin(), clicks.end(), [](const Click& a, const Click& b) { return a.time < b.time; });
    // ---- spacing (classic builder): below full faithfulness, clicks closer than the part's
    // gamemode can express are thinned (the weaker one goes). The path-first builder picks its
    // gamemodes for the clicks themselves and keeps them all.
    if (f < 0.95 && opt.engine != 2) {
        const double scale = 0.6 + 0.4 * (1.0 - f);
        std::vector<Click> kept;
        for (auto& k : clicks) {
            if (!kept.empty() && k.part == kept.back().part && k.part >= 0) {
                const auto& p = plan.parts[size_t(k.part)];
                double need = minGapFor(p.mode, p.speed, p.mini, profileAt(p.difficulty)) * scale;
                if (k.time - kept.back().time < need) {
                    if (k.strength > kept.back().strength) kept.back() = k;
                    continue;
                }
            }
            kept.push_back(k);
        }
        clicks = std::move(kept);
    }
    // ---- creative fill: long silent stretches get clicks from the music
    if (f < 0.8) {
        const double maxGap = 1.5 + 3.5 * f / 0.8;
        std::vector<Click> extra;
        for (auto& p : plan.parts) {
            const double need = minGapFor(p.mode, p.speed, p.mini, profileAt(p.difficulty));
            std::vector<double> times{p.t0 + (p.index == 0 ? 1.0 : 0.45)};
            for (auto& k : clicks) if (k.part == p.index) times.push_back(k.time);
            times.push_back(p.t1 - 0.3);
            for (size_t i = 0; i + 1 < times.size(); ++i) {
                double a = times[i], b = times[i + 1];
                if (b - a <= maxGap) continue;
                auto cands = gatherCandidates(A, a + need, b - need, opt.holdBias);
                std::sort(cands.begin(), cands.end(), [](const Candidate& x, const Candidate& y) { return x.sal > y.sal; });
                std::vector<double> placed{a, b};
                for (auto& cd : cands) {
                    bool ok = true;
                    for (double t : placed) if (std::fabs(t - cd.t) < std::max(need, maxGap * 0.45)) ok = false;
                    if (!ok) continue;
                    Click k;
                    k.time = cd.t;
                    k.release = cd.t + 0.05;
                    k.role = cd.role;
                    k.strength = std::min(1.0f, cd.sal);
                    k.pitch = cd.pitch;
                    k.part = p.index;
                    if (p.mode == Mode::Ship) { k.hold = true; k.release = cd.t + 0.18; }
                    extra.push_back(k);
                    placed.push_back(cd.t);
                }
            }
        }
        for (auto& k : extra) clicks.push_back(k);
        std::stable_sort(clicks.begin(), clicks.end(), [](const Click& a, const Click& b) { return a.time < b.time; });
    }
    // ship climbs need a minimum hold; nothing may run into the next click
    for (size_t i = 0; i < clicks.size(); ++i) {
        auto& k = clicks[i];
        if (k.part >= 0 && plan.parts[size_t(k.part)].mode == Mode::Ship) k.release = std::max(k.release, k.time + 0.08);
        if (i + 1 < clicks.size()) k.release = std::min(k.release, clicks[i + 1].time - 0.02);
        k.release = std::max(k.release, k.time + 0.01);
    }
    plan.clicks = std::move(clicks);
    for (size_t i = 0; i < plan.clicks.size(); ++i) {
        int pi = plan.clicks[i].part;
        if (pi >= 0 && pi < int(plan.parts.size())) plan.parts[size_t(pi)].clicks.push_back(int(i));
    }
    for (auto& p : plan.parts) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " | %zu recorded clicks", std::count_if(p.clicks.begin(), p.clicks.end(), [&](int ci) { return plan.clicks[size_t(ci)].recorded; }));
        p.why += buf;
    }
    return plan;
}

} // namespace al::gen
