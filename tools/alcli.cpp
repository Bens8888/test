// alcli — command-line front end to the AutoLayout core (analysis, generation, verification,
// benchmarks). Grows with each milestone.
#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/gen/generator.hpp>
#include <al/report/render.hpp>
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
        "  alcli demo-song <out.wav>                 render the bundled procedural demo song\n"
        "  alcli generate <audio|demo> [--seed N] [--difficulty 0-9] [--variants N] [--style N]\n"
        "               [--variant-seed N] [--modes 0,4,...]\n"
        "               [--out level.gmd] [--report report.json] [--inputs inputs.txt] [--png preview.png]\n");
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
    if (cmd == "generate" && argc >= 3) {
        audio::MusicAnalysis A;
        std::string src = argv[2];
        auto t0 = std::chrono::steady_clock::now();
        if (src == "demo") {
            A = audio::analyze(audio::render(audio::demoSong()));
        } else {
            std::string err;
            if (!audio::analyzeFile(src, A, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        }
        double tAnalyze = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        gen::GenOptions opt;
        opt.variants = 1;
        std::string out, reportPath, inputsPath, pngPath;
        bool exactSeed = false;
        for (int i = 3; i + 1 < argc; i += 2) {
            std::string k = argv[i], v = argv[i + 1];
            if (k == "--seed") opt.seed = std::stoull(v);
            else if (k == "--variant-seed") { opt.seed = std::stoull(v); exactSeed = true; } // seed of one variant itself
            else if (k == "--difficulty") opt.difficulty = gen::Difficulty(std::stoi(v));
            else if (k == "--variants") opt.variants = std::stoi(v);
            else if (k == "--style") opt.style = gen::Style(std::stoi(v));
            else if (k == "--out") out = v;
            else if (k == "--report") reportPath = v;
            else if (k == "--inputs") inputsPath = v;
            else if (k == "--png") pngPath = v;
            else if (k == "--modes") {
                // comma-separated gamemode indices to allow (0 cube,1 ship,2 ball,3 ufo,4 wave,5 robot,6 spider,7 swing)
                opt.modeWeights.fill(0.0f);
                size_t pos = 0;
                while (pos < v.size()) {
                    size_t e = v.find(',', pos);
                    int m = std::stoi(v.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
                    if (m >= 0 && m < 8) opt.modeWeights[size_t(m)] = 1.0f;
                    if (e == std::string::npos) break;
                    pos = e + 1;
                }
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        std::vector<gen::GenResult> results;
        if (exactSeed) results.push_back(gen::generateVariant(A, opt, opt.seed, nullptr));
        else results = gen::generateVariants(A, opt, nullptr);
        double tGen = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        if (results.empty()) { std::fprintf(stderr, "generation failed\n"); return 1; }
        std::printf("analysis %.2f s, %d variant(s) in %.2f s\n", tAnalyze, int(results.size()), tGen);
        for (auto& r : results) std::printf("  seed %llu: score %.2f, %.2f s, %s\n", (unsigned long long)r.seed, r.score, r.seconds, r.report.beatable ? "beatable" : "NOT beatable");
        const auto& best = results.front();
        std::printf("\nplan (%s, base difficulty %.1f):\n", gen::styleName(best.plan.style), best.plan.baseDifficulty);
        for (auto& p : best.plan.parts) std::printf("  %6.2f-%6.2f  %s  [%zu clicks]\n", p.t0, p.t1, p.why.c_str(), p.clicks.size());
        std::printf("\n%s", best.report.toText().c_str());
        if (!out.empty()) {
            level::Level L = best.level;
            L.name = "AutoLayout";
            writeFile(out, L.toGmd());
            std::printf("wrote %s\n", out.c_str());
        }
        if (!reportPath.empty()) writeFile(reportPath, best.report.toJson().dump(1));
        if (!pngPath.empty()) {
            report::RenderOptions ro;
            ro.scale = 0.25f;
            ro.maxWidth = 2400;
            writeFile(pngPath, report::renderPng(best.level, &best.inputs, ro));
            std::printf("wrote %s\n", pngPath.c_str());
        }
        if (!inputsPath.empty()) {
            std::string t;
            for (auto& e : best.inputs.events) t += std::to_string(e.tick) + (e.press ? " down\n" : " up\n");
            writeFile(inputsPath, t);
        }
        return 0;
    }
    return usage();
}
