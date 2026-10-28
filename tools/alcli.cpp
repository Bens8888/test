// alcli — command-line front end to the AutoLayout core (analysis, generation, verification,
// benchmarks). Grows with each milestone.
#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/gen/fusion.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/test_pattern.hpp>
#include <al/model/model.hpp>
#include <al/rec/demo.hpp>
#include <al/rec/recording.hpp>
#include <al/rec/validate.hpp>
#include <al/report/render.hpp>
#include <al/util/codec.hpp>
#include <al/util/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

using namespace al;

static int usage() {
    std::printf(
        "usage:\n"
        "  alcli analyze <audio> [--json out.json]   analyse a song and print the timeline summary\n"
        "  alcli demo-song <out.wav>                 render the bundled procedural demo song\n"
        "  alcli demo-recording <out.alrec|.json>    write the demo click recording\n"
        "  alcli bench [model.alm] [out.json]        performance benchmark (analysis, generation, physics, bot, model)\n"
        "  alcli validate <level.gmd> <recording> [yOffset=90]  simulator vs a run recorded in GD (position error per input)\n"
        "  alcli rec-info <recording> [out.json]     inspect / convert a recording or macro (.alrec, .json, .gdr, frames)\n"
        "  alcli render <level.gmd|piece-test> <out.png> [x0] [x1]   draw a level (outline silhouettes, orbs, pads, portals)\n"
        "  alcli generate <audio|demo> [--seed N] [--difficulty 0-9] [--variants N] [--style N]\n"
        "               [--variant-seed N] [--modes 0,4,...] [--recording file|demo] [--faith 0-1]\n"
        "               [--density 0-1] [--speed-changes 0-3]\n"
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
        o["power"] = s.power; o["firstBar"] = s.firstBar; o["barCount"] = s.barCount;
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
        // full-precision copies (used by the Python parity tests in training/)
        o["time"] = e.time; o["strength"] = e.strength; o["band"] = int(e.band); o["index"] = e.index;
        if (e.duration > 0) o["duration"] = e.duration;
        ev.push(o);
    }
    j["events"] = ev;
    // Per-frame curves and beat grid (full precision) for parity checks against training/autolayout/audio.py.
    auto arr = [](const auto& v) { Json a = Json::array(); for (auto x : v) a.push(x); return a; };
    j["hop"] = A.hop;
    j["hopCoarse"] = A.hopCoarse;
    Json curves = Json::object();
    curves["rmsDb"] = arr(A.rmsDb);
    curves["onsetEnv"] = arr(A.onsetEnv);
    Json bands = Json::array();
    for (auto& b : A.bandOnset) bands.push(arr(b));
    curves["bandOnset"] = bands;
    curves["intensity"] = arr(A.intensity);
    curves["percRatio"] = arr(A.percRatio);
    curves["bassPercRatio"] = arr(A.bassPercRatio);
    curves["pitchMidi"] = arr(A.pitchMidi);
    curves["pitchConf"] = arr(A.pitchConf);
    Json chroma = Json::array();
    for (auto& c : A.chroma) chroma.push(arr(c));
    curves["chroma"] = chroma;
    curves["waveformMin"] = arr(A.waveformMin);
    curves["waveformMax"] = arr(A.waveformMax);
    j["curves"] = curves;
    j["beats"] = arr(A.beats);
    j["beatStrength"] = arr(A.beatStrength);
    j["beatInBar"] = arr(A.beatInBar);
    j["downbeats"] = arr(A.downbeats);
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
    if (cmd == "validate" && argc >= 4) {
        // Simulator vs the real game: replay a recording made in GD (it stores the player's state
        // at every input) in the simulator on the same level and compare.
        std::string xml, bytes, err;
        if (!readFile(argv[2], xml) || !readFile(argv[3], bytes)) { std::fprintf(stderr, "cannot read the files\n"); return 1; }
        auto lvl = level::Level::fromGmd(xml, &err);
        if (!lvl) lvl = level::Level::fromLevelString(xml, &err);
        if (!lvl) { std::fprintf(stderr, "level: %s\n", err.c_str()); return 1; }
        auto rec = rec::importAny(bytes, &err);
        if (!rec) { std::fprintf(stderr, "recording: %s\n", err.c_str()); return 1; }
        const float yOffset = argc >= 5 ? std::stof(argv[4]) : 90.f; // GD node y = level-string y + 90
        auto v = rec::validate(*lvl, *rec, yOffset);
        std::printf("simulator %s the level (%d ticks%s)\n", v.simFinishes ? "finishes" : "does NOT finish", v.simTicks,
                    v.simDeath.empty() ? "" : (", dies: " + v.simDeath).c_str());
        if (v.compared == 0) { std::printf("the recording has no player states to compare (record it with the mod)\n"); return v.simFinishes ? 0 : 1; }
        std::printf("%d input events compared: x error mean %.3f max %.3f units, y error mean %.3f max %.3f units, gamemode mismatches %d\n",
                    v.compared, v.meanDx, v.maxDx, v.meanDy, v.maxDy, v.modeMismatches);
        if (v.firstDeviationTick >= 0) std::printf("first deviation above 2 units at tick %d (%.3f s)\n", v.firstDeviationTick, v.firstDeviationTick / 240.0);
        return v.simFinishes ? 0 : 1;
    }
    if (cmd == "bench") {
        // Performance targets: analysis < ~20 s for a 3-minute song, < ~10 s per variant,
        // thousands of rollouts per second, model inference in microseconds.
        using clk = std::chrono::steady_clock;
        auto secs = [](clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); };
        std::string modelPath = argc >= 3 ? argv[2] : "resources/models/model_all.alm";
        Json out = Json::object();
        // 1. audio analysis: the demo song and a ~3 minute random song
        auto demo = audio::render(audio::demoSong());
        auto t0 = clk::now();
        auto A = audio::analyze(demo);
        double tDemo = secs(t0);
        audio::SynthScore longSong;
        for (uint64_t s = 1; s < 200; ++s) { longSong = audio::randomSong(s); if (longSong.duration > 170) break; }
        auto longBuf = audio::render(longSong);
        t0 = clk::now();
        auto A2 = audio::analyze(longBuf);
        double tLong = secs(t0);
        std::printf("analysis: demo song %.1f s audio in %.2f s; %.1f s song in %.2f s (%.2f s per minute of audio)\n", A.duration, tDemo, A2.duration, tLong,
                    tLong / (A2.duration / 60.0));
        out["analysis_demo_seconds"] = tDemo;
        out["analysis_long_audio_seconds"] = A2.duration;
        out["analysis_long_seconds"] = tLong;
        // 2. generation
        gen::GenOptions opt;
        opt.difficulty = gen::Difficulty::Insane;
        std::vector<double> per;
        for (uint64_t s = 1; s <= 5; ++s) {
            t0 = clk::now();
            auto r = gen::generateVariant(A2, opt, s, nullptr);
            per.push_back(secs(t0));
        }
        std::sort(per.begin(), per.end());
        opt.variants = 5;
        t0 = clk::now();
        auto vs = gen::generateVariants(A2, opt, nullptr);
        double tPar = secs(t0);
        std::printf("generation (%.0f s song, Insane): per variant median %.2f s, max %.2f s; 5 variants in parallel %.2f s\n", A2.duration, per[2], per.back(), tPar);
        out["variant_median_seconds"] = per[2];
        out["variant_max_seconds"] = per.back();
        out["five_variants_parallel_seconds"] = tPar;
        // 3. physics and bot
        const auto& best = vs.front();
        phys::World W(best.level);
        t0 = clk::now();
        long steps = 0;
        for (int rep = 0; rep < 5; ++rep) { auto rr = bot::run(W, best.inputs); steps += rr.ticks; }
        double tRun = secs(t0);
        std::printf("physics: %.1f M steps/s (%.0f full-level rollouts/s of a %.0f s level)\n", steps / tRun / 1e6, 5.0 / tRun, A2.duration);
        out["physics_msteps_per_second"] = steps / tRun / 1e6;
        phys::GameState start = phys::initialState(W);
        bot::InputTrack none;
        t0 = clk::now();
        int rollouts = 0;
        while (secs(t0) < 1.0) { bot::RunOptions ro; ro.maxTicks = 1200; bot::runFrom(W, start, best.inputs, ro); ++rollouts; }
        std::printf("bot: %d five-second rollouts per second\n", rollouts);
        out["five_second_rollouts_per_second"] = rollouts;
        // 4. model inference
        std::string err;
        auto m = model::PatternModel::load(modelPath, &err);
        if (m) {
            gen::PatternContext ctx;
            std::vector<gen::HistoryStep> hist(16);
            for (auto& h : hist) { std::fill(h.f, h.f + gen::kContextFeatures, 0.3f); h.token = 3; }
            ctx.history = &hist;
            auto cands = gen::patternsFor(level::Mode::Cube, 5);
            std::vector<float> w;
            t0 = clk::now();
            int n = 0;
            while (secs(t0) < 0.5) { m->score(ctx, cands, w); ++n; }
            double us = secs(t0) / n * 1e6;
            std::printf("model: %zu parameters, %.0f us per decision (17-step GRU window)\n", m->parameterCount(), us);
            out["model_parameters"] = double(m->parameterCount());
            out["model_us_per_decision"] = us;
            gen::GenOptions mo;
            mo.difficulty = gen::Difficulty::Insane;
            t0 = clk::now();
            auto rm = gen::generateVariant(A2, mo, 1, m.get());
            double tm = secs(t0);
            std::printf("generation with the model: %.2f s for one variant (%s)\n", tm, rm.report.intendedInputs ? "beaten with the designed clicks" : "needed the bot");
            out["variant_with_model_seconds"] = tm;
        } else {
            std::printf("model: not measured (%s)\n", err.c_str());
        }
        if (argc >= 4) writeFile(argv[3], out.dump(1));
        return 0;
    }
    if (cmd == "demo-recording" && argc >= 3) {
        auto r = rec::demoRecording(audio::demoSong());
        std::string path = argv[2];
        bool json = path.size() > 5 && path.substr(path.size() - 5) == ".json";
        if (!writeFile(path, json ? r.toJson().dump(1) : r.serialize())) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
        std::printf("wrote %s (%zu events)\n", argv[2], r.events.size());
        return 0;
    }
    if (cmd == "rec-info" && argc >= 3) {
        std::string bytes, err;
        if (!readFile(argv[2], bytes)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        auto r = rec::importAny(bytes, &err);
        if (!r) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        std::printf("%s: source %s, level '%s' (%d), %zu events, %zu presses, %.2f s, %s\n", argv[2], r->source.c_str(), r->levelName.c_str(), r->levelId,
                    r->events.size(), r->pressTimes().size(), r->duration, r->completed ? "completed" : "incomplete");
        if (argc >= 4) { writeFile(argv[3], r->toJson().dump(1)); std::printf("wrote %s\n", argv[3]); }
        return 0;
    }
    if (cmd == "demo-song" && argc >= 3) {
        auto buf = audio::render(audio::demoSong());
        if (!writeFile(argv[2], audio::encodeWav16(buf))) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
        std::printf("wrote %s (%.1f s)\n", argv[2], buf.duration());
        return 0;
    }
    if (cmd == "render" && argc >= 4) {
        level::Level L;
        if (std::string(argv[2]) == "piece-test") {
            L.objects = gen::makePieceTest(0);
        } else {
            std::string text, err;
            if (!readFile(argv[2], text)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
            auto lvl = level::Level::fromGmd(text, &err);
            if (!lvl) { std::fprintf(stderr, "cannot parse %s: %s\n", argv[2], err.c_str()); return 1; }
            L = std::move(*lvl);
        }
        report::RenderOptions ro;
        ro.scale = 1.0f;
        ro.maxWidth = 2400;
        ro.drawPath = false;
        if (argc >= 5) ro.x0 = float(std::atof(argv[4]));
        if (argc >= 6) ro.x1 = float(std::atof(argv[5]));
        writeFile(argv[3], report::renderPng(L, nullptr, ro));
        std::printf("wrote %s\n", argv[3]);
        return 0;
    }
    if (cmd == "generate" && argc >= 3) {
        audio::MusicAnalysis A;
        std::string src = argv[2];
        auto t0 = std::chrono::steady_clock::now();
        if (src == "demo") {
            A = audio::analyze(audio::render(audio::demoSong()));
        } else if (src.rfind("random:", 0) == 0) {
            A = audio::analyze(audio::render(audio::randomSong(std::stoull(src.substr(7)))));
        } else {
            std::string err;
            if (!audio::analyzeFile(src, A, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        }
        double tAnalyze = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        gen::GenOptions opt;
        opt.variants = 1;
        std::string out, reportPath, inputsPath, pngPath;
        bool exactSeed = false;
        std::string recordingPath;
        for (int i = 3; i + 1 < argc; i += 2) {
            std::string k = argv[i], v = argv[i + 1];
            if (k == "--seed") opt.seed = std::stoull(v);
            else if (k == "--recording") recordingPath = v;
            else if (k == "--density") opt.density = std::stof(v);
            else if (k == "--speed-changes") opt.speedChanges = gen::SpeedChanges(std::stoi(v));
            else if (k == "--faith") opt.faithfulness = std::stof(v);
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
        std::optional<gen::DesignPlan> recPlan;
        if (!recordingPath.empty()) {
            std::string bytes, err;
            std::optional<rec::Recording> r;
            if (recordingPath == "demo") r = rec::demoRecording(audio::demoSong());
            else if (readFile(recordingPath, bytes)) r = rec::importAny(bytes, &err);
            else err = "cannot read " + recordingPath;
            if (!r) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
            auto F = gen::fuseClicks(*r, A);
            std::printf("recording: %s\n", F.summary().c_str());
            opt.input = gen::InputSource::Recording;
            Rng prng(opt.seed);
            recPlan = gen::planFromRecording(F, A, opt, prng);
        }
        const gen::DesignPlan* planPtr = recPlan ? &*recPlan : nullptr;
        if (exactSeed) results.push_back(gen::generateVariant(A, opt, opt.seed, nullptr, nullptr, planPtr));
        else results = gen::generateVariants(A, opt, nullptr, nullptr, 0, planPtr);
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
