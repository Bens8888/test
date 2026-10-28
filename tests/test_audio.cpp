#include "test.hpp"

#include <al/audio/analysis.hpp>
#include <al/audio/fft.hpp>
#include <al/audio/synth.hpp>
#include <al/util/codec.hpp>
#include <al/util/rng.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>

using namespace al::audio;

TEST(fft_matches_dft) {
    for (size_t n : {8u, 64u, 1024u}) {
        std::vector<cfloat> a(n), ref(n);
        for (size_t i = 0; i < n; ++i) a[i] = cfloat(float(std::sin(0.37 * i) + 0.1 * i), float(std::cos(1.3 * i)));
        for (size_t k = 0; k < n; ++k) {
            std::complex<double> s = 0;
            for (size_t j = 0; j < n; ++j) s += std::complex<double>(a[j]) * std::polar(1.0, -2 * 3.141592653589793 * double(k * j) / double(n));
            ref[k] = cfloat(s);
        }
        Fft fft(n);
        auto b = a;
        fft.transform(b);
        double err = 0;
        for (size_t k = 0; k < n; ++k) err = std::max(err, double(std::abs(b[k] - ref[k])));
        CHECK_MSG(err < 1e-3 * double(n), "n=%zu err=%g", n, err);
        fft.transform(b, true);
        double rt = 0;
        for (size_t k = 0; k < n; ++k) rt = std::max(rt, double(std::abs(b[k] / float(n) - a[k])));
        CHECK_MSG(rt < 1e-4, "roundtrip n=%zu err=%g", n, rt);
    }
}

TEST(real_fft_matches_complex) {
    const size_t n = 512;
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = float(std::sin(0.1 * i) + 0.5 * std::sin(0.77 * i + 1) + ((i * 7919) % 13) / 13.0);
    RealFft rf(n);
    std::vector<cfloat> out;
    rf.forward(x.data(), out);
    Fft cf(n);
    std::vector<cfloat> c(n);
    for (size_t i = 0; i < n; ++i) c[i] = cfloat(x[i], 0);
    cf.transform(c);
    double err = 0;
    for (size_t k = 0; k <= n / 2; ++k) err = std::max(err, double(std::abs(out[k] - c[k])));
    CHECK_MSG(err < 1e-2, "err=%g", err);
}

TEST(fft_pure_tone_bin) {
    const size_t n = 2048;
    const int sr = 22050;
    std::vector<float> x(n);
    double f = 1000.0;
    for (size_t i = 0; i < n; ++i) x[i] = float(std::sin(2 * 3.141592653589793 * f * double(i) / sr));
    RealFft rf(n);
    std::vector<float> mag;
    auto w = hannWindow(n);
    for (size_t i = 0; i < n; ++i) x[i] *= w[i];
    rf.magnitude(x.data(), mag);
    size_t peak = size_t(std::max_element(mag.begin(), mag.end()) - mag.begin());
    CHECK_NEAR(double(peak) * sr / n, f, double(sr) / n);
}

static double medianAbsErrorMs(const std::vector<double>& truth, const std::vector<double>& got, double maxMatch, int* matched) {
    std::vector<double> errs;
    for (double t : truth) {
        auto it = std::lower_bound(got.begin(), got.end(), t);
        double best = 1e9;
        if (it != got.end()) best = std::min(best, std::fabs(*it - t));
        if (it != got.begin()) best = std::min(best, std::fabs(*(it - 1) - t));
        if (best <= maxMatch) errs.push_back(best * 1000);
    }
    if (matched) *matched = int(errs.size());
    if (errs.empty()) return 1e9;
    std::sort(errs.begin(), errs.end());
    return errs[errs.size() / 2];
}

TEST(beats_click_track_bpm_and_timing) {
    for (double bpm : {90.0, 128.0, 150.0, 174.0}) {
        auto score = clickTrack(bpm, 30, 0.5);
        auto buf = render(score);
        auto A = analyze(buf);
        CHECK_MSG(std::fabs(A.mainBpm() - bpm) < 0.6, "bpm %.2f detected %.3f", bpm, A.mainBpm());
        int matched = 0;
        double med = medianAbsErrorMs(score.beats, A.beats, 0.05, &matched);
        CHECK_MSG(matched >= int(score.beats.size() * 0.95), "bpm %.0f: matched %d of %zu beats", bpm, matched, score.beats.size());
        CHECK_MSG(med < 4.0, "bpm %.0f: median beat error %.2f ms", bpm, med);
    }
}

TEST(onsets_accuracy_within_few_ms) {
    // Irregular onsets (not on a grid) to test the onset detector independently of beat tracking.
    SynthScore s;
    s.duration = 20;
    std::vector<double> truth;
    double t = 0.6;
    al::Rng rng(3);
    while (t < 19) {
        s.drums.push_back({t, Drum::Clap, float(rng.uniform(0.6, 1.0))});
        truth.push_back(t);
        t += rng.uniform(0.15, 0.6);
    }
    auto buf = render(s);
    auto A = analyze(buf);
    std::vector<double> got;
    for (auto& e : A.events) if (e.type == EventType::Onset) got.push_back(e.time);
    int matched = 0;
    double med = medianAbsErrorMs(truth, got, 0.03, &matched);
    CHECK_MSG(matched >= int(truth.size() * 0.95), "matched %d of %zu", matched, truth.size());
    CHECK_MSG(med < 3.0, "median onset error %.2f ms", med);
    CHECK_MSG(got.size() < truth.size() * 1.3, "too many false onsets: %zu vs %zu", got.size(), truth.size());
}

TEST(tempo_change_detected) {
    // 100 -> 140 is close to 3:2 and must not be folded into a single tempo.
    for (auto [bpm1, bpm2] : {std::pair{120.0, 150.0}, std::pair{100.0, 140.0}}) {
        auto score = clickTrackTempoChange(bpm1, bpm2, 20, 45);
        auto buf = render(score);
        auto A = analyze(buf);
        REQUIRE(A.tempo.size() >= 2);
        CHECK_MSG(std::fabs(A.tempo.front().bpm - bpm1) < 1.5, "%.0f->%.0f: first segment %.2f bpm", bpm1, bpm2, A.tempo.front().bpm);
        CHECK_MSG(std::fabs(A.tempo.back().bpm - bpm2) < 2.0, "%.0f->%.0f: last segment %.2f bpm", bpm1, bpm2, A.tempo.back().bpm);
        CHECK_NEAR(A.tempo[1].start, 20, 6.0);
        int matched = 0;
        double med = medianAbsErrorMs(score.beats, A.beats, 0.035, &matched);
        CHECK_MSG(matched >= int(score.beats.size() * 0.9), "%.0f->%.0f: matched %d of %zu", bpm1, bpm2, matched, score.beats.size());
        CHECK_MSG(med < 4.0, "median error %.2f", med);
        // TempoChange events: strength is a 0..1 salience, the new tempo is in A.tempo[index]
        int changes = 0;
        for (auto& e : A.events) {
            if (e.type != EventType::TempoChange) continue;
            ++changes;
            CHECK(e.strength >= 0 && e.strength <= 1);
            REQUIRE(e.index > 0 && size_t(e.index) < A.tempo.size());
            CHECK_NEAR(e.duration, 60.0 / A.tempo[size_t(e.index)].bpm, 1e-4);
        }
        CHECK(changes == int(A.tempo.size()) - 1);
    }
}

TEST(drums_kick_snare_hat_classification) {
    auto score = drumLoop(120, 16);
    auto buf = render(score);
    auto A = analyze(buf);
    std::vector<double> kickT, snareT, hatT;
    for (auto& d : score.drums) {
        if (d.kind == Drum::Kick) kickT.push_back(d.time);
        if (d.kind == Drum::Snare) snareT.push_back(d.time);
        if (d.kind == Drum::HiHat) {
            bool coincides = false;
            for (auto& o : score.drums) if (o.kind != Drum::HiHat && std::fabs(o.time - d.time) < 1e-6) coincides = true;
            if (!coincides) hatT.push_back(d.time); // off-beat hats only
        }
    }
    auto times = [&](EventType t) { std::vector<double> v; for (auto& e : A.events) if (e.type == t) v.push_back(e.time); return v; };
    int mk = 0, ms = 0, mh = 0;
    medianAbsErrorMs(kickT, times(EventType::Kick), 0.03, &mk);
    medianAbsErrorMs(snareT, times(EventType::Snare), 0.03, &ms);
    medianAbsErrorMs(hatT, times(EventType::HiHat), 0.03, &mh);
    CHECK_MSG(mk >= int(kickT.size() * 0.9), "kicks %d/%zu", mk, kickT.size());
    CHECK_MSG(ms >= int(snareT.size() * 0.8), "snares %d/%zu", ms, snareT.size());
    CHECK_MSG(mh >= int(hatT.size() * 0.8), "hats %d/%zu", mh, hatT.size());
    // false positives: kicks should not fire on off-beat hats
    auto kt = times(EventType::Kick);
    int falseKick = 0;
    for (double h : hatT) for (double k : kt) if (std::fabs(k - h) < 0.02) ++falseKick;
    CHECK_MSG(falseKick <= int(hatT.size() / 10), "false kicks on hats: %d", falseKick);
    CHECK_MSG(A.beatsPerBar == 4, "meter %d", A.beatsPerBar);
    // downbeats should align with bar starts (every 4 beats from 0.25s)
    int goodDown = 0;
    for (double d : A.downbeats) {
        double rel = (d - 0.25) / 2.0; // bar = 2 s at 120 bpm
        if (std::fabs(rel - std::round(rel)) < 0.02) ++goodDown;
    }
    CHECK_MSG(goodDown >= int(A.downbeats.size() * 0.9), "downbeats aligned %d/%zu", goodDown, A.downbeats.size());
}

TEST(pitch_tracking_melody) {
    SynthScore s;
    s.duration = 8;
    const float notes[] = {69, 72, 76, 74, 71, 67, 64, 69};
    for (int i = 0; i < 8; ++i) s.notes.push_back({0.5 + i * 0.9, 0.7, notes[i], 1.0f, 0});
    auto buf = render(s);
    auto A = analyze(buf);
    int good = 0;
    for (int i = 0; i < 8; ++i) {
        float conf;
        float p = A.pitchAt(0.5 + i * 0.9 + 0.35, &conf);
        if (std::fabs(p - notes[i]) < 0.5f) ++good;
    }
    CHECK_MSG(good >= 7, "pitch correct for %d/8 notes", good);
    int sustains = 0;
    for (auto& e : A.events) if (e.type == EventType::Sustain) ++sustains;
    CHECK_MSG(sustains >= 6, "sustains %d", sustains);
}

TEST(demo_song_sections_and_speed) {
    auto score = demoSong();
    auto buf = render(score);
    auto t0 = std::chrono::steady_clock::now();
    auto A = analyze(buf);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    demo song: %.1f s audio analysed in %.2f s, bpm %.2f, %zu beats, %zu sections, %zu events\n",
                A.duration, sec, A.mainBpm(), A.beats.size(), A.sections.size(), A.events.size());
    for (auto& sct : A.sections) std::printf("      %-8s %6.2f-%6.2f e=%.2f p=%.2f d=%.1f trend=%+.2f\n", sectionTypeName(sct.type), sct.start, sct.end, sct.energy, sct.power, sct.density, sct.trend);
    // The label at the middle of every ground-truth section must match (verse/chorus interchangeable).
    for (auto& truth : score.sections) {
        double mid = 0.5 * (truth.start + truth.end);
        int si = A.sectionAt(mid);
        REQUIRE(si >= 0);
        std::string got = sectionTypeName(A.sections[size_t(si)].type);
        bool ok = got == truth.label || (truth.label == "verse" && got == "chorus");
        CHECK_MSG(ok, "truth %s at %.1f s labelled %s", truth.label.c_str(), mid, got.c_str());
    }
    CHECK_NEAR(A.mainBpm(), 128, 0.6);
    // Beats on the true grid everywhere, including the drumless break (grid fit + robust BPM).
    {
        int matched = 0;
        double med = medianAbsErrorMs(score.beats, A.beats, 0.035, &matched);
        CHECK_MSG(matched >= int(score.beats.size() * 0.98), "demo beats matched %d of %zu", matched, score.beats.size());
        CHECK_MSG(med < 3.0, "demo median beat error %.2f ms", med);
        for (auto& truth : score.sections) {
            if (truth.label != "break") continue;
            std::vector<double> tb;
            for (double b : score.beats) if (b >= truth.start && b < truth.end) tb.push_back(b);
            int m = 0;
            medianAbsErrorMs(tb, A.beats, 0.035, &m);
            CHECK_MSG(m == int(tb.size()), "break beats matched %d of %zu", m, tb.size());
        }
    }
    // Performance target: 3-minute song in < 20 s. Scale the measured time to 180 s.
    CHECK_MSG(sec * 180.0 / A.duration < 20.0, "analysis would take %.1f s for 3 min", sec * 180.0 / A.duration);
    // Both drops should be found (energy peaks after build-ups)
    int dropsFound = 0;
    for (auto& truth : score.sections) {
        if (truth.label != "drop") continue;
        for (auto& sct : A.sections) {
            if (sct.type == SectionType::Drop && std::fabs(sct.start - truth.start) < 2.0) { ++dropsFound; break; }
        }
    }
    CHECK_MSG(dropsFound == 2, "drops found %d/2", dropsFound);
    // A build-up before at least one drop
    bool build = false;
    for (auto& sct : A.sections) if (sct.type == SectionType::BuildUp) build = true;
    CHECK(build);
    CHECK(A.sections.front().type == SectionType::Intro);
    // section boundaries should land on downbeats (bars of 1.875 s from 0.25)
    for (auto& sct : A.sections) {
        double rel = (sct.start - 0.25) / 1.875;
        CHECK_MSG(sct.start == 0 || std::fabs(rel - std::round(rel)) < 0.03, "section start %.3f not on a bar", sct.start);
    }
}

TEST(wav_roundtrip) {
    auto s = clickTrack(120, 3);
    auto buf = render(s);
    auto wav = encodeWav16(buf);
    AudioBuffer back;
    CHECK(decodeMemory(reinterpret_cast<const uint8_t*>(wav.data()), wav.size(), back) == DecodeError::None);
    CHECK(back.sampleRate == buf.sampleRate);
    CHECK(back.samples.size() == buf.samples.size());
    double err = 0;
    for (size_t i = 0; i < buf.samples.size(); ++i) err = std::max(err, double(std::fabs(back.samples[i] - buf.samples[i])));
    CHECK(err < 1.0 / 16000);
}

TEST(resample_preserves_tone) {
    AudioBuffer b;
    b.sampleRate = 44100;
    b.samples.resize(44100);
    for (size_t i = 0; i < b.samples.size(); ++i) b.samples[i] = float(std::sin(2 * 3.141592653589793 * 440 * double(i) / 44100));
    auto r = resample(b, 22050);
    CHECK(r.samples.size() == 22050);
    double maxErr = 0;
    for (size_t i = 200; i + 200 < r.samples.size(); ++i) {
        double ref = std::sin(2 * 3.141592653589793 * 440 * double(i) / 22050);
        maxErr = std::max(maxErr, std::fabs(r.samples[i] - ref));
    }
    CHECK_MSG(maxErr < 0.02, "resample err %g", maxErr);
}
