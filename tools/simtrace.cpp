// Simulator vs the real game: replay a route (one "tick press" per line) on a level and compare
// every tick with a trace logged in GD (tab-separated, columns tick, hold, x, y, vy, ...).
//   simtrace <level-string file> <route> <trace> [yOffset]
#include <al/level/level.hpp>
#include <al/physics/sim.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace al;

struct Row { int tick; double x, y, vy; };

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: simtrace <level> <route> <trace> [yOffset]\n"); return 2; }
    std::ifstream lf(argv[1], std::ios::binary);
    std::stringstream ss; ss << lf.rdbuf();
    std::string err;
    auto lvl = level::Level::fromLevelString(ss.str(), &err);
    if (!lvl) { std::fprintf(stderr, "level: %s\n", err.c_str()); return 1; }
    const double yOffset = argc >= 5 ? std::stod(argv[4]) : 90.0;
    const int dbgFrom = argc >= 7 ? std::stoi(argv[5]) : -1, dbgTo = argc >= 7 ? std::stoi(argv[6]) : -1;

    std::map<int, bool> inputs;
    std::ifstream rf(argv[2]);
    for (std::string line; std::getline(rf, line);) {
        if (line.empty() || line[0] == '#') continue;
        int tick = 0, press = 0;
        if (std::sscanf(line.c_str(), "%d %d", &tick, &press) == 2) inputs[tick] = press != 0;
    }

    std::vector<Row> rows;
    std::ifstream tf(argv[3]);
    std::string line;
    std::getline(tf, line);
    while (std::getline(tf, line)) {
        Row r{}; int hold = 0;
        if (std::sscanf(line.c_str(), "%d\t%d\t%lf\t%lf\t%lf", &r.tick, &hold, &r.x, &r.y, &r.vy) == 5) rows.push_back(r);
    }

    phys::World world(*lvl);
    world.setEndX(1e9f);
    auto gs = phys::initialState(world);
    bool pressed = false;
    double sumDx = 0, sumDy = 0, maxDx = 0, maxDy = 0, lastE = 0;
    int n = 0, first = -1, deathTick = -1, over = 0, deaths = 0, drifts = 0;
    const bool resync = std::getenv("SIMTRACE_RESYNC") != nullptr;
    for (const Row& r : rows) {
        if (auto it = inputs.find(r.tick); it != inputs.end()) pressed = it->second;
        phys::step(world, gs, pressed);
        if (gs.dead && resync) {
            // keep going from the game's state, so one mismatch does not hide everything after it
            auto& q = gs.p[0];
            ++deaths;
            std::printf("death   tick %6d x %8.1f y %7.2f  game y %7.2f  %s obj %d (id %d) mode %s\n", r.tick, q.x, q.y + yOffset, r.y,
                        phys::deathCauseName(q.deathCause), q.deathObj,
                        q.deathObj >= 0 ? lvl->objects[size_t(world.objects()[size_t(q.deathObj)].levelIndex)].id : 0, level::modeName(q.mode));
            gs.dead = false; q.dead = false; q.y = float(r.y - yOffset); q.v = r.vy; q.lastY = q.y;
            over = 0;
        }
        if (gs.dead) { deathTick = r.tick; break; }
        auto& p = gs.p[0];
        if (resync && std::fabs(p.y + yOffset - r.y) > 2) {
            if (++over >= 4) {
                ++drifts;
                std::printf("drift   tick %6d x %8.1f sim y %7.2f game y %7.2f  v %6.2f game v %6.2f mode %s speed %d ground %d flip %d ev %x\n", r.tick, p.x, p.y + yOffset, r.y,
                            p.v, r.vy, level::modeName(p.mode), int(p.speed), int(p.onGround), int(p.upsideDown), p.events);
                p.y = float(r.y - yOffset); p.v = r.vy; p.lastY = p.y;
                over = 0;
            }
            continue;
        }
        if (resync) over = 0;
        if (const double e = p.x - r.x; resync && std::fabs(e - lastE) > 0.4) {
            std::printf("xshift  tick %6d x %8.2f game x %8.2f error %+.2f speed %d mode %s ev %x\n", r.tick, p.x, r.x, e, int(p.speed), level::modeName(p.mode), p.events);
            lastE = e;
        }
        if (r.tick >= dbgFrom && r.tick <= dbgTo)
            std::printf("%d %c game y %.3f vy %.3f | sim y %.3f v %.3f mode %s ground %d grav %d ev %x obj %d\n", r.tick, pressed ? 'P' : '.', r.y, r.vy,
                        p.y + yOffset, p.v, level::modeName(p.mode), int(p.onGround), int(p.upsideDown), p.events, p.eventObj);
        if (r.tick >= dbgFrom && r.tick <= dbgTo)
            for (const auto& o : world.objects())
                if (o.kind == level::Kind::Pad && std::fabs(o.x - p.x) < 20)
                    std::printf("   pad x %.1f y %.1f hw %.2f hh %.2f down %d activated %d | player h %.2f y0 %.3f y1 %.3f\n", o.x, o.y, o.hw, o.hh, int(o.facingDown),
                                int(p.wasActivated(int(&o - world.objects().data()))), p.height(), p.y - p.height() / 2, p.y + p.height() / 2);
        const double dx = std::fabs(p.x - r.x), dy = std::fabs(p.y + yOffset - r.y);
        sumDx += dx; sumDy += dy; ++n;
        maxDx = std::max(maxDx, dx); maxDy = std::max(maxDy, dy);
        over = (dx > 2 || dy > 2) ? over + 1 : 0;
        if (first < 0 && over >= 4) {
            first = r.tick;
            std::printf("first sustained deviation over 2 units: tick %d, game (%.2f, %.2f) sim (%.2f, %.2f) mode %s speed %d\n", r.tick, r.x,
                        r.y, p.x, p.y + yOffset, level::modeName(p.mode), int(p.speed));
        }
        if (r.tick % 3000 == 0)
            std::printf("tick %6d  game (%9.2f, %7.2f)  sim (%9.2f, %7.2f)  mode %s\n", r.tick, r.x, r.y, p.x, p.y + yOffset, level::modeName(p.mode));
    }
    if (resync) std::printf("resync: %d deaths, %d drifts\n", deaths, drifts);
    std::printf("compared %d/%zu ticks  x error mean %.3f max %.3f  y error mean %.3f max %.3f\n", n, rows.size(), n ? sumDx / n : 0, maxDx,
                n ? sumDy / n : 0, maxDy);
    if (deathTick >= 0) {
        const auto& p = gs.p[0];
        std::printf("simulator died at tick %d (%.2f, %.2f): %s, object %d\n", deathTick, p.x, p.y + yOffset, phys::deathCauseName(p.deathCause), p.deathObj);
    }
    return 0;
}
