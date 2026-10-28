// Generator v2 end to end: clicks -> gameplay path -> structures -> verification -> report.
#include <al/gen/generator.hpp>
#include <al/gen/path.hpp>
#include <al/gen/profile.hpp>
#include <al/gen/structures.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace al::gen {

namespace {

const Part* musicPartAt(const DesignPlan& plan, double t) {
    for (const auto& p : plan.music) if (t >= p.t0 && t < p.t1) return &p;
    return plan.music.empty() ? nullptr : &plan.music.back();
}

std::string patternName(const PathClick& c) {
    std::string s = std::string(level::modeName(c.mode)) + ":" + actName(c.act);
    if (c.act == Act::Orb) {
        switch (c.objId) {
            case level::id::OrbYellow: s += "-yellow"; break;
            case level::id::OrbPink: s += "-pink"; break;
            case level::id::OrbRed: s += "-red"; break;
            case level::id::OrbBlue: s += "-blue"; break;
            case level::id::OrbGreen: s += "-green"; break;
            case level::id::OrbBlack: s += "-black"; break;
            case level::id::OrbDash: s += "-dash"; break;
            default: break;
        }
    }
    return s;
}

} // namespace

GenResult generateVariantV2(const audio::MusicAnalysis& A, const GenOptions& opt, uint64_t seed, const Progress* progress, const DesignPlan* fixedPlan,
                            const std::vector<uint64_t>* salts, int lockSegment, const std::vector<uint64_t>* baseSalts) {
    auto t0 = std::chrono::steady_clock::now();
    GenResult res;
    res.seed = seed;
    res.model = "path v2";
    Rng rng(combineSeed(seed, hashStr("autolayout-v2")));
    // ---- clicks (the plan's parts are the music's parts at this point)
    DesignPlan plan;
    if (fixedPlan) {
        plan = *fixedPlan;
        if (!plan.music.empty()) plan.parts = plan.music; // a v2 result being regenerated
    } else {
        if (progress) progress->report("plan", 0);
        Rng pr = rng.fork(hashStr("plan"));
        plan = planFromMusicV2(A, opt, pr);
    }
    plan.music = plan.parts;
    // ---- gameplay path
    if (progress) progress->report("build", 0);
    PathParams prm;
    prm.switchRate = opt.switchRate >= 0 ? opt.switchRate : (opt.profile ? opt.profile->switchRate() : -1.0f);
    if (salts) prm.salts = *salts;
    if (baseSalts) prm.baseSalts = *baseSalts;
    prm.lockSegment = lockSegment;
    Rng pathRng = rng.fork(hashStr("path"));
    PathPlan path = planPath(plan, A, opt, pathRng, prm, progress);
    checkCancel(progress);
    // ---- structures, verified; lighter versions if the full one ever fails
    if (progress) progress->report("verify", 0);
    StructureParams sp;
    sp.look = opt.look == 1 ? TileLook::Blocks : TileLook::Outline;
    StructureResult st;
    const float learned = opt.profile ? opt.profile->density() : -1.0f;
    const float densities[3] = {opt.density >= 0 ? opt.density : learned, 0.0f, 0.0f};
    for (int attempt = 0; attempt < 3; ++attempt) {
        sp.density = densities[attempt];
        sp.necessity = attempt < 2;
        Rng stRng = rng.fork(hashStr("structures"));
        st = buildStructures(path, plan, opt, stRng, sp);
        if (st.beatable) break;
        res.notes.push_back(attempt == 0 ? "structures simplified: the full version failed the final check"
                                         : "necessity hazards removed: the simplified version failed the final check");
    }
    res.level = st.level;
    res.level.settings.songOffset = plan.songStart;
    res.level.name = "AutoLayout";
    res.inputs = path.inputs;
    // ---- the plan as the UI shows it: one part per gamemode segment
    DesignPlan out = plan;
    out.parts.clear();
    for (size_t s = 0; s < path.segments.size(); ++s) {
        const PathSegment& sg = path.segments[s];
        Part p;
        p.index = int(s);
        const double first = sg.first < int(plan.clicks.size()) ? plan.clicks[size_t(sg.first)].time : plan.duration;
        const double prevLast = s > 0 && path.segments[s - 1].last < int(plan.clicks.size()) ? plan.clicks[size_t(path.segments[s - 1].last)].time : 0.0;
        p.t0 = s == 0 ? 0.0 : 0.5 * (prevLast + first);
        p.mode = sg.mode;
        p.speed = sg.speed;
        p.mini = sg.mini;
        p.flipped = sg.flipped;
        p.intensity = sg.intensity;
        if (const Part* mp = musicPartAt(plan, first)) {
            p.intent = mp->intent;
            p.difficulty = mp->difficulty;
            p.section = mp->section;
        }
        p.salt = salts && s < salts->size() ? (*salts)[s] : 0;
        p.why = sg.why;
        for (int c = sg.first; c <= sg.last && c < int(plan.clicks.size()); ++c) {
            p.clicks.push_back(c);
            out.clicks[size_t(c)].part = int(s);
        }
        out.parts.push_back(p);
    }
    for (size_t s = 0; s < out.parts.size(); ++s) out.parts[s].t1 = s + 1 < out.parts.size() ? out.parts[s + 1].t0 : plan.duration;
    res.plan = out;
    // ---- click results
    std::vector<int> used(plan.clicks.size(), -1);
    for (size_t i = 0; i < path.clicks.size(); ++i) used[size_t(path.clicks[i].click)] = int(i);
    for (size_t k = 0; k < plan.clicks.size(); ++k) {
        ClickResult r;
        r.click = int(k);
        if (used[k] < 0) {
            r.dropped = true;
            r.pattern = "left out";
        } else {
            const PathClick& pc = path.clicks[size_t(used[k])];
            r.pattern = patternName(pc);
            r.x = pc.x;
            if (size_t(used[k]) < st.checks.size()) {
                r.forced = st.checks[size_t(used[k])].forced;
                r.windowEarly = st.checks[size_t(used[k])].early;
                r.windowLate = st.checks[size_t(used[k])].late;
            }
        }
        res.clicks.push_back(r);
    }
    for (size_t s = 0; s < out.parts.size(); ++s) res.parts.push_back({int(s), true, false, 1, ""});
    // ---- final verification (the designed inputs), then a bot search as the last resort
    phys::World W(res.level);
    bool beatable = bot::isBeatable(W, res.inputs);
    const bool intended = beatable;
    bot::Failure fail;
    if (!beatable) {
        fail = bot::firstFailure(W, res.inputs);
        bot::SolveOptions so;
        so.progress = progress;
        auto solved = bot::autoSolve(W, so);
        if (solved.solved) { res.inputs = solved.inputs; beatable = true; }
    }
    res.report = makeReport(res.level, res.inputs, res.plan, res.clicks, res.parts, A, beatable);
    res.report.intendedInputs = intended;
    for (auto& n : path.notes) res.report.notes.push_back(n);
    for (auto& n : st.notes) res.report.notes.push_back(n);
    for (auto& n : res.notes) res.report.notes.push_back(n);
    if (!beatable) {
        res.report.failed = true;
        res.report.failTime = phys::kStepSec * fail.tick;
        res.report.failX = fail.x;
        res.report.failY = fail.y;
        res.report.failCause = phys::deathCauseName(fail.cause);
        res.report.notes.push_back("NOT BEATABLE: dies at " + std::to_string(res.report.failTime) + " s");
    }
    res.score = feelScore(res, A);
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

} // namespace al::gen
