#include <al/audio/analysis.hpp>
#include <al/audio/fft.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace al::audio {

const char* eventTypeName(EventType t) {
    switch (t) {
        case EventType::Beat: return "beat";
        case EventType::Downbeat: return "downbeat";
        case EventType::Onset: return "onset";
        case EventType::Kick: return "kick";
        case EventType::Snare: return "snare";
        case EventType::HiHat: return "hihat";
        case EventType::Note: return "note";
        case EventType::Sustain: return "sustain";
        case EventType::SectionStart: return "section";
        case EventType::BuildUp: return "buildup";
        case EventType::Drop: return "drop";
        case EventType::TempoChange: return "tempo";
        case EventType::Silence: return "silence";
    }
    return "?";
}

const char* sectionTypeName(SectionType t) {
    switch (t) {
        case SectionType::Intro: return "intro";
        case SectionType::Verse: return "verse";
        case SectionType::Chorus: return "chorus";
        case SectionType::BuildUp: return "buildup";
        case SectionType::Drop: return "drop";
        case SectionType::Break: return "break";
        case SectionType::Outro: return "outro";
    }
    return "?";
}

namespace {

constexpr int kFineN = 1024;
constexpr int kFineHop = 128;
constexpr int kCoarseN = 2048;
constexpr int kCoarseHop = 512;

float percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0.0f;
    size_t k = size_t(std::clamp(p, 0.0, 1.0) * double(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + long(k), v.end());
    return v[k];
}

std::vector<float> movingAverage(const std::vector<float>& x, int half) {
    std::vector<float> out(x.size());
    if (x.empty()) return out;
    std::vector<double> pre(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) pre[i + 1] = pre[i] + x[i];
    for (size_t i = 0; i < x.size(); ++i) {
        size_t a = i >= size_t(half) ? i - size_t(half) : 0;
        size_t b = std::min(x.size(), i + size_t(half) + 1);
        out[i] = float((pre[b] - pre[a]) / double(b - a));
    }
    return out;
}

// Sliding median with a symmetric window (edges use the available samples).
void slidingMedian(const float* in, float* out, size_t n, size_t stride, int half, std::vector<float>& scratch) {
    for (size_t i = 0; i < n; ++i) {
        size_t a = i >= size_t(half) ? i - size_t(half) : 0;
        size_t b = std::min(n, i + size_t(half) + 1);
        scratch.clear();
        for (size_t j = a; j < b; ++j) scratch.push_back(in[j * stride]);
        size_t m = scratch.size() / 2;
        std::nth_element(scratch.begin(), scratch.begin() + long(m), scratch.end());
        out[i * stride] = scratch[m];
    }
}

struct Peak { size_t frame; float value; double refined; };

// Peak picking with an adaptive threshold (local mean + delta, local max, min spacing).
std::vector<Peak> pickPeaks(const std::vector<float>& env, int preMax, int postMax, int preAvg, int postAvg,
                            float delta, int minSpacing, float absMin) {
    std::vector<Peak> peaks;
    const size_t n = env.size();
    std::vector<double> pre(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + env[i];
    long last = -1000000;
    // Parabolic interpolation of the peak position.
    auto refinedPos = [&](size_t i) {
        double y0 = env[i - 1], y1 = env[i], y2 = env[i + 1];
        double den = y0 - 2 * y1 + y2;
        double off = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        return double(i) + std::clamp(off, -0.5, 0.5);
    };
    for (size_t i = 1; i + 1 < n; ++i) {
        float v = env[i];
        if (v < absMin) continue;
        size_t a = i >= size_t(preMax) ? i - size_t(preMax) : 0;
        size_t b = std::min(n, i + size_t(postMax) + 1);
        bool isMax = true;
        for (size_t j = a; j < b; ++j) {
            if (env[j] > v || (env[j] == v && j < i)) { isMax = false; break; }
        }
        if (!isMax) continue;
        size_t aa = i >= size_t(preAvg) ? i - size_t(preAvg) : 0;
        size_t bb = std::min(n, i + size_t(postAvg) + 1);
        double mean = (pre[bb] - pre[aa]) / double(bb - aa);
        if (v < mean + delta) continue;
        if (long(i) - last < minSpacing) {
            if (!peaks.empty() && v > peaks.back().value) { peaks.back() = {i, v, refinedPos(i)}; last = long(i); }
            continue;
        }
        peaks.push_back({i, v, refinedPos(i)});
        last = long(i);
    }
    return peaks;
}

// Onset time refinement in the time domain: steepest rise of the log energy envelope of a
// high-passed signal around t (window +-searchSec). Returns t unchanged if nothing clear.
// Spectral flux with centred frames fires up to ~half a window early, so the search is
// asymmetric: before = 10 ms, after = 30 ms. If found is given it is set to whether an attack
// was found (callers that search around a shifted time must then keep their own time).
double refineOnset(const std::vector<float>& x, int sr, double t, double before = 0.010, double after = 0.030, bool* found = nullptr) {
    const int win = 64, step = 16;
    if (found) *found = false;
    long c = long(t * sr);
    long a = c - long(before * sr) - win, b = c + long(after * sr) + win;
    if (a < 1 || b >= long(x.size())) return t;
    std::vector<double> env;
    std::vector<long> pos;
    for (long s = a; s + win < b; s += step) {
        double e = 0;
        for (long k = s; k < s + win; ++k) {
            double hp = double(x[size_t(k)]) - double(x[size_t(k - 1)]); // first difference = gentle high-pass
            e += hp * hp + 0.25 * double(x[size_t(k)]) * double(x[size_t(k)]);
        }
        env.push_back(std::log(e + 1e-10));
        pos.push_back(s + win); // energy window ends here: the rise marks content starting just before
    }
    if (env.size() < 3) return t;
    double best = 0;
    size_t bi = 0;
    for (size_t i = 1; i < env.size(); ++i) {
        double d = env[i] - env[i - 1];
        if (d > best) { best = d; bi = i; }
    }
    if (best < 0.8) return t; // no clear attack (less than ~2.2x energy jump)
    if (found) *found = true;
    // The window [pos-win, pos) first includes the attack when its end passes it; attack lies within the last step.
    double onsetSample = double(pos[bi]) - step * 0.5;
    return onsetSample / sr;
}

float bpmPrior(double bpm, double pref) {
    double l = std::log2(bpm / pref);
    return float(std::exp(-0.5 * l * l / (0.9 * 0.9)));
}

} // namespace

double MusicAnalysis::mainBpm() const {
    if (tempo.empty()) return 120;
    double best = 0, bpm = tempo.front().bpm;
    for (auto& s : tempo) if (s.end - s.start > best) { best = s.end - s.start; bpm = s.bpm; }
    return bpm;
}

static float sampleCurve(const std::vector<float>& c, double t, double hop) {
    if (c.empty() || hop <= 0) return 0;
    double f = t / hop;
    if (f <= 0) return c.front();
    size_t i = size_t(f);
    if (i + 1 >= c.size()) return c.back();
    double fr = f - double(i);
    return float(c[i] * (1 - fr) + c[i + 1] * fr);
}

float MusicAnalysis::intensityAt(double t) const { return sampleCurve(intensity, t, hop); }
float MusicAnalysis::onsetAt(double t) const { return sampleCurve(onsetEnv, t, hop); }

float MusicAnalysis::pitchAt(double t, float* conf) const {
    if (pitchMidi.empty()) { if (conf) *conf = 0; return 0; }
    size_t i = size_t(std::clamp(t / hopCoarse, 0.0, double(pitchMidi.size() - 1)));
    if (conf) *conf = pitchConf[i];
    return pitchMidi[i];
}

int MusicAnalysis::sectionAt(double t) const {
    for (size_t i = 0; i < sections.size(); ++i) if (t < sections[i].end) return int(i);
    return sections.empty() ? -1 : int(sections.size() - 1);
}

double MusicAnalysis::beatPeriodAt(double t) const {
    if (beats.size() >= 2) {
        auto it = std::upper_bound(beats.begin(), beats.end(), t);
        size_t i = size_t(it - beats.begin());
        if (i == 0) i = 1;
        if (i >= beats.size()) i = beats.size() - 1;
        return beats[i] - beats[i - 1];
    }
    for (auto& s : tempo) if (t < s.end) return 60.0 / s.bpm;
    return 0.5;
}

double MusicAnalysis::snapToGrid(double t, int div) const {
    if (beats.size() < 2 || div <= 0) return t;
    auto it = std::upper_bound(beats.begin(), beats.end(), t);
    size_t i = size_t(it - beats.begin());
    double b0, b1;
    if (i == 0) { b1 = beats[0]; b0 = b1 - (beats[1] - beats[0]); }
    else if (i >= beats.size()) { b0 = beats.back(); b1 = b0 + (beats.back() - beats[beats.size() - 2]); }
    else { b0 = beats[i - 1]; b1 = beats[i]; }
    double step = (b1 - b0) / div;
    double k = std::round((t - b0) / step);
    return b0 + k * step;
}

std::vector<const MusicEvent*> MusicAnalysis::eventsIn(double t0, double t1, EventType type) const {
    std::vector<const MusicEvent*> out;
    auto it = std::lower_bound(events.begin(), events.end(), t0, [](const MusicEvent& e, double t) { return e.time < t; });
    for (; it != events.end() && it->time < t1; ++it) if (it->type == type) out.push_back(&*it);
    return out;
}

MusicAnalysis analyze(const AudioBuffer& input, const AnalysisOptions& opt, const Progress* progress) {
    MusicAnalysis A;
    if (progress) progress->report("resample", 0);
    AudioBuffer audio = input.sampleRate == opt.analysisRate ? input : resample(input, opt.analysisRate);
    const int sr = audio.sampleRate;
    std::vector<float>& x = audio.samples;
    A.duration = audio.duration();
    A.hop = double(kFineHop) / sr;
    A.hopCoarse = double(kCoarseHop) / sr;
    if (x.size() < size_t(kCoarseN * 2)) {
        // Too short to analyse meaningfully: return a single section, no events.
        A.sections.push_back({0, A.duration, SectionType::Verse, 0.5f, 0, 0, 0, 0});
        A.tempo.push_back({0, A.duration, 120});
        return A;
    }
    float peak = 0;
    for (float s : x) peak = std::max(peak, std::fabs(s));
    if (peak > 1e-6f) for (float& s : x) s /= peak;
    checkCancel(progress);

    // ---------------------------------------------------------------- waveform preview
    {
        const size_t bins = 4096;
        A.waveformMin.assign(bins, 0);
        A.waveformMax.assign(bins, 0);
        for (size_t b = 0; b < bins; ++b) {
            size_t a = b * x.size() / bins, e = (b + 1) * x.size() / bins;
            float lo = 0, hi = 0;
            for (size_t i = a; i < e; ++i) { lo = std::min(lo, x[i]); hi = std::max(hi, x[i]); }
            A.waveformMin[b] = lo;
            A.waveformMax[b] = hi;
        }
    }

    // ---------------------------------------------------------------- fine pass (flux per band, RMS)
    if (progress) progress->report("spectrum", 0);
    const size_t nFine = x.size() / kFineHop + 1;
    const double binHzFine = double(sr) / kFineN;
    const int bandEdges[5] = {int(30 / binHzFine), int(150 / binHzFine), int(500 / binHzFine), int(3000 / binHzFine),
                              std::min(kFineN / 2, int(11000 / binHzFine))};
    {
        RealFft fft(kFineN);
        auto win = hannWindow(kFineN);
        std::vector<float> frame(kFineN), mag, logMag(kFineN / 2 + 1, 0.0f), prevLog(kFineN / 2 + 1, 0.0f);
        A.rmsDb.assign(nFine, -120);
        A.onsetEnv.assign(nFine, 0);
        for (auto& b : A.bandOnset) b.assign(nFine, 0);
        for (size_t f = 0; f < nFine; ++f) {
            if ((f & 1023) == 0) { checkCancel(progress); if (progress) progress->report("spectrum", float(f) / nFine * 0.5f); }
            long start = long(f) * kFineHop - kFineN / 2;
            double sumSq = 0;
            for (int i = 0; i < kFineN; ++i) {
                long idx = start + i;
                float s = (idx >= 0 && idx < long(x.size())) ? x[size_t(idx)] : 0.0f;
                frame[size_t(i)] = s * win[size_t(i)];
                if (i >= kFineN / 2 - kFineHop / 2 && i < kFineN / 2 + kFineHop / 2) sumSq += double(s) * s;
            }
            A.rmsDb[f] = float(10.0 * std::log10(sumSq / kFineHop + 1e-12));
            fft.magnitude(frame.data(), mag);
            float total = 0;
            float band[4] = {0, 0, 0, 0};
            for (size_t k = 1; k < mag.size(); ++k) {
                logMag[k] = std::log1p(100.0f * mag[k]);
                float d = logMag[k] - prevLog[k];
                if (d > 0) {
                    total += d;
                    for (int b = 0; b < 4; ++b) if (int(k) >= bandEdges[b] && int(k) < bandEdges[b + 1]) band[b] += d;
                }
            }
            std::swap(prevLog, logMag);
            A.onsetEnv[f] = total;
            for (int b = 0; b < 4; ++b) A.bandOnset[size_t(b)][f] = band[b];
        }
        // Normalize each envelope by its 99th percentile (robust to a few huge hits).
        auto norm = [](std::vector<float>& v) {
            float p = percentile(v, 0.99);
            if (p <= 1e-9f) return;
            for (float& s : v) s = std::min(4.0f, s / p);
        };
        norm(A.onsetEnv);
        for (auto& b : A.bandOnset) norm(b);
    }

    // ---------------------------------------------------------------- coarse pass: HPSS, chroma, pitch
    const size_t nCoarse = x.size() / kCoarseHop + 1;
    const size_t nBins = kCoarseN / 2 + 1;
    const double binHz = double(sr) / kCoarseN;
    std::vector<float> spec(nCoarse * nBins);
    {
        RealFft fft(kCoarseN);
        auto win = hannWindow(kCoarseN);
        std::vector<float> frame(kCoarseN), mag;
        for (size_t f = 0; f < nCoarse; ++f) {
            if ((f & 255) == 0) { checkCancel(progress); if (progress) progress->report("spectrum", 0.5f + 0.5f * float(f) / nCoarse); }
            long start = long(f) * kCoarseHop - kCoarseN / 2;
            for (int i = 0; i < kCoarseN; ++i) {
                long idx = start + i;
                frame[size_t(i)] = ((idx >= 0 && idx < long(x.size())) ? x[size_t(idx)] : 0.0f) * win[size_t(i)];
            }
            fft.magnitude(frame.data(), mag);
            std::copy(mag.begin(), mag.end(), spec.begin() + long(f * nBins));
        }
    }
    if (progress) progress->report("hpss", 0);
    std::vector<float> harm(nCoarse * nBins), perc(nCoarse * nBins);
    std::vector<std::vector<float>> percBandE; // absolute percussive energy: bass / mid / high
    {
        std::vector<float> scratch;
        scratch.reserve(64);
        const size_t maxBin = std::min(nBins, size_t(8000 / binHz)); // nothing useful above 8 kHz for HPSS decisions
        for (size_t k = 0; k < nBins; ++k) {
            if ((k & 63) == 0) { checkCancel(progress); if (progress) progress->report("hpss", 0.5f * float(k) / nBins); }
            if (k >= maxBin) {
                for (size_t f = 0; f < nCoarse; ++f) { harm[f * nBins + k] = 0; perc[f * nBins + k] = spec[f * nBins + k]; }
                continue;
            }
            slidingMedian(&spec[k], &harm[k], nCoarse, nBins, 8, scratch); // across time -> harmonic
        }
        for (size_t f = 0; f < nCoarse; ++f) {
            if ((f & 255) == 0) { checkCancel(progress); if (progress) progress->report("hpss", 0.5f + 0.5f * float(f) / nCoarse); }
            slidingMedian(&spec[f * nBins], &perc[f * nBins], maxBin, 1, 8, scratch); // across frequency -> percussive
        }
        A.percRatio.assign(nCoarse, 0);
        A.bassPercRatio.assign(nCoarse, 0);
        const size_t bassLo = size_t(30 / binHz), bassHi = size_t(150 / binHz) + 1;
        const size_t midLo = size_t(150 / binHz) + 1, midHi = size_t(3000 / binHz) + 1;
        percBandE.assign(3, std::vector<float>(nCoarse, 0.0f));
        for (size_t f = 0; f < nCoarse; ++f) {
            double eh = 0, ep = 0, bh = 0, bp = 0;
            for (size_t k = 0; k < nBins; ++k) {
                size_t i = f * nBins + k;
                float H = harm[i], P = perc[i], S = spec[i];
                float h2 = H * H, p2 = P * P;
                float mh = (h2 + p2) > 1e-20f ? h2 / (h2 + p2) : 0.5f;
                harm[i] = S * mh;
                perc[i] = S * (1 - mh);
                eh += double(harm[i]) * harm[i];
                ep += double(perc[i]) * perc[i];
                if (k >= bassLo && k < bassHi) { bh += double(harm[i]) * harm[i]; bp += double(perc[i]) * perc[i]; }
                double pe = double(perc[i]) * perc[i];
                if (k >= bassLo && k < bassHi) percBandE[0][f] += float(pe);
                else if (k >= midLo && k < midHi) percBandE[1][f] += float(pe);
                else if (k >= midHi) percBandE[2][f] += float(pe);
            }
            A.percRatio[f] = float(ep / (eh + ep + 1e-12));
            A.bassPercRatio[f] = float(bp / (bh + bp + 1e-12));
        }
    }
    spec.clear();
    spec.shrink_to_fit();
    // Normalize each band's percussive energy by a high percentile, on a sqrt (amplitude) scale.
    for (auto& band : percBandE) {
        for (float& v : band) v = std::sqrt(v);
        float p = percentile(band, 0.97);
        if (p > 1e-12f) for (float& v : band) v /= p;
    }

    // Chroma from the harmonic part.
    A.chroma.assign(nCoarse, {});
    {
        std::vector<int> pc(nBins, -1);
        for (size_t k = 1; k < nBins; ++k) {
            double hz = k * binHz;
            if (hz < 60 || hz > 5000) continue;
            double midi = 69 + 12 * std::log2(hz / 440.0);
            pc[k] = int(std::lround(midi)) % 12;
            if (pc[k] < 0) pc[k] += 12;
        }
        for (size_t f = 0; f < nCoarse; ++f) {
            auto& c = A.chroma[f];
            c.fill(0);
            for (size_t k = 1; k < nBins; ++k) if (pc[k] >= 0) c[size_t(pc[k])] += harm[f * nBins + k];
            float m = *std::max_element(c.begin(), c.end());
            if (m > 1e-9f) for (float& v : c) v /= m;
        }
    }

    // Melody pitch: harmonic summation salience + Viterbi smoothing.
    if (progress) progress->report("pitch", 0);
    {
        const float midiLo = 40, midiHi = 88, stepSemi = 0.2f;
        const int nCand = int((midiHi - midiLo) / stepSemi) + 1;
        std::vector<float> sal(nCoarse * size_t(nCand));
        std::vector<float> frameEnergy(nCoarse);
        std::vector<float> m(nBins);
        for (size_t f = 0; f < nCoarse; ++f) {
            if ((f & 255) == 0) { checkCancel(progress); if (progress) progress->report("pitch", 0.6f * float(f) / nCoarse); }
            double e = 0;
            for (size_t k = 0; k < nBins; ++k) { m[k] = std::sqrt(harm[f * nBins + k]); e += double(harm[f * nBins + k]) * harm[f * nBins + k]; }
            frameEnergy[f] = float(e);
            for (int c = 0; c < nCand; ++c) {
                double f0 = 440.0 * std::pow(2.0, (midiLo + c * stepSemi - 69) / 12.0);
                double s = 0, w = 1;
                for (int h = 1; h <= 8; ++h) {
                    double bin = h * f0 / binHz;
                    if (bin >= double(nBins - 1) || h * f0 > 5000) break;
                    size_t b = size_t(bin);
                    double fr = bin - double(b);
                    // take the max of neighbouring bins around the partial to tolerate slight detuning
                    double v = std::max({double(m[b]) * (1 - fr) + double(m[b + 1]) * fr, double(m[b]), double(m[b + 1])});
                    s += w * v;
                    w *= 0.8;
                }
                sal[f * size_t(nCand) + size_t(c)] = float(s);
            }
        }
        // Viterbi over candidates with a pitch-jump penalty (window of +-5 semitones, plus a free jump at higher cost).
        const int win = int(5 / stepSemi);
        std::vector<float> cost(static_cast<size_t>(nCand)), next(static_cast<size_t>(nCand));
        std::vector<int> back(nCoarse * size_t(nCand));
        std::vector<float> salNorm(static_cast<size_t>(nCand));
        auto frameNorm = [&](size_t f) {
            float mx = 1e-9f;
            for (int c = 0; c < nCand; ++c) mx = std::max(mx, sal[f * size_t(nCand) + size_t(c)]);
            for (int c = 0; c < nCand; ++c) salNorm[size_t(c)] = sal[f * size_t(nCand) + size_t(c)] / mx;
        };
        frameNorm(0);
        for (int c = 0; c < nCand; ++c) cost[size_t(c)] = -salNorm[size_t(c)];
        const float jumpCost = 0.6f, perStep = 0.02f;
        for (size_t f = 1; f < nCoarse; ++f) {
            if ((f & 511) == 0) { checkCancel(progress); if (progress) progress->report("pitch", 0.6f + 0.4f * float(f) / nCoarse); }
            frameNorm(f);
            int bestPrev = int(std::min_element(cost.begin(), cost.end()) - cost.begin());
            float bestPrevCost = cost[size_t(bestPrev)];
            for (int c = 0; c < nCand; ++c) {
                float bc = bestPrevCost + jumpCost;
                int bi = bestPrev;
                int lo = std::max(0, c - win), hi = std::min(nCand - 1, c + win);
                for (int p = lo; p <= hi; ++p) {
                    float v = cost[size_t(p)] + perStep * float(std::abs(p - c));
                    if (v < bc) { bc = v; bi = p; }
                }
                next[size_t(c)] = bc - salNorm[size_t(c)];
                back[f * size_t(nCand) + size_t(c)] = bi;
            }
            float mn = *std::min_element(next.begin(), next.end());
            for (int c = 0; c < nCand; ++c) cost[size_t(c)] = next[size_t(c)] - mn; // keep numbers bounded
        }
        std::vector<int> path(nCoarse);
        path[nCoarse - 1] = int(std::min_element(cost.begin(), cost.end()) - cost.begin());
        for (size_t f = nCoarse - 1; f > 0; --f) path[f - 1] = back[f * size_t(nCand) + size_t(path[f])];

        // Voicing: salience of the chosen path relative to the frame's typical salience, and energy gate.
        float eMax = *std::max_element(frameEnergy.begin(), frameEnergy.end());
        A.pitchMidi.assign(nCoarse, 0);
        A.pitchConf.assign(nCoarse, 0);
        std::vector<float> ratio(nCoarse);
        for (size_t f = 0; f < nCoarse; ++f) {
            double mean = 0;
            for (int c = 0; c < nCand; ++c) mean += sal[f * size_t(nCand) + size_t(c)];
            mean /= nCand;
            ratio[f] = float(sal[f * size_t(nCand) + size_t(path[f])] / (mean + 1e-9));
        }
        float rLo = percentile(ratio, 0.1), rHi = percentile(ratio, 0.95);
        for (size_t f = 0; f < nCoarse; ++f) {
            float conf = std::clamp((ratio[f] - rLo) / std::max(1e-6f, rHi - rLo), 0.0f, 1.0f);
            bool loud = frameEnergy[f] > eMax * 1e-4f; // -40 dB
            A.pitchConf[f] = loud ? conf : 0;
            if (loud && conf > 0.35f) A.pitchMidi[f] = midiLo + path[f] * stepSemi;
        }
    }
    harm.clear();
    harm.shrink_to_fit();
    perc.clear();
    perc.shrink_to_fit();

    // ---------------------------------------------------------------- tempo (local autocorrelation)
    if (progress) progress->report("tempo", 0);
    const double hop = A.hop;
    std::vector<float> oenv = A.onsetEnv;
    {
        // remove slow trend so the ACF reflects periodicity, not loudness
        auto avg = movingAverage(oenv, int(0.4 / hop));
        for (size_t i = 0; i < oenv.size(); ++i) oenv[i] = std::max(0.0f, oenv[i] - avg[i]);
    }
    const int lagMin = int(std::floor(60.0 / opt.maxBpm / hop));
    const int lagMax = int(std::ceil(60.0 / opt.minBpm / hop));
    const int winFrames = int(8.0 / hop), winStep = int(2.0 / hop);
    std::vector<std::vector<float>> localAcf;
    std::vector<double> localCenter;
    for (long s = 0; s + 4 < long(nFine); s += winStep) {
        checkCancel(progress);
        long e = std::min<long>(long(nFine), s + winFrames);
        if (e - s < lagMax * 2) { if (!localAcf.empty()) break; e = std::min<long>(long(nFine), s + lagMax * 2 + 2); }
        std::vector<float> acf(static_cast<size_t>(lagMax + 1), 0);
        for (int lag = lagMin / 2; lag <= lagMax; ++lag) {
            double acc = 0;
            for (long i = s; i + lag < e; ++i) acc += double(oenv[size_t(i)]) * oenv[size_t(i + lag)];
            acf[size_t(lag)] = float(acc / double(std::max<long>(1, e - s - lag)));
        }
        localAcf.push_back(std::move(acf));
        localCenter.push_back((s + e) * 0.5 * hop);
        if (e >= long(nFine)) break;
    }
    auto bestLag = [&](const std::vector<float>& acf, double& outLag) {
        double best = -1;
        int bl = lagMin;
        for (int lag = lagMin; lag <= lagMax; ++lag) {
            double bpm = 60.0 / (lag * hop);
            double sc = acf[size_t(lag)];
            if (2 * lag <= lagMax) sc += 0.5 * acf[size_t(2 * lag)];
            if (lag / 2 >= lagMin / 2) sc += 0.25 * acf[size_t(lag / 2)];
            sc *= bpmPrior(bpm, opt.preferredBpm);
            if (sc > best) { best = sc; bl = lag; }
        }
        double y0 = acf[size_t(std::max(1, bl - 1))], y1 = acf[size_t(bl)], y2 = acf[size_t(std::min(lagMax, bl + 1))];
        double den = y0 - 2 * y1 + y2;
        double off = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        outLag = bl + std::clamp(off, -0.5, 0.5);
        return best;
    };
    // Local tempo per window (with the prior), then fold octave errors towards the median.
    // Local tempo per window; windows without clear periodicity (beatless breaks) are marked
    // unreliable (NaN) and inherit the neighbouring tempo instead of creating segments.
    std::vector<double> localBpm;
    std::vector<char> reliableW;
    for (auto& a : localAcf) {
        double lag;
        bestLag(a, lag);
        double mean = 0;
        int cnt = 0;
        for (int l = lagMin; l <= lagMax; ++l) { mean += a[size_t(l)]; ++cnt; }
        mean /= std::max(1, cnt);
        double peak = a[size_t(std::clamp(int(std::lround(lag)), lagMin, lagMax))];
        bool reliable = mean > 1e-9 && peak / mean > 1.6;
        localBpm.push_back(reliable ? 60.0 / (lag * hop) : std::numeric_limits<double>::quiet_NaN());
        reliableW.push_back(reliable ? 1 : 0);
    }
    {
        // fill unreliable windows from the nearest reliable neighbour
        double last = std::numeric_limits<double>::quiet_NaN();
        for (double& b : localBpm) { if (std::isnan(b)) b = last; else last = b; }
        last = std::numeric_limits<double>::quiet_NaN();
        for (size_t i = localBpm.size(); i-- > 0;) { if (std::isnan(localBpm[i])) localBpm[i] = last; else last = localBpm[i]; }
        for (double& b : localBpm) if (std::isnan(b)) b = opt.preferredBpm;
    }
    double globalBpm;
    {
        std::vector<double> tmp = localBpm;
        std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end());
        globalBpm = tmp[tmp.size() / 2];
        for (double& b : localBpm) {
            double best = b, bestErr = 1e9;
            // Only octave errors (x2, /2) are folded: 3:2 factors would turn real tempo changes
            // (e.g. 100 -> 140 BPM) into the wrong tempo.
            for (double m : {1.0, 2.0, 0.5}) {
                double c = b * m;
                double err = std::fabs(std::log2(c / globalBpm)) + (m == 1.0 ? 0.0 : 0.05);
                if (err < bestErr) { bestErr = err; best = c; }
            }
            if (std::fabs(std::log2(best / globalBpm)) < 0.25) b = best;
        }
    }
    {
        // median smoothing over 5 windows
        std::vector<double> sm(localBpm.size());
        for (size_t i = 0; i < localBpm.size(); ++i) {
            std::vector<double> w;
            for (long j = long(i) - 2; j <= long(i) + 2; ++j) if (j >= 0 && j < long(localBpm.size())) w.push_back(localBpm[size_t(j)]);
            std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
            sm[i] = w[w.size() / 2];
        }
        A.tempo.clear();
        TempoSegment cur{0, 0, sm.empty() ? globalBpm : sm[0]};
        int runDiff = 0;
        size_t runStart = 0;
        for (size_t i = 0; i < sm.size(); ++i) {
            // Only windows with clear periodicity count: a lone half-reliable window inside a
            // beatless break (whose tempo the unreliable neighbours inherit) must not open a segment.
            if (!reliableW[i]) continue;
            bool differs = std::fabs(sm[i] - cur.bpm) / cur.bpm > 0.04;
            if (differs) { if (runDiff == 0) runStart = i; ++runDiff; }
            else runDiff = 0;
            if (runDiff >= 3) {
                // the first differing window's centre is ~4 s past where the new tempo became dominant
                double changeT = std::max(0.0, localCenter[runStart] - 2.0);
                if (changeT - cur.start > 6.0) {
                    cur.end = changeT;
                    A.tempo.push_back(cur);
                    cur = {changeT, 0, sm[i]};
                } else {
                    cur.bpm = sm[i];
                }
                runDiff = 0;
            }
        }
        cur.end = A.duration;
        A.tempo.push_back(cur);
        // Precise BPM per segment from the summed ACF of its windows (parabolic peak near the estimate).
        for (auto& seg : A.tempo) {
            std::vector<float> sum(static_cast<size_t>(lagMax + 1), 0);
            for (size_t w = 0; w < localAcf.size(); ++w)
                if (localCenter[w] >= seg.start && localCenter[w] <= seg.end) for (size_t i = 0; i < sum.size(); ++i) sum[i] += localAcf[w][i];
            double target = 60.0 / seg.bpm / hop;
            int lo = std::max(lagMin, int(target * 0.95)), hi = std::min(lagMax - 1, int(target * 1.05) + 1);
            int bl = lo;
            for (int l = lo; l <= hi; ++l) if (sum[size_t(l)] > sum[size_t(bl)]) bl = l;
            if (bl > 0 && sum[size_t(bl)] > 0) {
                double y0 = sum[size_t(bl - 1)], y1 = sum[size_t(bl)], y2 = sum[size_t(bl + 1)];
                double den = y0 - 2 * y1 + y2;
                double off = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
                seg.bpm = 60.0 / ((bl + off) * hop);
            }
        }
        globalBpm = A.mainBpm();
    }

    // ---------------------------------------------------------------- beat tracking (dynamic programming)
    if (progress) progress->report("beats", 0);
    {
        // local score: onset envelope convolved with a short gaussian
        std::vector<float> local(nFine, 0);
        {
            double period = 60.0 / globalBpm / hop;
            int half = std::max(1, int(period / 32));
            std::vector<float> g(static_cast<size_t>(2 * half + 1));
            for (int i = -half; i <= half; ++i) g[size_t(i + half)] = float(std::exp(-0.5 * (i * i) / (half * half / 4.0 + 1e-9)));
            for (size_t i = 0; i < nFine; ++i) {
                double acc = 0;
                for (int k = -half; k <= half; ++k) {
                    long j = long(i) + k;
                    if (j >= 0 && j < long(nFine)) acc += g[size_t(k + half)] * oenv[size_t(j)];
                }
                local[i] = float(acc);
            }
            float sd = 0, mean = 0;
            for (float v : local) mean += v;
            mean /= float(nFine);
            for (float v : local) sd += (v - mean) * (v - mean);
            sd = std::sqrt(sd / float(nFine)) + 1e-9f;
            for (float& v : local) v /= sd;
        }
        auto periodAt = [&](size_t frame) {
            double t = frame * hop;
            for (auto& s : A.tempo) if (t < s.end) return 60.0 / s.bpm / hop;
            return 60.0 / A.tempo.back().bpm / hop;
        };
        const double tightness = 100.0;
        std::vector<double> score(nFine, 0);
        std::vector<long> backp(nFine, -1);
        for (size_t i = 0; i < nFine; ++i) {
            if ((i & 4095) == 0) { checkCancel(progress); if (progress) progress->report("beats", 0.8f * float(i) / nFine); }
            double P = periodAt(i);
            long lo = long(i) - long(std::round(2 * P)), hi = long(i) - long(std::round(P / 2));
            double best = -1e30;
            long bi = -1;
            for (long j = std::max(0L, lo); j <= hi; ++j) {
                double r = double(long(i) - j) / P;
                double v = score[size_t(j)] - tightness * std::log(r) * std::log(r);
                if (v > best) { best = v; bi = j; }
            }
            if (bi >= 0) { score[i] = local[i] + best; backp[i] = bi; }
            else score[i] = local[i];
        }
        // end point: best score in the last period
        double P = periodAt(nFine - 1);
        long start = std::max(0L, long(nFine) - long(std::round(P)) - 1);
        long endIdx = start;
        for (long i = start; i < long(nFine); ++i) if (score[size_t(i)] > score[size_t(endIdx)]) endIdx = i;
        std::vector<long> beatFrames;
        for (long i = endIdx; i >= 0; i = backp[size_t(i)]) beatFrames.push_back(i);
        std::reverse(beatFrames.begin(), beatFrames.end());
        // Extend backwards to the song start at the local period (DP stops where onsets begin).
        while (!beatFrames.empty()) {
            double p = periodAt(size_t(beatFrames.front()));
            long prev = beatFrames.front() - long(std::round(p));
            if (prev < long(0.05 / hop)) break;
            beatFrames.insert(beatFrames.begin(), prev);
        }
        // Refine to onset peaks within +-25 ms with parabolic interpolation.
        const int rad = std::max(1, int(0.025 / hop));
        for (long bf : beatFrames) {
            long bestI = bf;
            float bestV = oenv[size_t(bf)];
            for (long j = std::max(1L, bf - rad); j <= std::min(long(nFine) - 2, bf + rad); ++j)
                if (oenv[size_t(j)] > bestV * 1.2f && oenv[size_t(j)] > 0.15f) { bestV = oenv[size_t(j)]; bestI = j; }
            double pos = double(bestI);
            if (bestI > 0 && bestI + 1 < long(nFine)) {
                double y0 = oenv[size_t(bestI - 1)], y1 = oenv[size_t(bestI)], y2 = oenv[size_t(bestI + 1)];
                double den = y0 - 2 * y1 + y2;
                if (std::fabs(den) > 1e-9 && y1 > 0.1) pos += std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
            }
            double t = pos * hop;
            // Flux peaks sit ~half a hop after the true attack (frame centres); compensate with
            // time-domain refinement when there is a clear attack.
            if (opt.fineOnsets && bestV > 0.15f) t = refineOnset(x, sr, t);
            if (!A.beats.empty() && t <= A.beats.back() + 0.1 * periodAt(size_t(bf)) * hop) continue;
            A.beats.push_back(t);
            A.beatStrength.push_back(std::min(1.0f, bestV));
        }
    }

    // Refine each tempo segment's BPM by robust linear regression of the tracked beat times:
    // fit, drop beats whose residual exceeds 3x the median residual (phase slips in beatless
    // breaks, doubled beats), refit.
    for (auto& seg : A.tempo) {
        std::vector<double> bt;
        for (double b : A.beats) if (b >= seg.start + 0.5 && b < seg.end - 0.5) bt.push_back(b);
        if (bt.size() < 8) continue;
        double expected = 60.0 / seg.bpm;
        std::vector<double> idx(bt.size(), 0.0);
        for (size_t i = 1; i < bt.size(); ++i) idx[i] = idx[i - 1] + std::round((bt[i] - bt[i - 1]) / expected);
        std::vector<char> use(bt.size(), 1);
        double slope = 0;
        bool fitted = false;
        for (int iter = 0; iter < 3; ++iter) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (size_t i = 0; i < bt.size(); ++i)
                if (use[i]) { sx += idx[i]; sy += bt[i]; sxx += idx[i] * idx[i]; sxy += idx[i] * bt[i]; ++n; }
            double den = n * sxx - sx * sx;
            if (n < 8 || den <= 1e-9) break;
            slope = (n * sxy - sx * sy) / den;
            double t0 = (sy - slope * sx) / n;
            fitted = true;
            std::vector<double> res(bt.size());
            for (size_t i = 0; i < bt.size(); ++i) res[i] = std::fabs(bt[i] - (t0 + slope * idx[i]));
            std::vector<double> tmp = res;
            std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end());
            double thr = std::max(3.0 * tmp[tmp.size() / 2], 0.002);
            for (size_t i = 0; i < bt.size(); ++i) use[i] = res[i] <= thr;
        }
        if (fitted && slope > 0 && std::fabs(slope - expected) / expected < 0.03) seg.bpm = 60.0 / slope;
    }

    // ---------------------------------------------------------------- onsets, drums
    if (progress) progress->report("onsets", 0);
    auto toEvents = [&](const std::vector<Peak>& peaks, const std::vector<float>& env, EventType type, Band band, std::vector<MusicEvent>& out) {
        for (auto& p : peaks) {
            double t = p.refined * hop;
            if (opt.fineOnsets) t = refineOnset(x, sr, t);
            MusicEvent e;
            e.time = std::max(0.0, t);
            e.type = type;
            e.strength = std::min(1.0f, env[p.frame]);
            e.band = band;
            out.push_back(e);
        }
    };
    const int ms30 = std::max(1, int(0.030 / hop));
    std::vector<MusicEvent> ev;
    {
        auto pk = pickPeaks(A.onsetEnv, 3, 3, int(0.1 / hop), int(0.07 / hop), 0.06f, ms30 * 2, 0.08f);
        for (auto& p : pk) {
            // band with the largest normalized flux at this frame
            int bb = 0;
            float bv = -1;
            for (int b = 0; b < 4; ++b) {
                float v = A.bandOnset[size_t(b)][p.frame];
                if (v > bv) { bv = v; bb = b; }
            }
            std::vector<Peak> one{p};
            toEvents(one, A.onsetEnv, EventType::Onset, Band(bb), ev);
        }
        checkCancel(progress);
        auto bandMax = [&](int b, size_t f, int r) {
            float m = 0;
            for (long j = long(f) - r; j <= long(f) + r; ++j) if (j >= 0 && j < long(nFine)) m = std::max(m, A.bandOnset[size_t(b)][size_t(j)]);
            return m;
        };
        // Flux peaks fire before the attack and coarse frames are long: look at the next few coarse frames.
        auto percAt = [&](size_t fineFrame) {
            size_t cf = std::min(nCoarse - 1, size_t(double(fineFrame) * hop / A.hopCoarse));
            float m = 0;
            for (size_t c = cf; c < std::min(nCoarse, cf + 3); ++c) m = std::max(m, A.percRatio[c]);
            return m;
        };
        // Kick: bass-band peaks that are percussive and not just a sustained bass note.
        auto kicks = pickPeaks(A.bandOnset[0], 3, 3, int(0.1 / hop), int(0.05 / hop), 0.12f, int(0.09 / hop), 0.2f);
        std::vector<Peak> kk, sn, hh;
        auto bassPercAt = [&](size_t fineFrame) {
            size_t cf = std::min(nCoarse - 1, size_t(double(fineFrame) * hop / A.hopCoarse));
            float m = 0;
            for (size_t c = cf; c < std::min(nCoarse, cf + 3); ++c) m = std::max(m, A.bassPercRatio[c]);
            return m;
        };
        auto bandPercAt = [&](int band, size_t fineFrame) {
            size_t cf = std::min(nCoarse - 1, size_t(double(fineFrame) * hop / A.hopCoarse));
            float m = 0;
            for (size_t c = cf; c < std::min(nCoarse, cf + 3); ++c) m = std::max(m, percBandE[size_t(band)][c]);
            return m;
        };
        (void)bassPercAt;
        for (auto& p : kicks) if (bandPercAt(0, p.frame) > 0.35f) kk.push_back(p);
        toEvents(kk, A.bandOnset[0], EventType::Kick, Band::Bass, ev);
        // Snare: mid-band peaks with simultaneous high-band energy (broadband noise burst).
        auto mids = pickPeaks(A.bandOnset[2], 3, 3, int(0.1 / hop), int(0.05 / hop), 0.1f, int(0.09 / hop), 0.2f);
        for (auto& p : mids) {
            float hi = bandMax(3, p.frame, 2), lm = bandMax(1, p.frame, 2), bs = bandMax(0, p.frame, 1);
            float mid = A.bandOnset[2][p.frame];
            (void)bs;
            // broadband burst: high band present and the mid band comparable to it (hats are mostly high)
            float pm = bandPercAt(1, p.frame), ph = bandPercAt(2, p.frame);
            if (hi > 0.25f && mid >= 0.55f * hi && (lm > 0.15f || mid > 0.4f) && pm > 0.3f && ph > 0.15f) sn.push_back(p);
        }
        toEvents(sn, A.bandOnset[2], EventType::Snare, Band::Mid, ev);
        // Hi-hat: high-band peaks without mid/bass support.
        auto highs = pickPeaks(A.bandOnset[3], 2, 2, int(0.06 / hop), int(0.03 / hop), 0.08f, int(0.05 / hop), 0.15f);
        for (auto& p : highs) {
            float mid = bandMax(2, p.frame, 1), bs = bandMax(0, p.frame, 1), hi = A.bandOnset[3][p.frame];
            float pm = bandPercAt(1, p.frame), ph = bandPercAt(2, p.frame);
            if (mid < hi * 0.85f && ph > 0.15f && pm < 0.6f * std::max(ph, 0.3f)) hh.push_back(p);
        }
        toEvents(hh, A.bandOnset[3], EventType::HiHat, Band::High, ev);
    }

    // ---------------------------------------------------------------- beat phase check
    // The DP tracker can lock onto off-beats when off-beat bass/hats are loud. Kicks and
    // snares/claps mark the beat in nearly all music, so in windows of 8 beats where they
    // sit clearly on the half-beat positions, shift the beats by half a period.
    {
        auto evidence = [&](double t) {
            float s = 0;
            for (auto& e : ev) {
                if (e.time > t + 0.035) break;
                if ((e.type == EventType::Kick || e.type == EventType::Snare) && std::fabs(e.time - t) < 0.035) s += e.strength;
            }
            return s;
        };
        std::sort(ev.begin(), ev.end(), [](const MusicEvent& a, const MusicEvent& b) { return a.time < b.time; });
        size_t nb = A.beats.size();
        std::vector<int> shift(nb, 0);
        const size_t W = 8;
        std::vector<int> winDecision;
        for (size_t i0 = 0; i0 + 1 < nb; i0 += W) {
            float on = 0, off = 0;
            for (size_t i = i0; i < std::min(nb - 1, i0 + W); ++i) {
                on += evidence(A.beats[i]);
                off += evidence(0.5 * (A.beats[i] + A.beats[i + 1]));
            }
            winDecision.push_back(off > 1.4f * on + 0.5f ? 1 : 0);
        }
        // majority over neighbouring windows to avoid flip-flopping
        for (size_t w = 0; w < winDecision.size(); ++w) {
            int votes = 0, total = 0;
            for (long d = -1; d <= 1; ++d) {
                long j = long(w) + d;
                if (j >= 0 && j < long(winDecision.size())) { votes += winDecision[size_t(j)]; ++total; }
            }
            int dec = votes * 2 > total ? 1 : 0;
            for (size_t i = w * W; i < std::min(nb, (w + 1) * W); ++i) shift[i] = dec;
        }
        bool any = false;
        for (int v : shift) any |= v != 0;
        if (any) {
            std::vector<double> nbeats;
            std::vector<float> nstr;
            for (size_t i = 0; i < nb; ++i) {
                double t = A.beats[i];
                if (shift[i]) {
                    double period = i + 1 < nb ? A.beats[i + 1] - A.beats[i] : (i > 0 ? A.beats[i] - A.beats[i - 1] : 0.5);
                    t += 0.5 * period;
                    if (opt.fineOnsets) {
                        bool found = false;
                        double r = refineOnset(x, sr, t - 0.008, 0.010, 0.030, &found);
                        if (found) t = r; // no attack: keep the half-beat position, not the search centre
                    }
                }
                if (!nbeats.empty()) {
                    double p = (i > 0 ? A.beats[i] - A.beats[i - 1] : 0.5);
                    if (t - nbeats.back() < 0.6 * p) continue; // phase jump left a too-short interval
                }
                nbeats.push_back(t);
                size_t f = std::min(nFine - 1, size_t(t / hop));
                nstr.push_back(std::min(1.0f, A.onsetEnv[f]));
            }
            A.beats = std::move(nbeats);
            A.beatStrength = std::move(nstr);
        }
    }

    // ---------------------------------------------------------------- metronomic grid fit
    // Most game music is produced on a fixed grid. Within each tempo segment, fit a robust
    // line t = t0 + k*P through the tracked beats (iteratively dropping outliers). If the fit
    // is tight, replace the beats by the grid (removing local tracking glitches such as
    // snare rolls), then re-refine each grid beat to a nearby onset.
    {
        std::vector<double> out;
        std::vector<float> outStr;
        for (auto& seg : A.tempo) {
            std::vector<double> bt;
            for (double b : A.beats) if (b >= seg.start && b < seg.end) bt.push_back(b);
            if (bt.size() < 16) { for (double b : bt) out.push_back(b); continue; }
            double P = 60.0 / seg.bpm;
            std::vector<double> k(bt.size(), 0.0);
            for (size_t i = 1; i < bt.size(); ++i) k[i] = k[i - 1] + std::max(1.0, std::round((bt[i] - bt[i - 1]) / P));
            std::vector<char> use(bt.size(), 1);
            double t0 = bt[0], slope = P;
            for (int iter = 0; iter < 4; ++iter) {
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                int n = 0;
                for (size_t i = 0; i < bt.size(); ++i) if (use[i]) { sx += k[i]; sy += bt[i]; sxx += k[i] * k[i]; sxy += k[i] * bt[i]; ++n; }
                double den = n * sxx - sx * sx;
                if (n < 8 || den <= 1e-12) break;
                slope = (n * sxy - sx * sy) / den;
                t0 = (sy - slope * sx) / n;
                std::vector<double> res;
                for (size_t i = 0; i < bt.size(); ++i) res.push_back(std::fabs(bt[i] - (t0 + slope * k[i])));
                for (size_t i = 0; i < bt.size(); ++i) use[i] = res[i] < 0.03;
            }
            std::vector<double> res;
            int inl = 0;
            for (size_t i = 0; i < bt.size(); ++i) { double r = std::fabs(bt[i] - (t0 + slope * k[i])); res.push_back(r); if (r < 0.03) ++inl; }
            std::nth_element(res.begin(), res.begin() + long(res.size() / 2), res.end());
            // A very tight fit (median residual < 3 ms) is accepted with fewer inliers: beatless
            // breaks and fills let the tracker drift there, which is exactly what the grid repairs.
            double medRes = res[res.size() / 2];
            bool metronomic = std::fabs(slope - P) / P < 0.02 &&
                              ((medRes < 0.012 && inl >= int(bt.size() * 0.8)) || (medRes < 0.003 && inl >= int(bt.size() * 0.6)));
            if (!metronomic) { for (double b : bt) out.push_back(b); continue; }
            seg.bpm = 60.0 / slope;
            // grid over the whole segment
            double kStart = std::ceil((seg.start - t0) / slope - 0.25);
            for (double kk = kStart;; kk += 1) {
                double t = t0 + kk * slope;
                if (t >= seg.end || t >= A.duration - 0.05) break;
                if (t < 0) continue;
                double r = t;
                if (opt.fineOnsets) {
                    bool found = false;
                    double cand = refineOnset(x, sr, t - 0.004, 0.008, 0.012, &found);
                    if (found && std::fabs(cand - t) < 0.012) r = cand;
                }
                out.push_back(r);
            }
        }
        std::sort(out.begin(), out.end());
        A.beats.clear();
        A.beatStrength.clear();
        for (double b : out) {
            if (!A.beats.empty() && b - A.beats.back() < 0.1) continue;
            A.beats.push_back(b);
            size_t f = std::min(nFine - 1, size_t(b / hop));
            float m = 0;
            for (long j = long(f) - 3; j <= long(f) + 3; ++j) if (j >= 0 && j < long(nFine)) m = std::max(m, A.onsetEnv[size_t(j)]);
            A.beatStrength.push_back(std::min(1.0f, m));
        }
    }

    // ---------------------------------------------------------------- melody notes & sustains
    if (progress) progress->report("notes", 0);
    {
        const double hc = A.hopCoarse;
        size_t f = 0;
        while (f < nCoarse) {
            if (A.pitchMidi[f] <= 0) { ++f; continue; }
            size_t s = f;
            float ref = A.pitchMidi[f];
            double confSum = 0;
            int gap = 0;
            size_t lastVoiced = f;
            std::vector<float> ps;
            while (f < nCoarse) {
                float p = A.pitchMidi[f];
                if (p <= 0) { if (++gap > 2) break; ++f; continue; }
                if (std::fabs(p - ref) > 0.75f) break;
                gap = 0;
                ps.push_back(p);
                confSum += A.pitchConf[f];
                lastVoiced = f;
                if (ps.size() >= 3) { auto tmp = ps; std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end()); ref = tmp[tmp.size() / 2]; }
                ++f;
            }
            if (f == s) ++f;
            size_t len = lastVoiced - s + 1;
            if (len >= 2) {
                MusicEvent e;
                e.time = s * hc;
                e.type = EventType::Note;
                e.pitch = std::round(ref * 5) / 5;
                e.duration = float(len * hc);
                e.strength = float(confSum / double(ps.size()));
                // Snap note start to a nearby fine onset (within 40 ms) for timing accuracy.
                double bestD = 0.04;
                for (auto& o : ev) {
                    if (o.type != EventType::Onset) continue;
                    double d = std::fabs(o.time - e.time);
                    if (d < bestD) { bestD = d; e.time = o.time; }
                }
                ev.push_back(e);
                if (e.duration >= opt.sustainMin) {
                    MusicEvent su = e;
                    su.type = EventType::Sustain;
                    ev.push_back(su);
                }
            }
        }
    }

    // ---------------------------------------------------------------- intensity
    if (progress) progress->report("energy", 0);
    {
        float lo = percentile(A.rmsDb, 0.05), hiDb = percentile(A.rmsDb, 0.97);
        std::vector<float> loud(nFine), dens(nFine, 0);
        for (size_t i = 0; i < nFine; ++i) loud[i] = std::clamp((A.rmsDb[i] - lo) / std::max(1.0f, hiDb - lo), 0.0f, 1.0f);
        for (auto& e : ev) {
            if (e.type != EventType::Onset && e.type != EventType::Kick && e.type != EventType::Snare) continue;
            size_t fi = size_t(e.time / hop);
            if (fi < nFine) dens[fi] += 0.5f + 0.5f * e.strength;
        }
        dens = movingAverage(dens, int(1.0 / hop));
        float dp = percentile(dens, 0.97);
        if (dp > 0) for (float& d : dens) d = std::min(1.0f, d / dp);
        std::vector<float> raw(nFine);
        for (size_t i = 0; i < nFine; ++i) {
            size_t cf = std::min(nCoarse - 1, size_t(double(i) * hop / A.hopCoarse));
            raw[i] = 0.55f * loud[i] + 0.30f * dens[i] + 0.15f * A.percRatio[cf];
        }
        raw = movingAverage(raw, int(0.75 / hop));
        float a = percentile(raw, 0.02), b = percentile(raw, 0.98);
        A.intensity.resize(nFine);
        for (size_t i = 0; i < nFine; ++i) A.intensity[i] = std::clamp((raw[i] - a) / std::max(1e-6f, b - a), 0.0f, 1.0f);
    }

    // ---------------------------------------------------------------- meter & downbeats
    if (progress) progress->report("bars", 0);
    {
        size_t nb = A.beats.size();
        std::vector<float> accent(nb, 0);
        auto nearestStrength = [&](EventType type, double t, double tol) {
            float best = 0;
            for (auto& e : ev) if (e.type == type && std::fabs(e.time - t) < tol) best = std::max(best, e.strength);
            return best;
        };
        std::vector<float> chromaChg(nb, 0);
        for (size_t i = 0; i < nb; ++i) {
            double dt = (i + 1 < nb ? A.beats[i + 1] - A.beats[i] : 0.5) * 0.5;
            size_t c0 = size_t(std::max(0.0, A.beats[i] - dt) / A.hopCoarse), c1 = std::min(nCoarse - 1, size_t((A.beats[i] + dt) / A.hopCoarse));
            float chg = 0;
            if (c0 < nCoarse && c1 < nCoarse) for (int k = 0; k < 12; ++k) chg += std::fabs(A.chroma[c1][size_t(k)] - A.chroma[c0][size_t(k)]);
            chromaChg[i] = chg;
        }
        float chgNorm = std::max(1e-6f, percentile(chromaChg, 0.9));
        for (size_t i = 0; i < nb; ++i) {
            double t = A.beats[i];
            float kick = nearestStrength(EventType::Kick, t, 0.03);
            float snare = nearestStrength(EventType::Snare, t, 0.03);
            float note = 0;
            for (auto& e : ev) if ((e.type == EventType::Note) && std::fabs(e.time - t) < 0.04) note = std::max(note, e.strength);
            // Downbeats: kicks, harmonic changes and new notes; backbeats (2 and 4) carry the snare.
            accent[i] = kick - 0.8f * snare + 0.9f * std::min(1.0f, chromaChg[i] / chgNorm) + 0.3f * note;
        }
        auto phaseScore = [&](int m, int& bestPhase) {
            double best = -1e9;
            for (int ph = 0; ph < m; ++ph) {
                double on = 0, off = 0;
                int non = 0, noff = 0;
                for (size_t i = 0; i < nb; ++i) {
                    if (int((i + size_t(m) - size_t(ph)) % size_t(m)) == 0) { on += accent[i]; ++non; }
                    else { off += accent[i]; ++noff; }
                }
                double sc = (non ? on / non : 0) - (noff ? off / noff : 0);
                if (sc > best) { best = sc; bestPhase = ph; }
            }
            return best;
        };
        int ph4 = 0, ph3 = 0;
        double s4 = phaseScore(4, ph4), s3 = phaseScore(3, ph3);
        int m = opt.beatsPerBar > 0 ? opt.beatsPerBar : ((s3 > s4 * 1.25 && s3 > 0.05) ? 3 : 4);
        int ph = m == 3 ? ph3 : ph4;
        if (opt.beatsPerBar > 0) phaseScore(m, ph);
        A.beatsPerBar = m;
        (void)ph;
        // Viterbi over the position in the bar: advancing by one per beat is free, any other
        // transition (a bar-phase jump, e.g. after a tracking glitch or a 2-beat fill) costs.
        A.beatInBar.assign(nb, 0);
        if (nb > 0) {
            const double jumpPenalty = 2.5;
            std::vector<double> sc(static_cast<size_t>(m), 0.0), nsc(static_cast<size_t>(m), 0.0);
            std::vector<int> bp(nb * size_t(m), 0);
            auto emit = [&](size_t i, int st) { return st == 0 ? double(accent[i]) : (st == m / 2 ? -0.25 * accent[i] : -0.1 * accent[i]); };
            for (int st = 0; st < m; ++st) sc[size_t(st)] = emit(0, st);
            for (size_t i = 1; i < nb; ++i) {
                int bestS = int(std::max_element(sc.begin(), sc.end()) - sc.begin());
                for (int st = 0; st < m; ++st) {
                    int prevNatural = (st + m - 1) % m;
                    double natural = sc[size_t(prevNatural)];
                    double jump = sc[size_t(bestS)] - jumpPenalty;
                    if (natural >= jump) { nsc[size_t(st)] = natural + emit(i, st); bp[i * size_t(m) + size_t(st)] = prevNatural; }
                    else { nsc[size_t(st)] = jump + emit(i, st); bp[i * size_t(m) + size_t(st)] = bestS; }
                }
                double mx = *std::max_element(nsc.begin(), nsc.end());
                for (int st = 0; st < m; ++st) sc[size_t(st)] = nsc[size_t(st)] - mx;
            }
            int st = int(std::max_element(sc.begin(), sc.end()) - sc.begin());
            for (size_t i = nb; i-- > 0;) {
                A.beatInBar[i] = st;
                if (i > 0) st = bp[i * size_t(m) + size_t(st)];
            }
        }
        for (size_t i = 0; i < nb; ++i) if (A.beatInBar[i] == 0) A.downbeats.push_back(A.beats[i]);
    }

    // ---------------------------------------------------------------- sections
    if (progress) progress->report("sections", 0);
    {
        // bar boundaries (fallback: 8-second chunks)
        std::vector<double> bars = A.downbeats;
        if (bars.size() < 4) { bars.clear(); for (double t = 0; t < A.duration; t += 8.0) bars.push_back(t); }
        // A pickup "bar" from 0 s before the first downbeat does not count for the 4-bar phase.
        const size_t pickup = bars.front() > 0.5 ? 1 : 0;
        if (pickup) bars.insert(bars.begin(), 0.0);
        bars.push_back(A.duration);
        const size_t nbar = bars.size() - 1;
        const int D = 17;
        std::vector<std::array<float, D>> feat(nbar);
        std::vector<float> barEnergy(nbar);
        // "Power" = loudness + low-end weight (kick/bass). Unlike the intensity curve it does not
        // reward pure density, so snare-roll build-ups do not look like drops.
        float dbLo = percentile(A.rmsDb, 0.05), dbHi = percentile(A.rmsDb, 0.97);
        auto powerOf = [&](size_t f0, size_t f1) {
            double loud = 0, bass = 0;
            for (size_t f = f0; f < f1 && f < nFine; ++f) {
                loud += std::clamp((A.rmsDb[f] - dbLo) / std::max(1.0f, dbHi - dbLo), 0.0f, 1.0f);
                bass += std::min(1.0f, A.bandOnset[0][f]);
            }
            double n = double(std::max<size_t>(1, f1 - f0));
            return std::pair<double, double>(loud / n, bass / n);
        };
        std::vector<float> barLoud(nbar), barBass(nbar);
        for (size_t b = 0; b < nbar; ++b) {
            size_t f0 = size_t(bars[b] / hop), f1 = std::max(f0 + 1, std::min(nFine, size_t(bars[b + 1] / hop)));
            auto [l, bs] = powerOf(f0, f1);
            barLoud[b] = float(l);
            barBass[b] = float(bs);
        }
        float bassMax = std::max(1e-6f, percentile(barBass, 0.95));
        for (size_t b = 0; b < nbar; ++b) barEnergy[b] = 0.5f * barLoud[b] + 0.5f * std::min(1.0f, barBass[b] / bassMax);
        for (size_t b = 0; b < nbar; ++b) {
            size_t f0 = size_t(bars[b] / hop), f1 = std::max(f0 + 1, std::min(nFine, size_t(bars[b + 1] / hop)));
            double inten = 0, onset = 0, bass = 0, high = 0;
            for (size_t f = f0; f < f1 && f < nFine; ++f) { inten += A.intensity[f]; onset += A.onsetEnv[f]; bass += A.bandOnset[0][f]; high += A.bandOnset[3][f]; }
            double nf = double(f1 - f0);
            size_t c0 = size_t(bars[b] / A.hopCoarse), c1 = std::max(c0 + 1, std::min(nCoarse, size_t(bars[b + 1] / A.hopCoarse)));
            std::array<double, 12> chroma{};
            double pr = 0, pc = 0;
            for (size_t c = c0; c < c1 && c < nCoarse; ++c) { for (int k = 0; k < 12; ++k) chroma[size_t(k)] += A.chroma[c][size_t(k)]; pr += A.percRatio[c]; pc += A.pitchConf[c]; }
            double nc = double(c1 - c0);
            auto& F = feat[b];
            F[0] = float(inten / nf);
            F[1] = float(onset / nf);
            F[2] = float(bass / nf);
            F[3] = float(high / nf);
            F[4] = float(pr / nc);
            for (int k = 0; k < 12; ++k) F[size_t(5 + k)] = float(chroma[size_t(k)] / nc) * 0.35f; // chroma weighted down
        }
        // z-normalize the non-chroma dims
        for (int d = 0; d < 5; ++d) {
            double mean = 0, sd = 0;
            for (auto& F : feat) mean += F[size_t(d)];
            mean /= double(nbar);
            for (auto& F : feat) sd += (F[size_t(d)] - mean) * (F[size_t(d)] - mean);
            sd = std::sqrt(sd / double(nbar)) + 1e-6;
            for (auto& F : feat) F[size_t(d)] = float((F[size_t(d)] - mean) / sd);
        }
        auto dist = [&](size_t i, size_t j) {
            double s = 0;
            for (int d = 0; d < D; ++d) { double v = feat[i][size_t(d)] - feat[j][size_t(d)]; s += v * v; }
            return std::sqrt(s);
        };
        // Foote novelty with a checkerboard kernel of +-K bars.
        const int K = 4;
        std::vector<float> nov(nbar, 0);
        for (size_t b = 1; b < nbar; ++b) {
            double cross = 0, within = 0;
            int nc = 0, nw = 0;
            for (int i = -K; i < 0; ++i) for (int j = 0; j < K; ++j) {
                long a = long(b) + i, c = long(b) + j;
                if (a < 0 || c >= long(nbar)) continue;
                double w = std::exp(-0.5 * ((i + 0.5) * (i + 0.5) + (j + 0.5) * (j + 0.5)) / (K * K * 0.5));
                cross += w * dist(size_t(a), size_t(c)); ++nc;
            }
            for (int i = -K; i < 0; ++i) for (int j = -K; j < 0; ++j) {
                long a = long(b) + i, c = long(b) + j;
                if (a < 0 || c < 0 || i >= j) continue;
                within += dist(size_t(a), size_t(c)); ++nw;
            }
            for (int i = 0; i < K; ++i) for (int j = 0; j < K; ++j) {
                long a = long(b) + i, c = long(b) + j;
                if (a >= long(nbar) || c >= long(nbar) || i >= j) continue;
                within += dist(size_t(a), size_t(c)); ++nw;
            }
            nov[b] = float((nc ? cross / nc : 0) - 0.5 * (nw ? within / nw : 0));
            // energy jumps are strong boundary cues in electronic music
            double eBefore = 0, eAfter = 0;
            int n0 = 0, n1 = 0;
            for (int i = 1; i <= 2; ++i) { if (long(b) - i >= 0) { eBefore += barEnergy[b - size_t(i)]; ++n0; } if (b + size_t(i) - 1 < nbar) { eAfter += barEnergy[b + size_t(i) - 1]; ++n1; } }
            if (n0 && n1) nov[b] += float(4.0 * std::fabs(eAfter / n1 - eBefore / n0));
        }
        // choose boundaries: peaks, min 4 bars apart, prefer multiples of 4 bars
        float nmax = *std::max_element(nov.begin(), nov.end());
        std::vector<size_t> bounds{0};
        std::vector<std::pair<float, size_t>> cand;
        for (size_t b = 2; b + 2 <= nbar; ++b) {
            float v = nov[b];
            if (v < nmax * 0.3f) continue;
            if (v < nov[b - 1] || (b + 1 < nbar && v < nov[b + 1])) continue;
            size_t rb = b - pickup; // b >= 2 > pickup
            float bonus = (rb % 4 == 0) ? 1.15f : (rb % 2 == 0 ? 1.05f : 1.0f);
            cand.push_back({v * bonus, b});
        }
        std::sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::vector<size_t> chosen;
        for (auto& [v, b] : cand) {
            bool ok = b >= 4 && b + 4 <= nbar;
            for (size_t c : chosen) if ((b > c ? b - c : c - b) < 4) ok = false;
            if (ok) chosen.push_back(b);
        }
        std::sort(chosen.begin(), chosen.end());
        // snap to a multiple of 4 bars when one within 2 bars is nearly as novel, keeping >= 4 bars
        // to the neighbouring boundaries (and the song start / end)
        for (size_t ci = 0; ci < chosen.size(); ++ci) {
            size_t& b = chosen[ci];
            if ((b - pickup) % 4 == 0) continue;
            long prevB = ci > 0 ? long(chosen[ci - 1]) : 0, nextB = ci + 1 < chosen.size() ? long(chosen[ci + 1]) : long(nbar);
            for (long d : {-1L, 1L, -2L, 2L}) {
                long nb2 = long(b) + d;
                if (nb2 - prevB < 4 || nextB - nb2 < 4) continue;
                if ((nb2 - long(pickup)) % 4 == 0 && nov[size_t(nb2)] >= 0.75f * nov[b]) { b = size_t(nb2); break; }
            }
        }
        for (size_t b : chosen) if (b != bounds.back()) bounds.push_back(b);
        bounds.push_back(nbar);
        // build sections
        for (size_t i = 0; i + 1 < bounds.size(); ++i) {
            Section s;
            s.firstBar = int(bounds[i]);
            s.barCount = int(bounds[i + 1] - bounds[i]);
            s.start = bars[bounds[i]];
            s.end = bars[bounds[i + 1]];
            size_t f0 = size_t(s.start / hop), f1 = std::min(nFine, size_t(s.end / hop));
            double e = 0;
            for (size_t f = f0; f < f1; ++f) e += A.intensity[f];
            s.energy = float(e / double(std::max<size_t>(1, f1 - f0)));
            int cnt = 0;
            for (auto& o : ev) if (o.type == EventType::Onset && o.time >= s.start && o.time < s.end) ++cnt;
            s.density = float(cnt / std::max(0.1, s.end - s.start));
            // trend: linear regression slope of intensity (per 10 s)
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (size_t f = f0; f < f1; f += 8) { double t = f * hop; sx += t; sy += A.intensity[f]; sxx += t * t; sxy += t * A.intensity[f]; ++n; }
            if (n > 2) s.trend = float((n * sxy - sx * sy) / std::max(1e-9, n * sxx - sx * sx) * 10.0);
            A.sections.push_back(s);
        }
        // ---- section statistics (recomputed after any split/merge)
        auto computeStats = [&](Section& sc) {
            size_t f0 = size_t(sc.start / hop), f1 = std::min(nFine, size_t(sc.end / hop));
            double e = 0;
            for (size_t f = f0; f < f1; ++f) e += A.intensity[f];
            sc.energy = float(e / double(std::max<size_t>(1, f1 - f0)));
            int cnt = 0;
            for (auto& o : ev) if (o.type == EventType::Onset && o.time >= sc.start && o.time < sc.end) ++cnt;
            sc.density = float(cnt / std::max(0.1, sc.end - sc.start));
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (size_t f = f0; f < f1; f += 8) { double t = f * hop; sx += t; sy += A.intensity[f]; sxx += t * t; sxy += t * A.intensity[f]; ++n; }
            sc.trend = n > 2 ? float((n * sxy - sx * sy) / std::max(1e-9, n * sxx - sx * sx) * 10.0) : 0.0f;
            double acc = 0;
            for (int b = sc.firstBar; b < sc.firstBar + sc.barCount; ++b) acc += barEnergy[size_t(b)];
            sc.power = float(acc / std::max(1, sc.barCount));
        };
        // Low-end drive = detected kicks per second (weighted by strength) plus a little bass flux.
        // Kick rate separates drops (kick on every beat) from loud snare-roll build-ups.
        auto bassOf = [&](const Section& sc) {
            double k = 0;
            for (auto& o : ev) if (o.type == EventType::Kick && o.time >= sc.start && o.time < sc.end) k += 0.5 + 0.5 * o.strength;
            double acc = 0;
            for (int b = sc.firstBar; b < sc.firstBar + sc.barCount; ++b) acc += barBass[size_t(b)];
            return k / std::max(0.1, sc.end - sc.start) + 0.2 * acc / std::max(1, sc.barCount);
        };
        for (auto& sc : A.sections) computeStats(sc);

        // ---- labels from power (loudness + low end), bass weight and trend
        const size_t ns = A.sections.size();
        std::vector<float> power, bassN;
        for (auto& sc : A.sections) { power.push_back(sc.power); bassN.push_back(float(bassOf(sc))); }
        float bassTop = std::max(1e-6f, *std::max_element(bassN.begin(), bassN.end()));
        for (float& b : bassN) b /= bassTop;
        float pHi = percentile(power, 0.75), pLo = percentile(power, 0.25);
        float pMax = *std::max_element(power.begin(), power.end());
        std::vector<int> high(ns, 0);
        for (size_t i = 0; i < ns; ++i) high[i] = (power[i] >= std::max(0.5f, pHi - 0.05f) || power[i] >= pMax - 0.06f) && bassN[i] >= 0.55f;
        for (size_t i = 0; i < ns; ++i) {
            auto& sc = A.sections[i];
            bool lowPower = power[i] <= std::min(0.4f, pLo + 0.02f);
            // "Rising" sections that lead into a high section are build-ups even if they are loud
            // (snare rolls / risers): strongly increasing intensity and less low end than what follows.
            bool next = i + 1 < ns && high[i + 1];
            bool rising = sc.trend > 0.25f && next && bassN[i] < bassN[i + 1] * 0.9f;
            if (rising) sc.type = SectionType::BuildUp;
            else if (high[i]) sc.type = SectionType::Chorus; // Drop vs Chorus decided below
            else if (lowPower && sc.density < 6.0f) sc.type = SectionType::Break;
            else sc.type = SectionType::Verse;
        }
        // A section right before a build-up that is itself rising (and not a break) also builds.
        for (size_t i = ns; i-- > 1;) {
            if (A.sections[i].type != SectionType::BuildUp) continue;
            auto& p = A.sections[i - 1];
            if (p.type == SectionType::Verse && p.trend > 0.08f && p.power < A.sections[i].power) p.type = SectionType::BuildUp;
        }
        // Drop = first high section after a build-up / a big power jump; following high sections continue it.
        for (size_t i = 0; i < ns; ++i) {
            auto& sc = A.sections[i];
            if (sc.type != SectionType::Chorus) continue;
            if (i > 0 && A.sections[i - 1].type == SectionType::Drop) { sc.type = SectionType::Drop; continue; }
            float prevP = i > 0 ? power[i - 1] : 0.0f;
            bool afterBuild = i > 0 && A.sections[i - 1].type == SectionType::BuildUp;
            if (afterBuild || power[i] - prevP > 0.2f) sc.type = SectionType::Drop;
        }
        // merge adjacent build-ups
        for (size_t i = 1; i < A.sections.size();) {
            auto& a = A.sections[i - 1];
            auto& b = A.sections[i];
            if (a.type == SectionType::BuildUp && b.type == SectionType::BuildUp) {
                a.end = b.end;
                a.barCount += b.barCount;
                computeStats(a);
                A.sections.erase(A.sections.begin() + long(i));
            } else {
                ++i;
            }
        }
        // long build-ups: only the tail builds (split off the head as a verse)
        for (size_t i = 0; i < A.sections.size(); ++i) {
            auto& p = A.sections[i];
            if (p.type != SectionType::BuildUp || p.barCount <= 16) continue;
            Section head = p;
            int tailBars = 8;
            size_t boundaryBar = size_t(p.firstBar + p.barCount - tailBars);
            head.barCount = p.barCount - tailBars;
            head.end = bars[boundaryBar];
            head.type = SectionType::Verse;
            p.firstBar = int(boundaryBar);
            p.barCount = tailBars;
            p.start = head.end;
            computeStats(head);
            computeStats(p);
            A.sections.insert(A.sections.begin() + long(i), head);
            ++i;
        }
        if (!A.sections.empty()) {
            auto& f = A.sections.front();
            if (f.type != SectionType::Drop && f.energy < 0.6f) f.type = SectionType::Intro;
            auto& l = A.sections.back();
            if (A.sections.size() > 1 && l.type != SectionType::Drop && (l.energy < 0.5f || l.trend < -0.1f)) l.type = SectionType::Outro;
        }
        for (size_t i = 0; i < A.sections.size(); ++i) {
            auto& s = A.sections[i];
            MusicEvent e;
            e.time = s.start;
            e.type = EventType::SectionStart;
            e.index = int(i);
            e.strength = s.energy;
            e.duration = float(s.duration());
            ev.push_back(e);
            if (s.type == SectionType::Drop) { MusicEvent d = e; d.type = EventType::Drop; ev.push_back(d); }
            if (s.type == SectionType::BuildUp) { MusicEvent d = e; d.type = EventType::BuildUp; ev.push_back(d); }
        }
    }

    // ---------------------------------------------------------------- beats, tempo, silence events
    for (size_t i = 0; i < A.beats.size(); ++i) {
        MusicEvent e;
        e.time = A.beats[i];
        e.type = A.beatInBar[i] == 0 ? EventType::Downbeat : EventType::Beat;
        e.strength = A.beatStrength[i];
        e.index = int(i);
        ev.push_back(e);
    }
    for (size_t i = 1; i < A.tempo.size(); ++i) {
        MusicEvent e;
        e.time = A.tempo[i].start;
        e.type = EventType::TempoChange;
        e.index = int(i); // tempo segment: A.tempo[index].bpm is the new tempo
        e.strength = float(std::min(1.0, std::fabs(std::log2(A.tempo[i].bpm / A.tempo[i - 1].bpm)))); // 1 = factor 2
        e.duration = float(60.0 / A.tempo[i].bpm); // new beat period (s)
        ev.push_back(e);
    }
    {
        float thr = std::max(-50.0f, percentile(A.rmsDb, 0.5) - 30.0f);
        size_t i = 0;
        while (i < nFine) {
            if (A.rmsDb[i] >= thr) { ++i; continue; }
            size_t s = i;
            while (i < nFine && A.rmsDb[i] < thr) ++i;
            double len = double(i - s) * hop;
            if (len >= 0.4) {
                MusicEvent e;
                e.time = s * hop;
                e.type = EventType::Silence;
                e.duration = float(len);
                ev.push_back(e);
            }
        }
    }
    std::stable_sort(ev.begin(), ev.end(), [](const MusicEvent& a, const MusicEvent& b) {
        if (a.time != b.time) return a.time < b.time;
        return uint8_t(a.type) < uint8_t(b.type);
    });
    A.events = std::move(ev);
    if (progress) progress->report("done", 1);
    return A;
}

MusicAnalysis trimStart(const MusicAnalysis& A, double start, double* used) {
    MusicAnalysis B = A;
    const double step = A.hopCoarse > 0 ? A.hopCoarse : A.hop;
    if (step <= 0 || start <= 0) {
        if (used) *used = 0;
        return B;
    }
    const size_t nCoarse = size_t(std::floor(std::min(start, A.duration) / step));
    const double s = double(nCoarse) * step;
    if (used) *used = s;
    if (nCoarse == 0) return B;
    const size_t nFine = A.hop > 0 ? size_t(std::lround(s / A.hop)) : 0;
    auto dropFront = [](auto& v, size_t n) { v.erase(v.begin(), v.begin() + long(std::min(n, v.size()))); };
    dropFront(B.rmsDb, nFine);
    dropFront(B.onsetEnv, nFine);
    for (auto& b : B.bandOnset) dropFront(b, nFine);
    dropFront(B.intensity, nFine);
    dropFront(B.percRatio, nCoarse);
    dropFront(B.bassPercRatio, nCoarse);
    dropFront(B.pitchMidi, nCoarse);
    dropFront(B.pitchConf, nCoarse);
    dropFront(B.chroma, nCoarse);
    B.duration = std::max(0.0, A.duration - s);
    // beats (and their per-beat data) before the start go; indices shift
    size_t firstBeat = 0;
    while (firstBeat < A.beats.size() && A.beats[firstBeat] < s) ++firstBeat;
    dropFront(B.beats, firstBeat);
    dropFront(B.beatStrength, firstBeat);
    dropFront(B.beatInBar, firstBeat);
    for (auto& b : B.beats) b -= s;
    size_t firstDown = 0;
    while (firstDown < A.downbeats.size() && A.downbeats[firstDown] < s) ++firstDown;
    dropFront(B.downbeats, firstDown);
    for (auto& d : B.downbeats) d -= s;
    size_t firstTempo = 0;
    while (firstTempo + 1 < A.tempo.size() && A.tempo[firstTempo].end <= s) ++firstTempo;
    dropFront(B.tempo, firstTempo);
    for (auto& t : B.tempo) { t.start = std::max(0.0, t.start - s); t.end -= s; }
    size_t firstSection = 0;
    while (firstSection + 1 < A.sections.size() && A.sections[firstSection].end <= s) ++firstSection;
    dropFront(B.sections, firstSection);
    for (auto& sec : B.sections) {
        sec.start = std::max(0.0, sec.start - s);
        sec.end -= s;
        sec.firstBar = std::max(0, sec.firstBar - int(firstDown));
    }
    B.events.clear();
    for (const auto& e : A.events) {
        if (e.time < s) continue;
        MusicEvent f = e;
        f.time -= s;
        if (f.type == EventType::Beat || f.type == EventType::Downbeat) f.index -= int(firstBeat);
        else if (f.type == EventType::SectionStart) f.index -= int(firstSection);
        else if (f.type == EventType::TempoChange) f.index -= int(firstTempo);
        B.events.push_back(f);
    }
    if (A.duration > 0 && !A.waveformMin.empty()) {
        const size_t bins = A.waveformMin.size();
        const size_t cut = std::min(bins, size_t(double(bins) * s / A.duration));
        dropFront(B.waveformMin, cut);
        dropFront(B.waveformMax, cut);
    }
    return B;
}

bool analyzeFile(const std::string& path, MusicAnalysis& out, std::string& error, const AnalysisOptions& opt, const Progress* progress) {
    AudioBuffer buf;
    if (progress) progress->report("decode", 0);
    auto err = decodeFile(path, buf);
    if (err != DecodeError::None) { error = decodeErrorString(err); return false; }
    out = analyze(buf, opt, progress);
    return true;
}

} // namespace al::audio
