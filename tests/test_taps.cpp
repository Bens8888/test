// Tap-along tracks (Guideline Creator style): takes merge into one track, guidelines round-trip,
// the song clock is precise despite a coarse engine position, and a track becomes a level that
// keeps the taps where they were tapped and ends after the last one.
#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/gen/fusion.hpp>
#include <al/gen/generator.hpp>
#include <al/rec/demo.hpp>
#include <al/level/level.hpp>
#include <al/rec/taps.hpp>

#include <algorithm>
#include <cmath>

using namespace al;
using namespace al::rec;

namespace {

Tap tap(double t, double len = 0.05) { return {t, t + len}; }

} // namespace

TEST(taps_merge_takes) {
    TapTrack tr;
    tr.merge({tap(1.0), tap(2.0), tap(3.0)});
    CHECK(tr.taps().size() == 3 && tr.takes() == 1);
    // second take: re-taps 2.0 slightly late (replaces it), adds 2.5 and 4.0
    tr.merge({tap(2.03), tap(2.5), tap(4.0)});
    REQUIRE(tr.taps().size() == 5);
    CHECK_NEAR(tr.taps()[1].time, 2.03, 1e-9);
    CHECK_NEAR(tr.taps()[2].time, 2.5, 1e-9);
    for (size_t i = 1; i < tr.taps().size(); ++i) CHECK(tr.taps()[i - 1].time <= tr.taps()[i].time);
    // fast taps inside one take are all kept (a double is two clicks)
    tr.merge({tap(6.0, 0.02), tap(6.04, 0.02)});
    CHECK(tr.taps().size() == 7);
    CHECK(tr.undoLastTake());
    CHECK(tr.taps().size() == 5);
    CHECK(tr.undoLastTake() && tr.undoLastTake());
    CHECK(tr.empty() && !tr.undoLastTake());
    tr.merge({tap(1.0)});
    tr.clear();
    CHECK(tr.empty() && tr.takes() == 0);
}

TEST(taps_guidelines_and_json_roundtrip) {
    TapTrack tr;
    tr.merge({tap(0.5), tap(1.25, 0.6), tap(2.0), tap(2.1)});
    const double off = 0.3; // the level's song offset: guideline time = song time - 0.3
    std::string g = tr.toGuidelines(off);
    CHECK(g.find("0.2000~0.8~") == 0);
    auto back = TapTrack::fromGuidelines(g, off);
    REQUIRE(back.size() == 4);
    for (size_t i = 0; i < back.size(); ++i) CHECK_NEAR(back[i].time, tr.taps()[i].time, 1e-4);
    CHECK_NEAR(back[1].release, 1.85, 1e-4); // the hold's end came back from its yellow line
    CHECK(back[0].release - back[0].time < 0.15);
    // GD's own guideline strings (any colour), junk ignored
    auto gd = TapTrack::fromGuidelines("1.5~0.8~2~1~bad~0.8~3.25~0~", 0.0);
    REQUIRE(gd.size() == 3);
    CHECK_NEAR(gd[2].time, 3.25, 1e-9);
    // JSON (the saved track) and importAny
    auto j = tr.toJson();
    auto tr2 = TapTrack::fromJson(j);
    REQUIRE(tr2.has_value());
    CHECK(tr2->taps().size() == tr.taps().size());
    auto r = importAny(j.dump());
    REQUIRE(r.has_value());
    CHECK(r->source == "taps");
    CHECK(r->pressTimes().size() == 4);
    CHECK_NEAR(r->pressTimes()[1], 1.25, 1e-9);
}

TEST(taps_song_clock_is_precise) {
    // the engine reports the position in 21.3 ms blocks, read once per 60 fps frame
    SongClock clk;
    const double block = 1024.0 / 48000.0, start = 12.0;
    auto engine = [&](double trueSong) { return std::floor(trueSong / block) * block; };
    double worst = 0;
    for (int f = 0; f < 600; ++f) {
        const double clock = 100.0 + f / 60.0 + (f % 7) * 0.0007;
        const double song = start + (clock - 100.0);
        clk.sample(clock, engine(song));
        if (f > 60) {
            // a tap between frames
            const double tapClock = clock + 0.008;
            worst = std::max(worst, std::fabs(clk.songTimeAt(tapClock) - (start + tapClock - 100.0)));
        }
    }
    CHECK_MSG(worst < block * 0.75, "worst error %.1f ms", worst * 1000.0);
    // a seek restarts the estimate instead of averaging two positions
    clk.sample(200.0, 50.0);
    CHECK_NEAR(clk.songTimeAt(200.0), 50.0, 1e-9);
}

TEST(taps_track_makes_a_level_of_the_tapped_part) {
    auto song = audio::demoSong();
    auto A = audio::analyze(audio::render(song));
    // tap along to the first 20 s (the demo player's presses, as a tap track)
    auto demoRec = demoRecording(song);
    std::vector<Tap> take;
    for (double t : demoRec.pressTimes()) if (t < 20.0) take.push_back(tap(t));
    TapTrack tr;
    tr.merge(take);
    auto F = gen::fuseClicks(tr.toRecording("taps"), A);
    gen::GenOptions opt;
    opt.input = gen::InputSource::Taps;
    opt.faithfulness = 1.0f;
    opt.seed = 5;
    Rng rng(opt.seed);
    auto plan = gen::planFromRecording(F, A, opt, rng);
    REQUIRE(plan.clicks.size() == take.size());
    for (size_t i = 0; i < take.size(); ++i) CHECK_NEAR(plan.clicks[i].time, take[i].time - F.latencyMs / 1000.0, 1e-6);
    CHECK_MSG(plan.duration < take.back().time + 2.5, "level runs to %.1f s, last tap %.1f s", plan.duration, take.back().time);
    auto res = gen::generateVariant(A, opt, opt.seed, nullptr, nullptr, &plan);
    CHECK(res.report.beatable);
    int used = 0;
    for (auto& c : res.clicks) used += c.dropped ? 0 : 1;
    std::printf("    %zu taps, %d built, level %.1f s\n", take.size(), used, plan.duration);
    CHECK_MSG(used * 100 >= int(take.size()) * 90, "built %d of %zu taps", used, take.size());
    // nothing is built past the end of the tapped part
    double maxX = 0;
    for (auto& o : res.level.objects) maxX = std::max(maxX, o.x);
    const double endX = res.level.objects.empty() ? 0 : maxX;
    CHECK_MSG(endX < 30000.0, "objects up to x=%.0f", endX);
}

TEST(taps_late_take_starts_the_level_at_the_song_offset) {
    auto song = audio::demoSong();
    auto A = audio::analyze(audio::render(song));
    // the trimmed analysis is the same music, shifted
    double used = 0;
    auto B = audio::trimStart(A, 12.3, &used);
    CHECK(used > 12.2 && used <= 12.3);
    CHECK_NEAR(B.duration, A.duration - used, 1e-9);
    for (double t : {0.5, 3.0, 10.0}) {
        CHECK_NEAR(B.onsetAt(t), A.onsetAt(t + used), 1e-6);
        CHECK_NEAR(B.intensityAt(t), A.intensityAt(t + used), 1e-6);
        CHECK_NEAR(B.pitchAt(t), A.pitchAt(t + used), 1e-6);
    }
    REQUIRE(!B.events.empty() && !B.beats.empty());
    CHECK(B.events.front().time >= 0 && B.beats.front() >= 0);
    CHECK(B.beats.size() == B.beatStrength.size() && B.beats.size() == B.beatInBar.size());
    CHECK(!B.sections.empty() && B.sections.front().start == 0.0);
    // a take of the middle of the song
    auto demoRec = demoRecording(song);
    std::vector<Tap> take;
    for (double t : demoRec.pressTimes()) if (t > 20.0 && t < 34.0) take.push_back(tap(t));
    REQUIRE(take.size() > 8);
    TapTrack tr;
    tr.merge(take);
    auto rec = tr.toRecording();
    const double start = gen::levelStartFor(rec);
    CHECK(start > 15.0 && start < take.front().time);
    double songStart = 0;
    auto At = audio::trimStart(A, start, &songStart);
    auto F = gen::fuseClicks(gen::shiftRecording(rec, songStart), At);
    gen::GenOptions opt;
    opt.seed = 9;
    Rng rng(opt.seed);
    auto plan = gen::planFromRecording(F, At, opt, rng);
    plan.songStart = songStart;
    // the first click comes after a short run-up, not 20 s of nothing
    REQUIRE(!plan.clicks.empty());
    CHECK(plan.clicks.front().time > 1.0 && plan.clicks.front().time < 4.0);
    CHECK_NEAR(plan.clicks.front().time + songStart, take.front().time - F.latencyMs / 1000.0, 1e-6);
    auto res = gen::generateVariant(At, opt, opt.seed, nullptr, nullptr, &plan);
    CHECK(res.report.beatable);
    CHECK_NEAR(res.level.settings.songOffset, songStart, 1e-9);
    {
        auto back = level::Level::fromLevelString(res.level.toLevelString());
        REQUIRE(back.has_value());
        CHECK_NEAR(back->settings.songOffset, songStart, 1e-3);
    }
    // re-rolling a part keeps the song offset
    auto rr = gen::rerollPart(res, 0, At, opt, 77, nullptr);
    CHECK_NEAR(rr.level.settings.songOffset, songStart, 1e-9);
}
