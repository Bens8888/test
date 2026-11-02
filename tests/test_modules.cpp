// The structure library and the passes that place it.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/bot/bot.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/modules.hpp>
#include <al/physics/sim.hpp>

using namespace al;
using namespace al::gen;

namespace {

const audio::MusicAnalysis& demoAnalysis() {
    static const audio::MusicAnalysis A = audio::analyze(audio::render(audio::demoSong()));
    return A;
}

level::LevelObject object(int id, double x, double y, double rot = 0) {
    level::LevelObject o;
    o.id = id;
    o.x = x;
    o.y = y;
    o.rotation = rot;
    return o;
}

int count(const level::Level& L, int id) {
    int n = 0;
    for (const auto& o : L.objects) n += o.id == id;
    return n;
}

} // namespace

TEST(modules_library_is_well_formed) {
    size_t n = 0;
    const Module* table = moduleTable(n);
    CHECK(n >= 100);
    int used[kStructureStyles] = {};
    for (size_t i = 0; i < n; ++i) {
        const Module& m = table[i];
        CHECK(m.count >= 2 && m.w >= 1 && m.h >= 1 && m.modes != 0);
        for (int s = 0; s < kStructureStyles; ++s) used[s] += m.weight[s] > 0;
        for (int k = 0; k < m.count; ++k) {
            const ModulePiece& p = modulePieces()[m.first + k];
            CHECK_MSG(p.rot % 90 == 0, "module %d piece %d rotated by %d", int(i), k, p.rot);
            const auto kind = level::objectInfo(p.id).kind;
            CHECK_MSG(kind == level::Kind::Solid || kind == level::Kind::Slope || kind == level::Kind::Hazard, "module %d: object %d", int(i), p.id);
        }
    }
    for (int s = 0; s < kStructureStyles; ++s) CHECK_MSG(used[s] >= 5, "style %s: %d modules", structureStyle(s).name, used[s]);
}

// Bloodbath's two-pad corridors, tick 1078 of a run recorded in GD: the ceiling pad touched while
// the gravity is normal does nothing, and fires as soon as the floor pad has flipped the gravity.
TEST(blue_pad_facing_away_stays_armed) {
    level::Level L;
    L.objects.push_back(object(67, 60, 27, 180));
    L.objects.push_back(object(67, 60, 3));
    phys::World W(L);
    W.setEndX(1e9f);
    auto s = phys::initialState(W);
    int flips = 0;
    bool wasFlipped = false;
    for (int t = 0; t < 40 && !s.dead; ++t) {
        phys::step(W, s, false);
        if (s.p[0].upsideDown != wasFlipped) ++flips;
        wasFlipped = s.p[0].upsideDown;
    }
    CHECK_MSG(flips == 2, "%d gravity flips", flips);
    CHECK(!s.p[0].upsideDown);
}

TEST(structure_styles_beatable_and_built_from_the_library) {
    const auto& A = demoAnalysis();
    for (int style = 1; style <= kStructureStyles; ++style) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = uint64_t(40 + style);
        opt.difficulty = Difficulty::Insane;
        opt.structureStyle = style;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        phys::World W(res.level);
        CHECK_MSG(bot::isBeatable(W, res.inputs), "style %d", style);
        CHECK_MSG(res.report.intendedInputs, "style %d: the designed clicks do not win", style);
        const int rails = count(res.level, level::id::OutlineTop);
        CHECK_MSG(rails > 0, "style %d: no library pieces", style);
    }
}

TEST(wave_parts_are_line_slopes) {
    const auto& A = demoAnalysis();
    GenOptions opt;
    opt.engine = 2;
    opt.seed = 7;
    opt.difficulty = Difficulty::Insane;
    opt.variants = 1;
    opt.modeWeights.fill(0.0f);
    opt.modeWeights[size_t(Mode::Wave)] = 1.0f;
    auto res = generateVariant(A, opt, opt.seed, nullptr);
    phys::World W(res.level);
    CHECK(bot::isBeatable(W, res.inputs));
    CHECK_MSG(count(res.level, 1338) > 100, "%d line slopes", count(res.level, 1338));
}

TEST(speed_style_steers_the_speed) {
    const auto& A = demoAnalysis();
    auto mean = [&](int speedStyle) {
        GenOptions opt;
        opt.engine = 2;
        opt.seed = 3;
        opt.difficulty = Difficulty::Harder;
        opt.speedStyle = speedStyle;
        opt.variants = 1;
        auto res = generateVariant(A, opt, opt.seed, nullptr);
        double sum = 0;
        for (const auto& p : res.plan.parts) sum += p.speed;
        return sum / double(std::max<size_t>(1, res.plan.parts.size()));
    };
    CHECK(mean(1) < mean(3));
}
