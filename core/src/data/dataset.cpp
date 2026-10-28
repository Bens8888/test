#include <al/data/dataset.hpp>

#include <al/util/codec.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "../gen/pattern_params.hpp"

namespace al::data {

using gen::kContextFeatures;
using level::Kind;
using level::Mode;

// ---------------------------------------------------------------- JSON

Json Sequence::toJson() const {
    Json j = Json::object();
    j["source"] = source;
    j["style"] = style;
    j["difficulty"] = difficulty;
    j["density"] = density;
    j["reward"] = reward;
    Json steps_ = Json::array();
    for (const auto& s : steps) {
        Json o = Json::object();
        Json f = Json::array();
        for (float v : s.f) f.push(double(std::round(v * 10000.0f) / 10000.0f));
        o["f"] = std::move(f);
        o["t"] = s.token;
        o["time"] = std::round(s.time * 1000.0) / 1000.0;
        steps_.push(std::move(o));
    }
    j["steps"] = std::move(steps_);
    return j;
}

Sequence Sequence::fromJson(const Json& j) {
    Sequence s;
    s.source = j.get("source", "");
    s.style = j.get("style", 0);
    s.difficulty = float(j.get("difficulty", 4.0));
    s.density = float(j.get("density", -1.0));
    s.reward = float(j.get("reward", 0.0));
    for (const Json& o : j["steps"].items()) {
        Step st;
        const Json& f = o["f"];
        for (int i = 0; i < kContextFeatures; ++i) st.f[size_t(i)] = float(f[size_t(i)].asNumber());
        st.token = o.get("t", -1);
        st.time = o.get("time", 0.0);
        s.steps.push_back(st);
    }
    return s;
}

bool appendJsonl(const std::string& path, const std::vector<Sequence>& seqs) {
    FILE* fp = std::fopen(path.c_str(), "ab");
    if (!fp) return false;
    for (const auto& s : seqs) {
        std::string line = s.toJson().dump();
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), fp);
    }
    std::fclose(fp);
    return true;
}

std::vector<Sequence> readJsonl(const std::string& path) {
    std::vector<Sequence> out;
    std::string text;
    if (!readFile(path, text)) return out;
    for (auto line : split(text, '\n')) {
        line = trim(line);
        if (line.empty()) continue;
        if (auto j = Json::parse(line)) out.push_back(Sequence::fromJson(*j));
    }
    return out;
}

// ---------------------------------------------------------------- self-play

float reward(const gen::GenResult& r) {
    const auto& rep = r.report;
    if (!rep.beatable) return -1e9f;
    const float n = float(std::max(1, rep.clicks));
    float x = 0;
    x += 1.0f * rep.musicAlignment;
    x += 0.8f * float(rep.forced) / n;
    x -= 1.5f * float(rep.dropped) / n;
    x += 0.5f * rep.curveCorrelation;
    x -= 0.15f * float(std::max(0.0, rep.longestGap - 2.0));
    x -= 0.05f * float(rep.risky.size());
    x += 0.1f * r.score;
    if (!rep.intendedInputs) x -= 1.0f;
    if (rep.fallbackParts > 0) x -= 0.3f * float(rep.fallbackParts);
    return x;
}

Sequence fromGenerated(const gen::GenResult& r, const gen::GenOptions& opt) {
    Sequence s;
    s.source = "selfplay";
    s.style = int(r.plan.style);
    s.difficulty = r.plan.baseDifficulty;
    s.density = opt.density;
    s.reward = reward(r);
    for (const auto& c : r.clicks) {
        if (!c.decision || c.dropped) continue;
        Step st;
        st.f = c.features;
        st.token = gen::patternToken(c.label.empty() ? c.pattern : c.label);
        st.time = c.click >= 0 && size_t(c.click) < r.plan.clicks.size() ? r.plan.clicks[size_t(c.click)].time : 0;
        if (st.token >= 0) s.steps.push_back(st);
    }
    std::stable_sort(s.steps.begin(), s.steps.end(), [](const Step& a, const Step& b) { return a.time < b.time; });
    return s;
}

// ---------------------------------------------------------------- real levels

std::string ExtractReport::summary() const {
    char buf[256];
    if (!ok) std::snprintf(buf, sizeof(buf), "rejected: %s", reason.c_str());
    else std::snprintf(buf, sizeof(buf), "%d presses, %d decisions labelled, %d unlabelled, %.1f s", presses, labelled, unlabelled, seconds);
    return buf;
}

namespace {

const phys::PlayerState& stateAt(const std::vector<phys::PlayerState>& trace, int tick) {
    // trace[i] is the state after step i + 1
    return trace[size_t(std::clamp(tick - 1, 0, int(trace.size()) - 1))];
}

float feetOf(const phys::PlayerState& p) { return p.y - float(p.flip()) * p.height() * 0.5f; }

// gravity-relative height of y above the surface s
float above(float y, float surf, int g) { return (y - surf) * float(g); }

} // namespace

int classifyGroundJump(const phys::World& W, const std::vector<phys::PlayerState>& trace, const std::vector<uint32_t>& events,
                       int pressTick, int endTick, int* orbObjectId) {
    using namespace gen::pp;
    if (trace.empty()) return -1;
    const auto& p0 = stateAt(trace, pressTick);
    const int g = p0.flip();
    // the surface the player last stood on (an orb press happens in the air)
    float surf0 = feetOf(p0);
    for (int t = pressTick; t > std::max(1, pressTick - 400); --t) {
        const auto& q = stateAt(trace, t);
        if (q.onGround && q.flip() == g) { surf0 = feetOf(q); break; }
    }
    const char* prefix = p0.mode == Mode::Robot ? "robot" : "cube";
    // orb used by this press?
    bool orb = false;
    int orbKind = 0;
    for (int t = pressTick; t <= std::min(endTick, pressTick + 6); ++t) {
        if (size_t(t - 1) < events.size() && (events[size_t(t - 1)] & phys::EvOrb)) {
            orb = true;
            const auto& ps = stateAt(trace, t);
            std::vector<int> near;
            W.query(ps.x - 40, ps.x + 40, near);
            float best = 1e9f;
            for (int si : near) {
                const auto& so = W.objects()[size_t(si)];
                if (so.kind != Kind::Orb) continue;
                float d = std::hypot(so.x - ps.x, so.y - ps.y);
                if (d < best) { best = d; orbKind = so.sub; if (orbObjectId) *orbObjectId = so.levelIndex; }
            }
            break;
        }
    }
    // the landing that ends this jump
    int land = -1;
    bool left = false;
    for (int t = pressTick + 1; t <= endTick; ++t) {
        const auto& s = stateAt(trace, t);
        if (!s.onGround) left = true;
        if (left && size_t(t - 1) < events.size() && (events[size_t(t - 1)] & phys::EvLand)) { land = t; break; }
    }
    if (land < 0) return -1;
    const auto& p1 = stateAt(trace, land);
    if (p1.mode != p0.mode) return -1;
    const float x0 = p0.x, x1 = p1.x;
    const float dy = above(feetOf(p1), surf0, g);
    int terrain = TrFlat;
    if (dy >= 45) terrain = TrUp2;
    else if (dy >= 15) terrain = TrUp1;
    else if (dy <= -45) terrain = TrDown2;
    else if (dy <= -15) terrain = TrDown1;
    // hazards and obstacles under the arc
    std::vector<int> near;
    W.query(x0, x1, near);
    int spikes = 0, halves = 0, saws = 0, blocks = 0;
    for (int si : near) {
        const auto& so = W.objects()[size_t(si)];
        if (so.x < x0 + 5 || so.x > x1 - 5) continue;
        float base = above(so.y - float(g) * so.hh, surf0, g); // gravity-relative bottom
        if (so.kind == Kind::Hazard || so.kind == Kind::HazardSlope) {
            float baseLand = base - dy; // relative to the landing surface (descents put spikes down there)
            if ((base < -8 || base > 40) && (dy > -15 || baseLand < -8 || baseLand > 40)) continue;
            (so.hh * 2 < 9 ? halves : spikes)++; // spike hitbox 6x12, half spike 6x5.6
        } else if (so.kind == Kind::Saw) {
            float c = above(so.y, surf0, g);
            if (c > -20 && c < 40) ++saws;
        } else if (so.kind == Kind::Solid) {
            float top = above(so.y + float(g) * so.hh, surf0, g);
            if (terrain == TrFlat && top > 5 && top <= 35 && std::fabs(so.x - x1) > 20) ++blocks; // (step walls are terrain)
        }
    }
    // gap: cells under the arc without support at the take-off surface (and no floor there)
    int gapCells = 0, run = 0;
    for (float x = x0 + 15; x <= x1 - 15; x += 30) {
        bool support = g > 0 && surf0 < 1.0f; // the level floor
        std::vector<int> col;
        W.query(x - 2, x + 2, col);
        for (int si : col) {
            const auto& so = W.objects()[size_t(si)];
            if (so.kind != Kind::Solid) continue;
            float top = above(so.y + float(g) * so.hh, surf0, g);
            if (std::fabs(top) < 6 && so.minX() <= x && so.maxX() >= x) support = true;
        }
        run = support ? 0 : run + 1;
        gapCells = std::max(gapCells, run);
    }
    if (gapCells >= 2 && (terrain == TrFlat || terrain == TrUp1)) terrain = gapCells >= 4 ? TrGap4 : (gapCells == 3 ? TrGap3 : TrGap2);
    // map to a library id
    static const char* kTr[] = {"flat", "up1", "up2", "down1", "down2", "gap2", "gap3", "gap4"};
    std::string id;
    if (orb) {
        // an orb jumps a pit: missing ground or a row of spikes (the generator's floor pits)
        int pit = std::max(gapCells, spikes + halves);
        int tr = terrain == TrUp1 ? TrUp1 : (pit >= 4 ? TrGap4 : pit == 3 ? TrGap3 : pit == 2 ? TrGap2 : TrFlat);
        id = std::string(prefix) + ":" + (orbKind == int(level::OrbType::Pink) ? "orb-pink" : "orb-yellow") + "+" + kTr[tr];
    } else {
        const char* ob = "none";
        if (saws > 0) ob = "saw";
        else if (blocks > 0 && spikes + halves == 0) ob = "block1";
        else if (spikes > 0 && halves > 0) ob = "mix2";
        else if (spikes >= 3) ob = "spike3";
        else if (spikes == 2) ob = "spike2";
        else if (spikes == 1) ob = "spike1";
        else if (halves >= 2) ob = "half2";
        else if (halves == 1) ob = "half1";
        id = std::string(prefix) + ":" + ob + "+" + kTr[terrain];
    }
    int tok = gen::patternToken(id);
    if (tok < 0 && !orb) {
        // nearest existing combination: keep the obstacle, try flat terrain, then a single spike
        auto plus = id.find('+');
        tok = gen::patternToken(id.substr(0, plus) + "+flat");
        if (tok < 0) tok = gen::patternToken(std::string(prefix) + ":spike1+" + kTr[terrain]);
    }
    return tok;
}

namespace {

// Family token of a flying or surface region [t0, t1).
int classifyRegion(const phys::World& W, const std::vector<phys::PlayerState>& trace, int t0, int t1, Mode mode) {
    using namespace gen::pp;
    int solids = 0, hazards = 0, saws = 0, halfBlocks = 0, samples = 0;
    std::vector<float> clear;
    float xA = stateAt(trace, t0).x, xB = stateAt(trace, t1).x;
    std::vector<int> near;
    W.query(xA, xB, near);
    for (int si : near) {
        const auto& so = W.objects()[size_t(si)];
        if (so.x < xA || so.x > xB) continue;
        if (so.kind == Kind::Solid) { ++solids; if (so.hw < 10) ++halfBlocks; }
        else if (so.kind == Kind::Hazard || so.kind == Kind::HazardSlope) ++hazards;
        else if (so.kind == Kind::Saw) ++saws;
    }
    for (int t = t0; t < t1; t += 6) {
        const auto& p = stateAt(trace, t);
        std::vector<int> col;
        W.query(p.x - 15, p.x + 15, col);
        float up = 300, down = 300;
        for (int si : col) {
            const auto& so = W.objects()[size_t(si)];
            if (so.kind != Kind::Solid && so.kind != Kind::Hazard && so.kind != Kind::Saw) continue;
            if (so.minY() >= p.y) up = std::min(up, so.minY() - p.y);
            else if (so.maxY() <= p.y) down = std::min(down, p.y - so.maxY());
        }
        if (p.bounded) { up = std::min(up, p.ceilY - p.y); down = std::min(down, p.y - p.floorY); }
        clear.push_back(up + down);
        ++samples;
    }
    float len = std::max(30.0f, xB - xA);
    float perCol = float(solids + hazards + saws) / (len / 30.0f);
    bool dense = perCol > 2.5f;
    std::sort(clear.begin(), clear.end());
    float med = clear.empty() ? 300 : clear[clear.size() / 2];
    const char* prefix = level::modeName(mode);
    std::string id;
    std::string lower(prefix);
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (mode == Mode::Ball || mode == Mode::Spider) {
        const char* fam = hazards > halfBlocks * 2 ? "spikes" : (halfBlocks > 0 ? "blocks" : "gaps");
        id = lower + ":" + fam + (dense ? "-dense" : "-sparse");
    } else {
        const char* fam;
        if (saws >= 3) fam = "saw";
        else if (hazards > solids / 3 && hazards >= 4) fam = "spiked";
        else if (med < 75) fam = "tight";
        else if (solids < 4) fam = "open";
        else fam = mode == Mode::Wave ? "tight" : "stairs";
        id = lower + ":" + fam + (dense ? "-dense" : "-sparse");
    }
    (void)samples;
    return gen::patternToken(id);
}

} // namespace

Sequence fromLevel(const level::Level& lvl, const audio::MusicAnalysis& A, const ExtractOptions& opt, ExtractReport& rep, const bot::InputTrack* solution) {
    auto t0c = std::chrono::steady_clock::now();
    Sequence seq;
    seq.source = "level:" + lvl.name;
    seq.style = opt.style;
    rep = ExtractReport{};
    auto reject = [&](const std::string& why) { rep.ok = false; rep.reason = why; return seq; };
    // ---- filters
    if (lvl.settings.platformer) return reject("platformer level");
    if (lvl.settings.twoPlayer) return reject("2-player level");
    int gameplay = 0;
    for (auto& o : lvl.objects) {
        auto k = o.info().kind;
        if (k != Kind::Deco && k != Kind::Trigger) ++gameplay;
    }
    if (gameplay < 30) return reject("too few gameplay objects (" + std::to_string(gameplay) + ")");
    if (A.duration < 15) return reject("song shorter than 15 s or missing");
    phys::World W(lvl);
    bot::InputTrack sol;
    if (solution) sol = *solution;
    else {
        bot::SolveOptions so;
        so.beamWidth = opt.solveBeam;
        so.maxTicks = opt.maxSeconds * 240;
        auto res = bot::autoSolve(W, so);
        if (!res.solved) return reject("the bot could not beat the level (stuck at x " + std::to_string(int(res.bestX)) + ")");
        sol = res.inputs;
    }
    bot::RunOptions ro;
    ro.recordTrace = true;
    ro.maxTicks = opt.maxSeconds * 240;
    auto run = bot::run(W, sol, ro);
    if (!run.won) return reject("the solution does not finish the level");
    const double levelSeconds = double(run.ticks) / phys::kTPS;
    if (levelSeconds < 15) return reject("level shorter than 15 s");
    if (levelSeconds > A.duration + 10) return reject("level is much longer than the song (wrong song?)");
    std::vector<int> presses;
    for (auto& e : sol.events) if (e.press) presses.push_back(e.tick);
    rep.presses = int(presses.size());
    if (presses.size() < 10) return reject("fewer than 10 clicks");
    // ---- difficulty label
    float diff = opt.difficulty;
    if (diff < 0) {
        double rate = double(presses.size()) / levelSeconds;
        diff = 0;
        for (int d = 0; d <= 9; ++d) {
            const auto& pr = gen::profile(gen::Difficulty(d));
            if (rate >= 0.5 * (pr.clicksCalm + pr.clicksIntense) * 0.85) diff = float(d);
        }
    }
    seq.difficulty = diff;
    // ---- decisions
    auto cands = gen::gatherCandidates(A, 0, A.duration + 1, 0.0f);
    auto releaseAfter = [&](int tick) {
        for (auto& e : sol.events) if (!e.press && e.tick > tick) return e.tick;
        return tick + 5;
    };
    const double off = lvl.settings.songOffset;
    double lastTime = -1;
    float lastPitch = 0;
    std::string prev1, prev2;
    for (size_t i = 0; i < presses.size(); ++i) {
        const int k = presses[i];
        const auto& ps = stateAt(run.trace, k);
        const double t = off + double(k) / phys::kTPS;
        const int next = i + 1 < presses.size() ? presses[i + 1] : k + 360;
        int tok = -1;
        bool groundMode = ps.mode == Mode::Cube || ps.mode == Mode::Robot;
        if (groundMode) {
            tok = classifyGroundJump(W, run.trace, run.traceEvents, k, std::min(next + 120, run.ticks), nullptr);
        } else {
            // one decision per region of this gamemode
            bool first = i == 0 || stateAt(run.trace, presses[i - 1]).mode != ps.mode;
            if (!first) { lastTime = t; continue; }
            int end = k;
            while (end < run.ticks && stateAt(run.trace, end).mode == ps.mode) end += 4;
            tok = classifyRegion(W, run.trace, k, std::min(end, run.ticks), ps.mode);
        }
        if (tok < 0) { ++rep.unlabelled; lastTime = t; continue; }
        gen::PatternContext ctx;
        ctx.mode = ps.mode;
        ctx.intensity = A.intensityAt(t);
        ctx.difficulty = diff;
        int si = A.sectionAt(t);
        ctx.intent = si >= 0 ? gen::intentFor(A.sections[size_t(si)].type) : gen::Intent::Groove;
        ctx.speed = ps.speed;
        ctx.mini = ps.mini;
        ctx.style = gen::Style(opt.style);
        ctx.gapPrev = lastTime >= 0 ? t - lastTime : 2.0;
        ctx.gapNext = i + 1 < presses.size() ? double(presses[i + 1] - k) / phys::kTPS : 2.0;
        auto it = std::lower_bound(cands.begin(), cands.end(), t, [](const gen::Candidate& c, double v) { return c.t < v; });
        ctx.role = gen::Role::Recorded;
        ctx.strength = 0.3f;
        for (auto j : {it - 1, it}) {
            if (j < cands.begin() || j >= cands.end()) continue;
            if (std::fabs(j->t - t) <= 0.07) { ctx.role = j->role; ctx.strength = std::min(1.0f, j->sal); }
        }
        float pitch = A.pitchAt(t);
        ctx.pitchDelta = (pitch > 0 && lastPitch > 0) ? pitch - lastPitch : 0;
        ctx.hold = double(releaseAfter(k) - k) / phys::kTPS >= 0.15;
        ctx.prevPattern = prev1;
        ctx.prevPattern2 = prev2;
        Step st;
        gen::contextFeatures(ctx, st.f.data());
        st.token = tok;
        st.time = t;
        seq.steps.push_back(st);
        ++rep.labelled;
        prev2 = prev1;
        prev1 = gen::patternLibrary()[size_t(tok)].id;
        lastTime = t;
        if (pitch > 0) lastPitch = pitch;
    }
    if (rep.labelled < 8) return reject("fewer than 8 decisions could be labelled");
    rep.ok = true;
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0c).count();
    return seq;
}

} // namespace al::data
