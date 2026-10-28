#include <al/audio/synth.hpp>
#include <al/util/rng.hpp>

#include <algorithm>
#include <cmath>

namespace al::audio {

static constexpr double kTau = 6.283185307179586;

static double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

AudioBuffer render(const SynthScore& score, int sr, uint64_t seed) {
    AudioBuffer out;
    out.sampleRate = sr;
    size_t n = size_t(std::ceil(score.duration * sr)) + size_t(sr);
    out.samples.assign(n, 0.0f);
    Rng rng(seed);
    auto& y = out.samples;

    for (auto& d : score.drums) {
        size_t s0 = size_t(std::max(0.0, d.time) * sr);
        switch (d.kind) {
            case Drum::Kick: {
                double phase = 0;
                size_t len = size_t(0.35 * sr);
                for (size_t i = 0; i < len && s0 + i < n; ++i) {
                    double t = double(i) / sr;
                    double f = 45 + 110 * std::exp(-t * 28);
                    phase += kTau * f / sr;
                    double env = std::exp(-t * 9.0) * std::min(1.0, t * sr / 8.0);
                    y[s0 + i] += float(0.9 * d.velocity * env * std::sin(phase));
                }
                break;
            }
            case Drum::Snare:
            case Drum::Clap: {
                size_t len = size_t(0.22 * sr);
                double lp = 0, prev = 0;
                for (size_t i = 0; i < len && s0 + i < n; ++i) {
                    double t = double(i) / sr;
                    double noise = rng.uniform(-1, 1);
                    double hp = noise - prev; // crude high-pass for a crisp noise burst
                    prev = noise;
                    lp += 0.5 * (hp - lp);
                    double env = std::exp(-t * (d.kind == Drum::Clap ? 30.0 : 18.0));
                    double tone = d.kind == Drum::Snare ? 0.35 * std::sin(kTau * 190 * t) * std::exp(-t * 30) : 0.0;
                    y[s0 + i] += float(0.5 * d.velocity * (env * lp * 1.4 + tone));
                }
                break;
            }
            case Drum::HiHat: {
                size_t len = size_t(0.06 * sr);
                double prev = 0, prev2 = 0;
                for (size_t i = 0; i < len && s0 + i < n; ++i) {
                    double t = double(i) / sr;
                    double noise = rng.uniform(-1, 1);
                    double hp = noise - 2 * prev + prev2; // second difference: strong high-pass
                    prev2 = prev;
                    prev = noise;
                    double env = std::exp(-t * 70.0);
                    y[s0 + i] += float(0.22 * d.velocity * env * hp);
                }
                break;
            }
        }
    }

    for (auto& note : score.notes) {
        size_t s0 = size_t(std::max(0.0, note.time) * sr);
        double f = midiHz(note.midi);
        double rel = note.voice == 2 ? 0.3 : 0.06;
        size_t len = size_t((note.duration + rel) * sr);
        double ph = 0;
        for (size_t i = 0; i < len && s0 + i < n; ++i) {
            double t = double(i) / sr;
            double att = note.voice == 2 ? 0.08 : 0.004;
            double env = std::min(1.0, t / att);
            if (t > note.duration) env *= std::exp(-(t - note.duration) / (rel * 0.35));
            else if (note.voice == 0) env *= 0.75 + 0.25 * std::exp(-t * 6);
            ph += kTau * f / sr;
            double s;
            if (note.voice == 0) s = std::sin(ph) + 0.5 * std::sin(2 * ph) + 0.33 * std::sin(3 * ph) + 0.2 * std::sin(4 * ph) + 0.12 * std::sin(5 * ph);
            else if (note.voice == 1) s = std::sin(ph) + 0.3 * std::sin(2 * ph);
            else s = std::sin(ph) + 0.25 * std::sin(2 * ph + 0.3) + 0.15 * std::sin(3 * ph);
            double gain = note.voice == 0 ? 0.16 : (note.voice == 1 ? 0.28 : 0.07);
            y[s0 + i] += float(gain * note.velocity * env * s);
        }
    }
    float peak = 0;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    if (peak > 0.98f) for (float& v : y) v *= 0.98f / peak;
    out.samples.resize(size_t(score.duration * sr));
    return out;
}

SynthScore clickTrack(double bpm, double duration, double offset) {
    SynthScore s;
    s.bpm = bpm;
    s.duration = duration;
    double p = 60.0 / bpm;
    for (double t = offset; t < duration - 0.2; t += p) {
        s.drums.push_back({t, Drum::Clap, 1.0f});
        s.beats.push_back(t);
    }
    return s;
}

SynthScore clickTrackTempoChange(double bpm1, double bpm2, double switchTime, double duration) {
    SynthScore s;
    s.bpm = bpm1;
    s.duration = duration;
    double t = 0.5;
    while (t < duration - 0.2) {
        s.drums.push_back({t, Drum::Clap, 1.0f});
        s.beats.push_back(t);
        t += 60.0 / (t < switchTime ? bpm1 : bpm2);
    }
    return s;
}

SynthScore drumLoop(double bpm, int bars, double offset) {
    SynthScore s;
    s.bpm = bpm;
    double p = 60.0 / bpm;
    s.duration = offset + bars * 4 * p + 0.5;
    const int roots[4] = {45, 41, 48, 43};
    for (int b = 0; b < bars * 4; ++b) {
        double t = offset + b * p;
        s.beats.push_back(t);
        if (b % 4 == 0) s.notes.push_back({t, 4 * p * 0.95, float(roots[(b / 4) % 4]), 0.9f, 1});
        s.drums.push_back({t, Drum::Kick, 1.0f});
        if (b % 2 == 1) s.drums.push_back({t, Drum::Snare, 0.9f});
        s.drums.push_back({t, Drum::HiHat, 0.6f});
        s.drums.push_back({t + p / 2, Drum::HiHat, 0.5f});
    }
    return s;
}

SynthScore demoSong(uint64_t seed) {
    SynthScore s;
    Rng rng(seed);
    s.bpm = 128;
    const double p = 60.0 / s.bpm, bar = 4 * p;
    const double t0 = 0.25;
    // Chord roots (A minor: Am F C G) and a lead motif per bar
    const int roots[4] = {57, 53, 48, 55};
    const int scale[7] = {0, 2, 3, 5, 7, 8, 10}; // natural minor degrees relative to A
    struct Part { const char* label; int bars; int kind; };
    // kind: 0 intro, 1 verse, 2 build, 3 drop, 4 break, 5 outro
    const Part parts[] = {{"intro", 8, 0}, {"verse", 8, 1}, {"buildup", 8, 2}, {"drop", 16, 3},
                          {"break", 8, 4}, {"buildup", 4, 2}, {"drop", 16, 3}, {"outro", 8, 5}};
    // A repeating 2-bar lead motif (8th-note grid positions, scale degrees, lengths in 8ths)
    struct M { int pos; int deg; int len; };
    const M motifA[] = {{0, 4, 2}, {2, 3, 1}, {3, 2, 1}, {4, 0, 3}, {8, 4, 1}, {9, 5, 1}, {10, 4, 2}, {12, 2, 4}};
    const M motifB[] = {{0, 0, 1}, {1, 2, 1}, {2, 4, 2}, {4, 6, 1}, {5, 4, 1}, {6, 3, 2}, {8, 2, 1}, {9, 3, 1}, {10, 4, 6}};
    double t = t0;
    int barIndex = 0;
    for (auto& part : parts) {
        double start = t;
        for (int b = 0; b < part.bars; ++b, ++barIndex) {
            double bt = t + b * bar;
            int root = roots[barIndex % 4];
            for (int q = 0; q < 4; ++q) s.beats.push_back(bt + q * p);
            switch (part.kind) {
                case 0: // intro: pad + soft hats
                    s.notes.push_back({bt, bar * 0.95, float(root + 12), 0.8f, 2});
                    for (int e = 0; e < 8; ++e) s.drums.push_back({bt + e * p / 2, Drum::HiHat, e % 2 ? 0.3f : 0.45f});
                    if (b >= 4) s.drums.push_back({bt, Drum::Kick, 0.6f});
                    break;
                case 1: // verse: kick on 1 and 3, snare 2 and 4, bass, sparse lead
                    for (int q = 0; q < 4; ++q) {
                        if (q % 2 == 0) s.drums.push_back({bt + q * p, Drum::Kick, 0.85f});
                        else s.drums.push_back({bt + q * p, Drum::Snare, 0.75f});
                        s.drums.push_back({bt + q * p + p / 2, Drum::HiHat, 0.4f});
                    }
                    s.notes.push_back({bt, p * 1.9, float(root - 12), 0.8f, 1});
                    s.notes.push_back({bt + 2 * p, p * 1.9, float(root - 12), 0.8f, 1});
                    s.notes.push_back({bt, bar * 0.95, float(root + 12), 0.6f, 2});
                    if (b % 2 == 0) for (auto& m : motifB) if (m.pos < 8) s.notes.push_back({bt + m.pos * p / 2, m.len * p / 2 * 0.9, float(69 + scale[m.deg % 7]), 0.7f, 0});
                    break;
                case 2: { // build-up: accelerating snares, rising pitch, pad
                    int sub = b < part.bars / 2 ? 2 : 4;
                    if (b == part.bars - 1) sub = 8;
                    for (int k = 0; k < 4 * sub; ++k) s.drums.push_back({bt + k * p / sub, Drum::Snare, 0.4f + 0.5f * float(b) / part.bars});
                    s.drums.push_back({bt, Drum::Kick, 0.7f});
                    s.notes.push_back({bt, bar * 0.95, float(root + 12 + b), 0.7f, 2});
                    break;
                }
                case 3: { // drop: four on the floor, claps, driving bass, full lead
                    for (int q = 0; q < 4; ++q) {
                        s.drums.push_back({bt + q * p, Drum::Kick, 1.0f});
                        if (q % 2) s.drums.push_back({bt + q * p, Drum::Clap, 0.9f});
                        s.drums.push_back({bt + q * p + p / 2, Drum::HiHat, 0.6f});
                        s.notes.push_back({bt + q * p + p / 2, p * 0.45, float(root - 12), 1.0f, 1});
                    }
                    const M* mot = (b / 2) % 2 == 0 ? motifA : motifB;
                    size_t cnt = (b / 2) % 2 == 0 ? std::size(motifA) : std::size(motifB);
                    int half = b % 2;
                    for (size_t i = 0; i < cnt; ++i) {
                        auto& m = mot[i];
                        if ((m.pos >= 8) != bool(half)) continue;
                        double nt = bt + (m.pos - half * 8) * p / 2;
                        s.notes.push_back({nt, m.len * p / 2 * 0.9, float(69 + scale[m.deg % 7] + (m.deg >= 7 ? 12 : 0)), 1.0f, 0});
                    }
                    break;
                }
                case 4: // break: pad + long lead notes, no drums
                    s.notes.push_back({bt, bar * 0.98, float(root + 12), 0.8f, 2});
                    s.notes.push_back({bt, bar * 0.9, float(69 + scale[(barIndex * 3) % 7]), 0.6f, 0});
                    break;
                case 5: // outro: fading kicks and pad
                    s.notes.push_back({bt, bar * 0.95, float(root + 12), 0.7f * (1.0f - float(b) / part.bars), 2});
                    if (b < 4) s.drums.push_back({bt, Drum::Kick, 0.6f});
                    for (int e = 0; e < 4; ++e) s.drums.push_back({bt + e * p, Drum::HiHat, 0.25f});
                    break;
            }
        }
        t += part.bars * bar;
        s.sections.push_back({start, t, part.label});
    }
    s.duration = t + 1.0;
    (void)rng;
    return s;
}

} // namespace al::audio
