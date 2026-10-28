#include <al/rec/validate.hpp>

#include <al/bot/bot.hpp>
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>

namespace al::rec {

Validation validate(const level::Level& lvl, const Recording& rec, float yOffset, double threshold) {
    Validation v;
    phys::World W(lvl);
    bot::RunOptions ro;
    ro.recordTrace = true;
    auto run = bot::run(W, rec.toInputTrack(), ro);
    v.simFinishes = run.won;
    v.simTicks = run.ticks;
    if (run.dead) v.simDeath = phys::deathCauseName(run.cause);
    double sx = 0, sy = 0;
    for (const auto& e : rec.events) {
        if (e.player2) continue;
        if (e.p1.x == 0 && e.p1.y == 0) continue; // imported macros carry no state
        int k = rec.tps == 240.0 ? e.tick : int(std::lround(e.tick * 240.0 / rec.tps));
        if (k < 1 || k > int(run.trace.size())) continue;
        const auto& p = run.trace[size_t(k - 1)];
        double dx = std::fabs(double(p.x) - e.p1.x), dy = std::fabs(double(p.y) - (double(e.p1.y) - yOffset));
        sx += dx;
        sy += dy;
        v.maxDx = std::max(v.maxDx, dx);
        v.maxDy = std::max(v.maxDy, dy);
        if (int(p.mode) != int(e.p1.mode)) ++v.modeMismatches;
        if (v.firstDeviationTick < 0 && (dx > threshold || dy > threshold)) v.firstDeviationTick = k;
        ++v.compared;
    }
    if (v.compared) {
        v.meanDx = sx / v.compared;
        v.meanDy = sy / v.compared;
    }
    return v;
}

} // namespace al::rec
