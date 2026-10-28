#include <al/bot/bot.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace al::bot {

using phys::GameState;
using phys::World;

bool InputTrack::heldAt(int tick) const {
    // last event with event.tick <= tick
    auto it = std::upper_bound(events.begin(), events.end(), tick, [](int t, const InputEvent& e) { return t < e.tick; });
    if (it == events.begin()) return false;
    return (it - 1)->press;
}

void InputTrack::normalize() {
    std::stable_sort(events.begin(), events.end(), [](const InputEvent& a, const InputEvent& b) { return a.tick < b.tick; });
    std::vector<InputEvent> out;
    bool state = false;
    for (auto& e : events) {
        if (!out.empty() && out.back().tick == e.tick) {
            // same tick: the later event wins
            out.back().press = e.press;
            state = e.press;
            if (out.size() >= 2 && out[out.size() - 2].press == out.back().press) out.pop_back();
            else if (out.size() == 1 && !out.back().press) out.pop_back();
            continue;
        }
        if (e.press == state) continue;
        out.push_back(e);
        state = e.press;
    }
    events = std::move(out);
}

size_t InputTrack::pressCount() const {
    size_t n = 0;
    for (auto& e : events) n += e.press;
    return n;
}

RunResult runFrom(const World& W, GameState g, const InputTrack& in, const RunOptions& opt) {
    RunResult r;
    size_t ei = 0;
    // skip events before the current tick, establishing the held state
    bool held = g.pressed;
    while (ei < in.events.size() && in.events[ei].tick <= g.tick) { held = in.events[ei].press; ++ei; }
    const int startTick = g.tick;
    while (!g.dead && !g.won && g.tick - startTick < opt.maxTicks) {
        int next = g.tick + 1;
        while (ei < in.events.size() && in.events[ei].tick <= next) { held = in.events[ei].press; ++ei; }
        phys::step(W, g, held);
        if (opt.recordTrace && (g.tick % std::max(1, opt.traceEvery)) == 0) {
            r.trace.push_back(g.p[0]);
            r.traceEvents.push_back(g.p[0].events | (g.dual ? g.p[1].events : 0));
        }
    }
    r.won = g.won;
    r.dead = g.dead;
    r.ticks = g.tick;
    const auto& p = g.p[0].dead || !g.dual ? g.p[0] : g.p[1];
    r.x = g.p[0].x;
    r.y = g.p[0].y;
    if (g.dead) {
        const auto& dp = g.p[0].dead ? g.p[0] : g.p[1];
        r.deathTick = g.tick;
        r.cause = dp.deathCause;
        r.deathObject = dp.deathObj;
        if (dp.deathObj >= 0 && size_t(dp.deathObj) < W.objects().size()) r.deathLevelObject = W.objects()[size_t(dp.deathObj)].levelIndex;
        r.x = dp.x;
        r.y = dp.y;
    }
    (void)p;
    return r;
}

RunResult run(const World& W, const InputTrack& in, const RunOptions& opt) { return runFrom(W, phys::initialState(W), in, opt); }

bool isBeatable(const World& W, const InputTrack& in) { return run(W, in).won; }

Failure firstFailure(const World& W, const InputTrack& in) {
    auto r = run(W, in);
    Failure f;
    if (r.won) return f;
    f.failed = true;
    f.tick = r.dead ? r.deathTick : r.ticks;
    f.x = r.x;
    f.y = r.y;
    f.cause = r.cause;
    f.levelObject = r.deathLevelObject;
    return f;
}

// ---------------------------------------------------------------- beam search

namespace {

struct Node {
    GameState state;
    int parent;     // index into previous layer's history
    int toggles;
    bool held;
};

uint64_t mixKey(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t stateKey(const GameState& g, float yQuant, float vQuant) {
    uint64_t h = 0;
    int n = g.dual ? 2 : 1;
    for (int i = 0; i < n; ++i) {
        const auto& p = g.p[size_t(i)];
        h = mixKey(h, uint64_t(int64_t(std::lround(p.y / yQuant))));
        h = mixKey(h, uint64_t(int64_t(std::lround(p.v / vQuant))));
        h = mixKey(h, uint64_t(int64_t(std::lround(p.x * 4))));
        uint64_t flags = uint64_t(p.mode) | (uint64_t(p.speed) << 4) | (uint64_t(p.mini) << 8) | (uint64_t(p.upsideDown) << 9) |
                         (uint64_t(p.holding) << 10) | (uint64_t(p.canJump) << 11) | (uint64_t(p.isJumping) << 12) |
                         (uint64_t(p.jumpUnused) << 13) | (uint64_t(p.dashing) << 14) | (uint64_t(p.onSlope) << 15) |
                         (uint64_t(p.robotInvalid) << 16) | (uint64_t(p.uncapped) << 17);
        h = mixKey(h, flags);
        h = mixKey(h, uint64_t(std::lround(p.robotCharge * 100)));
        uint64_t act = 0;
        for (int k = 0; k < p.activatedCount; ++k) act = mixKey(act, uint64_t(uint32_t(p.activated[size_t(k)])));
        h = mixKey(h, act);
    }
    h = mixKey(h, uint64_t(g.dual));
    return h;
}

} // namespace

SolveResult autoSolveFrom(const World& W, const GameState& start, const SolveOptions& opt) {
    auto t0 = std::chrono::steady_clock::now();
    SolveResult res;
    const int K = std::max(1, opt.decisionTicks);
    const float goalX = opt.stopAtX >= 0 ? opt.stopAtX : W.endX();
    // history[layer][i] = (parentIndex << 1) | held
    std::vector<std::vector<uint32_t>> history;
    std::vector<Node> beam{{start, -1, 0, start.pressed}}, next;
    history.push_back({0u | uint32_t(start.pressed)});
    std::unordered_map<uint64_t, size_t> seen;
    seen.reserve(size_t(opt.beamWidth) * 4);
    int layer = 0;
    const int startTick = start.tick;
    int winner = -1;
    GameState winState;
    while (!beam.empty()) {
        if (opt.progress && (layer & 255) == 0) {
            checkCancel(opt.progress);
            float x = beam.front().state.p[0].x;
            opt.progress->report("solve", std::clamp((x - start.p[0].x) / std::max(1.0f, goalX - start.p[0].x), 0.0f, 1.0f));
        }
        next.clear();
        seen.clear();
        std::vector<uint32_t> hist;
        for (size_t bi = 0; bi < beam.size(); ++bi) {
            const Node& nd = beam[bi];
            // try keeping the current input first (fewer toggles preferred), then switching
            for (int a = 0; a < 2; ++a) {
                bool held = a == 0 ? nd.held : !nd.held;
                GameState g = nd.state;
                for (int k = 0; k < K && !g.dead && !g.won; ++k) phys::step(W, g, held);
                ++res.expanded;
                if (g.dead) continue;
                res.bestX = std::max(res.bestX, g.p[0].x);
                if (g.won || g.p[0].x >= goalX) {
                    winner = int(hist.size());
                    hist.push_back((uint32_t(bi) << 1) | uint32_t(held));
                    winState = g;
                    break;
                }
                uint64_t key = stateKey(g, 0.05f, 0.0005f);
                int toggles = nd.toggles + (held != nd.held ? 1 : 0);
                auto it = seen.find(key);
                if (it != seen.end()) {
                    Node& ex = next[it->second];
                    if (opt.preferFewInputs && toggles < ex.toggles) {
                        ex = {g, int(bi), toggles, held};
                        hist[it->second] = (uint32_t(bi) << 1) | uint32_t(held);
                    }
                    continue;
                }
                seen.emplace(key, next.size());
                next.push_back({g, int(bi), toggles, held});
                hist.push_back((uint32_t(bi) << 1) | uint32_t(held));
            }
            if (winner >= 0) break;
        }
        ++layer;
        history.push_back(std::move(hist));
        if (winner >= 0) break;
        if (next.empty()) { res.failTick = beam.front().state.tick; break; }
        if (int(next.size()) > opt.beamWidth) {
            // Keep a diverse subset: bucket by coarse (y, v, mode, gravity), best (fewest toggles) per bucket first.
            std::vector<size_t> order(next.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return next[a].toggles < next[b].toggles; });
            std::unordered_map<uint64_t, int> coarse;
            std::vector<size_t> keep, rest;
            for (size_t i : order) {
                uint64_t ck = stateKey(next[i].state, 3.0f, 0.75f);
                if (coarse[ck]++ == 0) keep.push_back(i);
                else rest.push_back(i);
            }
            for (size_t i : rest) { if (int(keep.size()) >= opt.beamWidth) break; keep.push_back(i); }
            if (int(keep.size()) > opt.beamWidth) keep.resize(size_t(opt.beamWidth));
            std::sort(keep.begin(), keep.end());
            std::vector<Node> pruned;
            std::vector<uint32_t> prunedHist;
            pruned.reserve(keep.size());
            for (size_t i : keep) { pruned.push_back(next[i]); prunedHist.push_back(history.back()[i]); }
            history.back() = std::move(prunedHist);
            next = std::move(pruned);
        }
        beam.swap(next);
        if (beam.front().state.tick - startTick > opt.maxTicks) break;
    }
    if (winner >= 0) {
        // reconstruct held state per layer
        std::vector<bool> heldPerLayer(static_cast<size_t>(layer));
        int idx = winner;
        for (int l = layer; l >= 1; --l) {
            uint32_t h = history[size_t(l)][size_t(idx)];
            heldPerLayer[size_t(l - 1)] = (h & 1u) != 0;
            idx = int(h >> 1);
        }
        bool cur = start.pressed;
        for (int l = 0; l < layer; ++l) {
            bool h = heldPerLayer[size_t(l)];
            if (h != cur) { res.inputs.add(startTick + l * K + 1, h); cur = h; }
        }
        res.solved = true;
        res.bestX = winState.p[0].x;
    }
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

SolveResult autoSolve(const World& W, const SolveOptions& opt) { return autoSolveFrom(W, phys::initialState(W), opt); }

// ---------------------------------------------------------------- timing windows

std::vector<TimingWindow> timingWindows(const World& W, const InputTrack& in, int maxShift, int horizonTicks) {
    std::vector<TimingWindow> out;
    if (in.events.empty()) return out;
    // Snapshots of the reference run, taken a little before each event.
    std::vector<GameState> snaps(in.events.size());
    std::vector<float> xs(in.events.size(), 0);
    {
        GameState g = phys::initialState(W);
        size_t ei = 0, si = 0;
        bool held = false;
        int endTick = in.events.back().tick + horizonTicks + 2;
        while (!g.dead && !g.won && g.tick < endTick) {
            int nextT = g.tick + 1;
            while (si < in.events.size() && in.events[si].tick - maxShift - 1 <= g.tick) { snaps[si] = g; ++si; }
            while (ei < in.events.size() && in.events[ei].tick <= nextT) { held = in.events[ei].press; xs[ei] = g.p[0].x; ++ei; }
            phys::step(W, g, held);
        }
        while (si < in.events.size()) { snaps[si] = g; ++si; }
    }
    for (size_t i = 0; i < in.events.size(); ++i) {
        TimingWindow tw;
        tw.eventIndex = int(i);
        tw.tick = in.events[i].tick;
        tw.press = in.events[i].press;
        tw.x = xs[i];
        // limits: the shifted event must stay strictly between its neighbours
        int lo = i > 0 ? in.events[i - 1].tick + 1 : 1;
        int hi = i + 1 < in.events.size() ? in.events[i + 1].tick - 1 : tw.tick + maxShift;
        int horizonEnd = std::max(tw.tick + horizonTicks, i + 2 < in.events.size() ? in.events[i + 2].tick + 1 : tw.tick + horizonTicks);
        auto survives = [&](int shift) {
            int t = tw.tick + shift;
            if (t < lo || t > hi) return false;
            InputTrack mod = in;
            mod.events[i].tick = t;
            GameState g = snaps[i];
            if (g.dead) return false;
            RunOptions ro;
            ro.maxTicks = horizonEnd + maxShift - g.tick;
            auto r = runFrom(W, g, mod, ro);
            return !r.dead;
        };
        if (!survives(0)) { out.push_back(tw); continue; }
        while (tw.early < maxShift && survives(-(tw.early + 1))) ++tw.early;
        while (tw.late < maxShift && survives(tw.late + 1)) ++tw.late;
        out.push_back(tw);
    }
    return out;
}

} // namespace al::bot
