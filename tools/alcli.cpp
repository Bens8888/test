// alcli — command-line front end to the AutoLayout core (analysis, generation, verification,
// benchmarks). Grows with each milestone.
#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/util/codec.hpp>
#include <al/util/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace al;

static int usage() {
    std::printf(
        "usage:\n"
        "  alcli analyze <audio> [--json out.json]   analyse a song and print the timeline summary\n"
        "  alcli demo-song <out.wav>                 render the bundled procedural demo song\n");
    return 2;
}

static Json analysisToJson(const audio::MusicAnalysis& A) {
    Json j = Json::object();
    j["duration"] = A.duration;
    j["bpm"] = A.mainBpm();
    j["beatsPerBar"] = A.beatsPerBar;
    Json tempo = Json::array();
    for (auto& t : A.tempo) { Json s; s["start"] = t.start; s["end"] = t.end; s["bpm"] = t.bpm; tempo.push(s); }
    j["tempo"] = tempo;
    Json secs = Json::array();
    for (auto& s : A.sections) {
        Json o;
        o["type"] = audio::sectionTypeName(s.type);
        o["start"] = s.start; o["end"] = s.end; o["energy"] = s.energy; o["density"] = s.density; o["trend"] = s.trend;
        secs.push(o);
    }
    j["sections"] = secs;
    Json ev = Json::array();
    for (auto& e : A.events) {
        Json o;
        o["t"] = std::round(e.time * 10000) / 10000;
        o["type"] = audio::eventTypeName(e.type);
        o["s"] = std::round(e.strength * 1000) / 1000;
        if (e.pitch > 0) o["pitch"] = e.pitch;
        if (e.duration > 0) o["dur"] = std::round(e.duration * 1000) / 1000;
        ev.push(o);
    }
    j["events"] = ev;
    return j;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    if (cmd == "analyze" && argc >= 3) {
        audio::MusicAnalysis A;
        std::string err;
        auto t0 = std::chrono::steady_clock::now();
        Progress prog;
        if (!audio::analyzeFile(argv[2], A, err, {}, &prog)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("duration %.2f s  bpm %.2f  meter %d/4  beats %zu  events %zu  (%.2f s)\n", A.duration, A.mainBpm(), A.beatsPerBar,
                    A.beats.size(), A.events.size(), sec);
        for (auto& s : A.sections)
            std::printf("  %-8s %7.2f - %7.2f  energy %.2f  density %.1f/s  trend %+.2f\n", audio::sectionTypeName(s.type), s.start, s.end, s.energy, s.density, s.trend);
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--json") == 0) writeFile(argv[i + 1], analysisToJson(A).dump(1));
        }
        return 0;
    }
    if (cmd == "demo-song" && argc >= 3) {
        auto buf = audio::render(audio::demoSong());
        if (!writeFile(argv[2], audio::encodeWav16(buf))) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
        std::printf("wrote %s (%.1f s)\n", argv[2], buf.duration());
        return 0;
    }
    return usage();
}
