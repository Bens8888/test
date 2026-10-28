// Integration test of click-recording mode on the bundled demo song: a simulated player's
// recording is fused with the music (roles, latency, figures, motifs) and turned into levels at
// strict and creative faithfulness; every level must be beaten by its designed inputs.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/gen/fusion.hpp>
#include <al/gen/generator.hpp>
#include <al/physics/sim.hpp>
#include <al/rec/demo.hpp>

#include <chrono>

using namespace al;
using namespace al::gen;

namespace {

struct Demo {
    audio::SynthScore song;
    audio::MusicAnalysis A;
    rec::Recording recording;
};

const Demo& demo() {
    static const Demo d = [] {
        Demo x;
        x.song = audio::demoSong();
        x.A = audio::analyze(audio::render(x.song));
        x.recording = rec::demoRecording(x.song);
        return x;
    }();
    return d;
}

} // namespace

TEST(fusion_roles_latency_figures_motifs) {
    const auto& d = demo();
    auto F = fuseClicks(d.recording, d.A);
    std::printf("    %s\n", F.summary().c_str());
    CHECK(F.clicks.size() > 150);
    CHECK_NEAR(F.latencyMs, 25.0, 8.0);         // the simulated player is 25 ms late
    CHECK_MSG(F.matchedFraction > 0.9f, "matched %.2f", F.matchedFraction);
    CHECK_MSG(F.meanAbsOffsetMs < 15.0f, "mean offset %.1f ms", F.meanAbsOffsetMs);
    CHECK_MSG(F.holds >= 3, "holds %d", F.holds);
    CHECK_MSG(!F.motifs.empty(), "no motif found in a looping drum part");
    int kickOrSnare = 0;
    for (auto& c : F.clicks) if (c.role == Role::Kick || c.role == Role::Snare || c.role == Role::Downbeat || c.role == Role::Beat) ++kickOrSnare;
    CHECK_MSG(kickOrSnare * 10 >= int(F.clicks.size()) * 6, "drum roles %d of %zu", kickOrSnare, F.clicks.size());
    CHECK(!F.hasModes); // played as a cube on an empty level
}

TEST(fusion_strict_and_creative_levels_are_beatable) {
    const auto& d = demo();
    auto F = fuseClicks(d.recording, d.A);
    for (float faith : {1.0f, 0.5f, 0.1f}) {
        GenOptions opt;
        opt.input = InputSource::Recording;
        opt.faithfulness = faith;
        opt.difficulty = Difficulty::Harder;
        opt.seed = 21;
        Rng rng(opt.seed);
        DesignPlan plan = planFromRecording(F, d.A, opt, rng);
        int recorded = 0, added = 0;
        for (auto& c : plan.clicks) (c.recorded ? recorded : added)++;
        auto t0 = std::chrono::steady_clock::now();
        auto res = generateVariant(d.A, opt, opt.seed, nullptr, nullptr, &plan);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int keptRecorded = 0;
        for (auto& cr : res.clicks) if (!cr.dropped && plan.clicks[size_t(cr.click)].recorded) ++keptRecorded;
        std::printf("    faithfulness %.1f: plan %d recorded + %d added clicks, built %d recorded (%.0f%%), %d dropped, %.2f s, %s\n", faith, recorded, added,
                    keptRecorded, 100.0 * keptRecorded / std::max(1, recorded), res.report.dropped, sec, res.report.intendedInputs ? "designed inputs win" : "NEEDS BOT");
        CHECK(res.report.beatable);
        CHECK(res.report.intendedInputs);
        if (faith >= 1.0f) {
            CHECK(added == 0);
            CHECK(recorded == int(F.clicks.size()));
            // strict: the built clicks are exactly the recorded ones
            CHECK_MSG(keptRecorded * 100 >= recorded * 85, "kept %d of %d", keptRecorded, recorded);
            for (auto& c : plan.clicks) {
                bool found = false;
                for (auto& fc : F.clicks) if (std::fabs(fc.rawTime - c.time) < 1e-9) found = true;
                CHECK(found);
                if (!found) break;
            }
        } else {
            CHECK(recorded > 0);
        }
    }
}

TEST(fusion_recording_roundtrip_gives_same_plan) {
    const auto& d = demo();
    auto back = rec::Recording::deserialize(d.recording.serialize());
    REQUIRE(back.has_value());
    auto F1 = fuseClicks(d.recording, d.A), F2 = fuseClicks(*back, d.A);
    REQUIRE(F1.clicks.size() == F2.clicks.size());
    for (size_t i = 0; i < F1.clicks.size(); ++i) CHECK_NEAR(F1.clicks[i].time, F2.clicks[i].time, 1e-6);
}
