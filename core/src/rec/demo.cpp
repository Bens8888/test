#include <al/rec/demo.hpp>

#include <al/util/rng.hpp>

#include <algorithm>
#include <cmath>

namespace al::rec {

Recording demoRecording(const audio::SynthScore& song, uint64_t seed, double latency, double jitter) {
    Rng rng(seed);
    struct Tap { double t; double len; };
    std::vector<Tap> taps;
    auto sectionAt = [&](double t) -> std::string {
        for (auto& s : song.sections) if (t >= s.start && t < s.end) return s.label;
        return "";
    };
    const double beat = 60.0 / song.bpm;
    for (auto& d : song.drums) {
        std::string sec = sectionAt(d.time);
        bool take = false;
        if (sec == "intro") take = d.kind == audio::Drum::Kick && std::fmod(std::round((d.time - 0.25) / beat), 2.0) == 0.0;
        else if (sec == "verse" || sec == "buildup") take = d.kind == audio::Drum::Kick || d.kind == audio::Drum::Snare;
        else if (sec == "drop") take = d.kind == audio::Drum::Kick || d.kind == audio::Drum::Snare || d.kind == audio::Drum::Clap;
        else if (sec == "outro") take = d.kind == audio::Drum::Kick && std::fmod(std::round((d.time - 0.25) / beat), 4.0) == 0.0;
        if (take) taps.push_back({d.time, 0.06});
    }
    for (auto& n : song.notes) {
        if (n.voice != 0 || sectionAt(n.time) != "break") continue;
        taps.push_back({n.time, n.duration > 0.3 ? n.duration * 0.8 : 0.06});
    }
    std::sort(taps.begin(), taps.end(), [](const Tap& a, const Tap& b) { return a.t < b.t; });
    Recording r;
    r.levelName = "AutoLayout demo";
    r.source = "recorded";
    r.completed = true;
    r.duration = song.duration;
    double last = -1;
    for (size_t i = 0; i < taps.size(); ++i) {
        double t = taps[i].t + latency + rng.gauss() * jitter;
        if (t - last < 0.16) continue; // a human does not double-tap faster than this here
        double next = i + 1 < taps.size() ? taps[i + 1].t + latency : 1e9;
        double rel = t + std::min(taps[i].len + rng.uniform(0.0, 0.02), std::max(0.03, next - t - 0.05));
        for (int k = 0; k < 2; ++k) {
            RecEvent e;
            e.time = k == 0 ? t : rel;
            e.tick = int32_t(std::lround(e.time * 240.0));
            e.press = k == 0;
            e.p1.x = float(e.time * 311.58);
            e.p1.y = 15;
            e.p1.mode = 0;
            e.p1.speed = 1;
            e.p1.onGround = k == 0;
            r.events.push_back(e);
        }
        last = t;
    }
    return r;
}

} // namespace al::rec
