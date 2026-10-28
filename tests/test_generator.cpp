// Generator tests: every gamemode on the demo song, a fuzz run over seeds/difficulties/styles
// (zero unbeatable outputs, the designed inputs must beat every level), determinism and the
// level file round trip.
#include "test.hpp"

#include <algorithm>
#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/generator.hpp>
#include <al/physics/sim.hpp>
#include <al/util/rng.hpp>

#include <chrono>
#include <cstdlib>

using namespace al;
using namespace al::gen;

namespace {

const audio::MusicAnalysis& demoAnalysis() {
    static const audio::MusicAnalysis A = audio::analyze(audio::render(audio::demoSong()));
    return A;
}

GenOptions optionsFor(uint64_t seed, Difficulty d, int onlyMode = -1, Style style = Style::Auto, int engine = 1) {
    GenOptions opt;
    opt.engine = engine;
    opt.seed = seed;
    opt.difficulty = d;
    opt.style = style;
    opt.variants = 1;
    if (onlyMode >= 0) {
        opt.modeWeights.fill(0.0f);
        opt.modeWeights[size_t(onlyMode)] = 1.0f;
    }
    return opt;
}

} // namespace

TEST(generator_every_mode_with_designed_inputs) {
    const auto& A = demoAnalysis();
    for (int m = 0; m < 8; ++m) {
        auto opt = optionsFor(uint64_t(11 + m), Difficulty::Insane, m);
        auto t0 = std::chrono::steady_clock::now();
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto& r = res.report;
        std::printf("    %-6s %3d clicks, %3d forced, %2d dropped, window min %5.1f ms, %d fallback parts, %.2f s\n",
                    level::modeName(level::Mode(m)), r.clicks, r.forced, r.dropped, r.minWindowMs, r.fallbackParts, sec);
        CHECK_MSG(r.beatable && r.intendedInputs, "mode %d: beatable %d intended %d", m, r.beatable, r.intendedInputs);
        CHECK_MSG(r.dropped * 10 <= r.clicks, "mode %d: %d of %d clicks dropped", m, r.dropped, r.clicks);
        CHECK_MSG(r.fallbackParts == 0, "mode %d: %d fallback parts", m, r.fallbackParts);
        CHECK_MSG(r.minWindowMs >= 0.85 * profile(Difficulty::Insane).minWindowMs, "mode %d: min window %.1f ms", m, r.minWindowMs);
        CHECK_MSG(sec < 10.0, "mode %d: %.2f s", m, sec);
    }
}

TEST(generator_fuzz_zero_unbeatable) {
    const auto& A = demoAnalysis();
    Rng rng(2024);
    int n = 0, bad = 0, intendedBad = 0;
    double worst = 0;
    const char* env = std::getenv("AL_FUZZ_N"); // larger runs: AL_FUZZ_N=500 ./al_tests
    const int count = env ? std::max(1, std::atoi(env)) : 24;
    for (int i = 0; i < count; ++i) {
        auto d = Difficulty(rng.below(10));
        auto st = Style(rng.below(uint32_t(Style::Count)));
        auto opt = optionsFor(rng.next(), d, -1, st);
        auto t0 = std::chrono::steady_clock::now();
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, sec);
        if (sec > 5.0)
            std::printf("    slow: seed %llu difficulty %d style %d: %.2f s\n", (unsigned long long)opt.seed, int(d), int(st), sec);
        ++n;
        // independent re-verification of the shipped level with the shipped inputs
        phys::World W(res.level);
        bool ok = bot::isBeatable(W, res.inputs);
        if (!ok || !res.report.beatable) {
            ++bad;
            std::printf("    UNBEATABLE: seed %llu difficulty %d style %d: %s\n", (unsigned long long)opt.seed, int(d), int(st),
                        res.report.notes.empty() ? "" : res.report.notes.back().c_str());
        }
        for (auto& note : res.report.notes)
            if (note.find("repaired") != std::string::npos)
                std::printf("    repair: seed %llu difficulty %d style %d: %s\n", (unsigned long long)opt.seed, int(d), int(st), note.c_str());
        if (!res.report.intendedInputs) {
            ++intendedBad;
            std::printf("    designed inputs failed: seed %llu difficulty %d style %d\n", (unsigned long long)opt.seed, int(d), int(st));
        }
    }
    std::printf("    %d levels, %d unbeatable, %d needed the bot search, slowest %.2f s\n", n, bad, intendedBad, worst);
    CHECK(bad == 0);
    CHECK(intendedBad == 0);
}

TEST(generator_same_seed_same_layout) {
    const auto& A = demoAnalysis();
    auto opt = optionsFor(99, Difficulty::Harder);
    auto a = generateVariant(A, opt, 99, nullptr);
    auto b = generateVariant(A, opt, 99, nullptr);
    auto c = generateVariant(A, opt, 100, nullptr);
    CHECK(a.level.toLevelString() == b.level.toLevelString());
    CHECK(a.level.toLevelString() != c.level.toLevelString());
}

TEST(generator_level_file_roundtrip) {
    const auto& A = demoAnalysis();
    auto opt = optionsFor(5, Difficulty::Hard);
    auto res = generateVariant(A, opt, 5, nullptr);
    std::string gmd = res.level.toGmd();
    auto parsed = level::Level::fromGmd(gmd);
    REQUIRE(parsed.has_value());
    const level::Level& back = *parsed;
    CHECK(back.objects.size() == res.level.objects.size());
    phys::World W(back);
    CHECK(bot::isBeatable(W, res.inputs));
}

TEST(generator_repair_rebuilds_a_part_with_the_safe_fallback) {
    // the repair path the generator takes if the final verification ever fails: one part is
    // rebuilt without its clicks, the parts after it on top, and the designed inputs still win
    const auto& A = demoAnalysis();
    auto opt = optionsFor(77, Difficulty::Harder);
    Rng rng(combineSeed(opt.seed, hashStr("autolayout")));
    DesignPlan plan = planFromMusic(A, opt, rng);
    Builder b(A, opt, plan, nullptr, rng.fork(hashStr("build")));
    CHECK(b.build());
    const int part = int(plan.parts.size()) / 2;
    const size_t partsBefore = b.partResults().size();
    b.forceFallback(part);
    CHECK(b.rebuildFrom(part));
    CHECK(b.partResults().size() == partsBefore);
    CHECK(b.partResults()[size_t(part)].fallback);
    CHECK(b.partResults()[size_t(part)].note == "repaired with the safe fallback");
    for (int i = 0; i < int(b.partResults().size()); ++i)
        if (i != part) CHECK_MSG(!b.partResults()[size_t(i)].fallback, "part %d", i);
    phys::World W(b.level());
    CHECK(bot::isBeatable(W, b.inputs()));
    int droppedInPart = 0;
    for (auto& c : b.clickResults()) if (c.dropped && c.pattern == "fallback") ++droppedInPart;
    CHECK(droppedInPart == int(plan.parts[size_t(part)].clicks.size()));
}

// ---------------------------------------------------------------- generator v2

TEST(generator_v2_fuzz_beatable_with_designed_clicks) {
    const auto& A = demoAnalysis();
    Rng rng(77);
    const char* env = std::getenv("AL_FUZZ_N");
    const int count = env ? std::max(1, std::atoi(env)) : 10;
    int bad = 0, clicks = 0, used = 0;
    double worst = 0;
    for (int i = 0; i < count; ++i) {
        auto d = Difficulty(rng.below(10));
        auto st = Style(rng.below(uint32_t(Style::Count)));
        auto opt = optionsFor(rng.next(), d, -1, st, 2);
        auto t0 = std::chrono::steady_clock::now();
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, sec);
        phys::World W(res.level);
        const bool ok = bot::isBeatable(W, res.inputs) && res.report.beatable && res.report.intendedInputs;
        if (!ok) {
            ++bad;
            std::printf("    FAILED: seed %llu difficulty %d style %d\n", (unsigned long long)opt.seed, int(d), int(st));
        }
        clicks += res.report.clicks;
        used += res.report.clicks - res.report.dropped;
    }
    std::printf("    %d levels, %d failed, %d of %d clicks used, slowest %.2f s\n", count, bad, used, clicks, worst);
    CHECK(bad == 0);
    CHECK(used * 100 >= clicks * 95);
    CHECK(worst < 10.0);
}

TEST(generator_v2_every_gamemode) {
    const auto& A = demoAnalysis();
    for (int m = 0; m < kModeCount; ++m) {
        auto opt = optionsFor(uint64_t(31 + m), Difficulty::Insane, m, Style::Auto, 2);
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        double inMode = 0, total = 0;
        for (auto& p : res.plan.parts) { total += p.t1 - p.t0; if (int(p.mode) == m) inMode += p.t1 - p.t0; }
        std::printf("    %-6s %3d clicks, %2d left out, %3d forced, window min %5.1f ms, %.0f%% of the time in the mode\n", level::modeName(level::Mode(m)),
                    res.report.clicks, res.report.dropped, res.report.forced, res.report.minWindowMs, 100.0 * inMode / std::max(1e-9, total));
        CHECK_MSG(res.report.beatable && res.report.intendedInputs, "mode %d", m);
        CHECK_MSG(inMode >= 0.95 * total, "mode %d: only %.0f%%", m, 100.0 * inMode / total);
        CHECK_MSG(res.report.dropped * 20 <= res.report.clicks, "mode %d: %d of %d left out", m, res.report.dropped, res.report.clicks);
    }
}

TEST(generator_v2_same_seed_same_layout_and_reroll) {
    const auto& A = demoAnalysis();
    auto opt = optionsFor(123, Difficulty::Harder, -1, Style::Auto, 2);
    auto a = generateVariant(A, opt, 123, nullptr);
    auto b = generateVariant(A, opt, 123, nullptr);
    CHECK(a.level.toLevelString() == b.level.toLevelString());
    REQUIRE(a.plan.parts.size() > 4);
    // re-roll one segment: everything well before it stays the same
    const int k = int(a.plan.parts.size()) / 2;
    auto r = rerollPart(a, k, A, opt, 999, nullptr);
    CHECK(r.report.beatable && r.report.intendedInputs);
    const double t = a.plan.parts[size_t(k)].t0;
    const double cutX = t * phys::unitsPerSecond(1) * 0.8; // safely before the segment at any speed
    auto before = [&](const level::Level& L) {
        std::vector<std::string> v;
        for (auto& o : L.objects) if (o.x < cutX && o.id != level::id::TrigEnd) v.push_back(std::to_string(o.id) + "@" + std::to_string(int(o.x)) + "," + std::to_string(int(o.y)));
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(before(a.level) == before(r.level));
    CHECK(a.level.toLevelString() != r.level.toLevelString());
}

