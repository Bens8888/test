#include <al/gen/generator.hpp>
#include <al/util/codec.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

namespace al::gen {

namespace oid = level::id;

// ---------------------------------------------------------------- report

Report makeReport(const level::Level& lvl, const bot::InputTrack& inputs, const DesignPlan& plan, const std::vector<ClickResult>& clicks,
                  const std::vector<PartResult>& parts, const audio::MusicAnalysis& A, bool beatable) {
    Report r;
    r.beatable = beatable;
    r.objects = int(lvl.objects.size());
    for (auto& o : lvl.objects) {
        auto k = o.info().kind;
        if (k == level::Kind::Trigger || k == level::Kind::EndTrigger) ++r.triggers;
        else if (k != level::Kind::Deco) ++r.gameplayObjects;
        if (k == level::Kind::ModePortal || k == level::Kind::SpeedPortal || k == level::Kind::GravityPortal || k == level::Kind::SizePortal ||
            k == level::Kind::DualPortal)
            ++r.portals;
    }
    double sumW = 0;
    int nW = 0;
    r.minWindowMs = 1e9;
    int strong = 0;
    for (auto& c : clicks) {
        ++r.clicks;
        if (c.dropped) { ++r.dropped; continue; }
        if (c.forced) ++r.forced;
        double w = c.windowMs();
        sumW += w;
        ++nW;
        r.minWindowMs = std::min(r.minWindowMs, w);
        const auto& ck = plan.clicks[size_t(c.click)];
        const auto& part = plan.parts[size_t(std::clamp(ck.part, 0, int(plan.parts.size()) - 1))];
        double minW = profileAt(part.difficulty).minWindowMs;
        if (w < minW * 1.3 || w < 70) r.risky.push_back({ck.time, c.x, w, c.pattern});
        if (ck.strength >= 0.6f) ++strong;
    }
    if (nW == 0) r.minWindowMs = 0;
    r.meanWindowMs = nW ? sumW / nW : 0;
    r.musicAlignment = nW ? float(strong) / float(nW) : 0;
    std::sort(r.risky.begin(), r.risky.end(), [](const Report::Risky& a, const Report::Risky& b) { return a.windowMs < b.windowMs; });
    // longest gap without input during gameplay (from the first click to the last part end)
    double prev = plan.parts.empty() ? 0 : plan.parts.front().t0;
    for (auto& e : inputs.events) {
        if (!e.press) continue;
        double t = e.tick / double(phys::kTPS);
        if (t - prev > r.longestGap) { r.longestGap = t - prev; r.longestGapAt = prev; }
        prev = t;
    }
    // difficulty curve: clicks per second weighted by tightness, per 2 s
    for (double t = 0; t < plan.duration; t += 2.0) {
        double v = 0;
        for (auto& c : clicks) {
            if (c.dropped) continue;
            double ct = plan.clicks[size_t(c.click)].time;
            if (ct < t || ct >= t + 2.0) continue;
            v += 120.0 / std::max(30.0, c.windowMs());
        }
        r.difficultyCurve.push_back({t, float(v)});
        r.intensityCurve.push_back({t, A.intensityAt(t + 1.0)});
    }
    float dmax = 0;
    for (auto& d : r.difficultyCurve) dmax = std::max(dmax, d.second);
    if (dmax > 0) for (auto& d : r.difficultyCurve) d.second /= dmax;
    {
        // Pearson correlation between difficulty and music intensity
        double n = double(r.difficultyCurve.size()), sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        for (size_t i = 0; i < r.difficultyCurve.size(); ++i) {
            double x = r.difficultyCurve[i].second, y = r.intensityCurve[i].second;
            sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
        }
        double den = std::sqrt(std::max(1e-12, (n * sxx - sx * sx) * (n * syy - sy * sy)));
        r.curveCorrelation = n > 2 ? float((n * sxy - sx * sy) / den) : 0;
    }
    for (auto& p : plan.parts) r.modeSeconds[size_t(int(p.mode))] += p.t1 - p.t0;
    for (auto& p : parts) {
        if (p.fallback) {
            ++r.fallbackParts;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "part %d (%.1f-%.1f s) used the safe fallback", p.part, plan.parts[size_t(p.part)].t0, plan.parts[size_t(p.part)].t1);
            r.notes.push_back(buf);
        }
    }
    if (r.dropped > 0) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "%d of %d clicks could not be built fairly and were left out", r.dropped, r.clicks);
        r.notes.push_back(buf);
    }
    return r;
}

Json Report::toJson() const {
    Json j = Json::object();
    j["beatable"] = beatable;
    j["clicks"] = clicks;
    j["dropped"] = dropped;
    j["forced"] = forced;
    j["minWindowMs"] = minWindowMs;
    j["meanWindowMs"] = meanWindowMs;
    j["longestGap"] = longestGap;
    j["longestGapAt"] = longestGapAt;
    j["musicAlignment"] = musicAlignment;
    j["curveCorrelation"] = curveCorrelation;
    j["objects"] = objects;
    j["gameplayObjects"] = gameplayObjects;
    j["portals"] = portals;
    j["triggers"] = triggers;
    j["fallbackParts"] = fallbackParts;
    Json risky = Json::array();
    for (auto& rk : this->risky) {
        Json e;
        e["time"] = rk.time;
        e["x"] = rk.x;
        e["windowMs"] = rk.windowMs;
        e["pattern"] = rk.pattern;
        risky.push(e);
    }
    j["risky"] = risky;
    Json curve = Json::array();
    for (size_t i = 0; i < difficultyCurve.size(); ++i) {
        Json e;
        e["t"] = difficultyCurve[i].first;
        e["difficulty"] = difficultyCurve[i].second;
        e["intensity"] = intensityCurve[i].second;
        curve.push(e);
    }
    j["curve"] = curve;
    Json modes = Json::object();
    for (int m = 0; m < 8; ++m) if (modeSeconds[size_t(m)] > 0) modes[level::modeName(level::Mode(m))] = modeSeconds[size_t(m)];
    j["modeSeconds"] = modes;
    Json n = Json::array();
    for (auto& s : notes) n.push(s);
    j["notes"] = n;
    return j;
}

std::string Report::toText() const {
    char buf[512];
    std::string s;
    std::snprintf(buf, sizeof(buf), "%s | %d clicks (%d forced, %d dropped) | windows: min %.0f ms, mean %.0f ms\n",
                  beatable ? "BEATABLE (bot verified)" : "NOT BEATABLE", clicks, forced, dropped, minWindowMs, meanWindowMs);
    s += buf;
    std::snprintf(buf, sizeof(buf), "longest gap without input: %.2f s at %.1f s | music alignment %.0f%% | difficulty follows music: r = %.2f\n",
                  longestGap, longestGapAt, musicAlignment * 100.0f, curveCorrelation);
    s += buf;
    std::snprintf(buf, sizeof(buf), "%d objects (%d gameplay, %d portals, %d triggers)\n", objects, gameplayObjects, portals, triggers);
    s += buf;
    if (!risky.empty()) {
        s += "risky spots:\n";
        for (size_t i = 0; i < std::min<size_t>(risky.size(), 8); ++i) {
            std::snprintf(buf, sizeof(buf), "  %6.2f s  x=%6.0f  window %4.0f ms  (%s)\n", risky[i].time, risky[i].x, risky[i].windowMs, risky[i].pattern.c_str());
            s += buf;
        }
    }
    for (auto& n : notes) { s += "note: "; s += n; s += '\n'; }
    return s;
}

float feelScore(const GenResult& r, const audio::MusicAnalysis& A) {
    (void)A;
    const auto& rep = r.report;
    if (!rep.beatable) return -1000;
    float s = 0;
    s += 3.0f * rep.musicAlignment;
    s += 2.0f * std::max(0.0f, rep.curveCorrelation);
    s -= 4.0f * float(rep.dropped) / std::max(1, rep.clicks);
    s -= 2.0f * float(rep.fallbackParts);
    s += 1.0f * float(rep.forced) / std::max(1, rep.clicks);
    // variety: distinct patterns per click
    std::vector<std::string> ids;
    for (auto& c : r.clicks) if (!c.dropped) ids.push_back(c.pattern);
    std::sort(ids.begin(), ids.end());
    size_t distinct = size_t(std::unique(ids.begin(), ids.end()) - ids.begin());
    s += 1.5f * std::min(1.0f, float(distinct) / std::max(1.0f, 0.35f * float(ids.size())));
    // fairness: penalise very tight spots relative to the difficulty
    s -= 0.15f * float(rep.risky.size());
    // repetition: identical pattern 3+ times in a row
    int run = 1;
    for (size_t i = 1; i < r.clicks.size(); ++i) {
        if (r.clicks[i].pattern == r.clicks[i - 1].pattern && !r.clicks[i].dropped) { if (++run >= 3) s -= 0.2f; }
        else run = 1;
    }
    return s;
}

// ---------------------------------------------------------------- generation

GenResult generateVariant(const audio::MusicAnalysis& A, const GenOptions& opt, uint64_t seed, const StyleModel* model,
                          const Progress* progress, const DesignPlan* fixedPlan) {
    auto t0 = std::chrono::steady_clock::now();
    GenResult res;
    res.seed = seed;
    Rng rng(combineSeed(seed, hashStr("autolayout")));
    if (fixedPlan) res.plan = *fixedPlan;
    else {
        if (progress) progress->report("plan", 0);
        Rng pr = rng.fork(hashStr("plan"));
        res.plan = planFromMusic(A, opt, pr);
    }
    Builder b(A, opt, res.plan, model, rng.fork(hashStr("build")));
    if (!b.build(progress)) throw Cancelled{};
    res.level = b.level();
    res.inputs = b.inputs();
    res.clicks = b.clickResults();
    res.parts = b.partResults();
    res.model = model ? model->name() : "rules";
    // decoration (never touches gameplay hitboxes), then the final bot verification
    if (progress) progress->report("decorate", 0);
    Rng dr = rng.fork(hashStr("decorate"));
    decorate(res.level, res.inputs, res.plan, A, opt, dr, opt.groupBase > 0 ? opt.groupBase : 1000);
    if (progress) progress->report("verify", 0);
    phys::World W(res.level);
    bool beatable = bot::isBeatable(W, res.inputs);
    bool intended = beatable;
    if (!beatable) {
        auto f = bot::firstFailure(W, res.inputs);
        phys::World W0(b.level());
        auto f0 = bot::firstFailure(W0, res.inputs);
        debugLog("final verification failed: tick %d x=%.0f y=%.0f cause=%s obj=%d | before decoration: failed=%d tick %d x=%.0f",
                 f.tick, f.x, f.y, phys::deathCauseName(f.cause), f.levelObject, f0.failed, f0.tick, f0.x);
        if (!f0.failed) {
            // the decoration broke it (should not happen): ship the undecorated layout instead
            res.level = b.level();
            res.notes.push_back("decoration removed: it interfered with the gameplay");
            beatable = intended = true;
        }
    }
    bot::Failure fail;
    if (!beatable) {
        // Should not happen (every part was verified); search for any solution before giving up.
        phys::World W1(res.level);
        fail = bot::firstFailure(W1, res.inputs);
        bot::SolveOptions so;
        so.progress = progress;
        auto solved = bot::autoSolve(W1, so);
        if (solved.solved) { res.inputs = solved.inputs; beatable = true; }
    }
    res.report = makeReport(res.level, res.inputs, res.plan, res.clicks, res.parts, A, beatable);
    res.report.intendedInputs = intended;
    for (auto& n : res.notes) res.report.notes.push_back(n);
    if (!beatable) {
        res.report.failed = true;
        res.report.failTime = phys::kStepSec * fail.tick;
        res.report.failX = fail.x;
        res.report.failY = fail.y;
        res.report.failCause = phys::deathCauseName(fail.cause);
        res.report.notes.push_back("NOT BEATABLE: dies at " + fmtNum(std::round(res.report.failTime * 100) / 100) + " s (x " + fmtNum(std::round(fail.x)) + ", " + res.report.failCause + ")");
    }
    res.score = feelScore(res, A);
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

std::vector<GenResult> generateVariants(const audio::MusicAnalysis& A, const GenOptions& opt, const StyleModel* model,
                                        const Progress* progress, int threads, const DesignPlan* plan) {
    const int n = std::max(1, opt.variants);
    std::vector<GenResult> out(static_cast<size_t>(n));
    std::vector<char> ok(static_cast<size_t>(n), 0);
    if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
    threads = std::min(threads, n);
    std::atomic<int> next{0}, done{0};
    std::mutex progMutex;
    Progress sub;
    if (progress) {
        sub.cancelFlag = progress->cancelFlag;
        sub.onProgress = [&](const std::string& stage, float f) {
            std::lock_guard<std::mutex> lk(progMutex);
            progress->report(stage, (float(done.load()) + std::clamp(f, 0.0f, 1.0f)) / float(n));
        };
    }
    auto worker = [&]() {
        for (;;) {
            int i = next.fetch_add(1);
            if (i >= n) return;
            try {
                uint64_t seed = combineSeed(opt.seed, uint64_t(i));
                out[size_t(i)] = generateVariant(A, opt, seed, model, progress ? &sub : nullptr, plan);
                ok[size_t(i)] = 1;
            } catch (const Cancelled&) {
                return;
            }
            done.fetch_add(1);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    if (progress && progress->cancelled()) throw Cancelled{};
    std::vector<GenResult> results;
    for (int i = 0; i < n; ++i) if (ok[size_t(i)]) results.push_back(std::move(out[size_t(i)]));
    std::stable_sort(results.begin(), results.end(), [](const GenResult& a, const GenResult& b) { return a.score > b.score; });
    return results;
}

GenResult rerollPart(const GenResult& base, int partIndex, const audio::MusicAnalysis& A, const GenOptions& opt, uint64_t seed,
                     const StyleModel* model) {
    DesignPlan plan = base.plan;
    Rng rng(combineSeed(seed, uint64_t(partIndex) * 0x9E37ull));
    replanPart(plan, partIndex, A, opt, rng);
    // The parts before the re-rolled one are rebuilt deterministically from the same seed so they stay identical.
    return generateVariant(A, opt, base.seed, model, nullptr, &plan);
}

} // namespace al::gen
