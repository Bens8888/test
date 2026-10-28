// aldata: training data tools.
//   aldata vocab <out.json>                        pattern vocabulary (token -> id, mode, tags) for training
//   aldata selfplay --out f.jsonl [--songs N] [--variants K] [--keep 0.3] [--seed S] [--threads T] [--model m.alm]
//       random songs x random settings, K generated variants each, the best fraction by reward kept
//   aldata extract <level.gmd> <song> --out f.jsonl [--style N] [--difficulty D]
//   aldata extract-dir <dir> --out f.jsonl          every <name>.gmd with a song next to it (see training/RETRAINING.md)
#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/data/dataset.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/profile.hpp>
#include <al/model/model.hpp>
#include <al/util/codec.hpp>
#include <al/util/json.hpp>
#include <al/util/rng.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace al;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  aldata vocab <out.json>\n"
                 "  aldata selfplay --out f.jsonl [--songs N] [--variants K] [--keep F] [--seed S] [--threads T] [--model m.alm]\n"
                 "  aldata extract <level.gmd> <song> --out f.jsonl [--style N] [--difficulty D]\n"
                 "  aldata extract-dir <dir> --out f.jsonl\n"
                 "  aldata profile <dir|level.gmd> --out profile.json   style profile for generator v2 (no song needed)\n");
    return 2;
}

std::map<std::string, std::string> flags(int argc, char** argv, int from) {
    std::map<std::string, std::string> f;
    for (int i = from; i + 1 < argc; i += 2) f[argv[i]] = argv[i + 1];
    return f;
}

double num(const std::map<std::string, std::string>& f, const char* k, double def) {
    auto it = f.find(k);
    return it == f.end() ? def : std::stod(it->second);
}

// Difficulty from a folder name (easy, normal, hard, harder, insane, easy_demon ... extreme_demon).
float difficultyFromName(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, float> names[] = {{"extreme", 9}, {"insane_demon", 8}, {"insane demon", 8}, {"hard_demon", 7}, {"hard demon", 7},
                                                         {"medium", 6}, {"easy_demon", 5}, {"easy demon", 5}, {"insane", 4}, {"harder", 3},
                                                         {"hard", 2}, {"normal", 1}, {"easy", 0}};
    for (auto& [n, d] : names) if (s.find(n) != std::string::npos) return d;
    return -1;
}

int styleFromName(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 1; i < int(gen::Style::Count); ++i) {
        std::string n = gen::styleName(gen::Style(i));
        for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (s == n) return i;
    }
    return 0;
}

int cmdVocab(const std::string& out) {
    Json arr = Json::array();
    const auto& L = gen::patternLibrary();
    for (size_t i = 0; i < L.size(); ++i) {
        Json o = Json::object();
        o["token"] = int(i);
        o["id"] = L[i].id;
        o["mode"] = int(L[i].mode);
        o["tags"] = double(L[i].tags);
        o["minDiff"] = L[i].minDiff;
        o["complexity"] = L[i].complexity;
        arr.push(std::move(o));
    }
    Json j = Json::object();
    j["features"] = gen::kContextFeatures;
    j["styles"] = int(gen::Style::Count);
    j["vocab"] = std::move(arr);
    if (!writeFile(out, j.dump(1))) return 1;
    std::printf("wrote %s (%zu tokens)\n", out.c_str(), L.size());
    return 0;
}

int cmdSelfplay(const std::map<std::string, std::string>& f) {
    auto outIt = f.find("--out");
    if (outIt == f.end()) return usage();
    const std::string out = outIt->second;
    const int songs = int(num(f, "--songs", 20));
    const int variants = int(num(f, "--variants", 6));
    const double keep = num(f, "--keep", 0.34);
    const uint64_t seed = uint64_t(num(f, "--seed", 1));
    int threads = int(num(f, "--threads", 0));
    if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
    std::unique_ptr<model::PatternModel> mdl;
    if (auto it = f.find("--model"); it != f.end()) {
        std::string err;
        mdl = model::PatternModel::load(it->second, &err);
        if (!mdl) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        std::printf("using model %s\n", mdl->name().c_str());
    }
    std::mutex io;
    std::atomic<int> next{0}, sequences{0}, levels{0}, unbeatable{0};
    double rewardSum = 0, keptRewardSum = 0, dropSum = 0, alignSum = 0; // under `io`
    auto t0 = std::chrono::steady_clock::now();
    auto worker = [&]() {
        for (;;) {
            int i = next.fetch_add(1);
            if (i >= songs) return;
            Rng rng(combineSeed(seed, uint64_t(i)));
            auto score = audio::randomSong(combineSeed(seed * 7919, uint64_t(i)));
            auto A = audio::analyze(audio::render(score));
            gen::GenOptions opt;
            opt.engine = 1; // training data for the v1 pattern model
            opt.difficulty = gen::Difficulty(rng.below(10));
            opt.style = gen::Style(1 + rng.below(uint32_t(gen::Style::Count) - 1));
            opt.density = rng.chance(0.5) ? -1.0f : float(rng.uniform(0.2, 0.9));
            opt.speedChanges = gen::SpeedChanges(rng.below(4));
            std::vector<std::pair<float, data::Sequence>> scored;
            for (int v = 0; v < variants; ++v) {
                uint64_t vs = rng.next();
                opt.seed = vs;
                auto r = gen::generateVariant(A, opt, vs, mdl.get());
                ++levels;
                if (!r.report.beatable || !r.report.intendedInputs) {
                    ++unbeatable;
                    std::lock_guard<std::mutex> lk(io);
                    std::printf("  FAILED VERIFICATION: song seed %llu, variant seed %llu, difficulty %d, style %d, density %.2f, speed changes %d: %s\n",
                                (unsigned long long)combineSeed(seed * 7919, uint64_t(i)), (unsigned long long)vs, int(opt.difficulty), int(opt.style),
                                double(opt.density), int(opt.speedChanges), r.report.notes.empty() ? "" : r.report.notes.back().c_str());
                    continue;
                }
                auto seq = data::fromGenerated(r, opt);
                seq.source = "selfplay:" + std::to_string(i);
                {
                    std::lock_guard<std::mutex> lk(io);
                    rewardSum += seq.reward;
                    dropSum += double(r.report.dropped) / std::max(1, r.report.clicks);
                    alignSum += r.report.musicAlignment;
                }
                scored.push_back({seq.reward, std::move(seq)});
            }
            std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
            size_t n = std::max<size_t>(1, size_t(std::ceil(keep * double(scored.size()))));
            std::vector<data::Sequence> kept;
            for (size_t k = 0; k < std::min(n, scored.size()); ++k) kept.push_back(std::move(scored[k].second));
            std::lock_guard<std::mutex> lk(io);
            data::appendJsonl(out, kept);
            sequences += int(kept.size());
            for (auto& k : kept) keptRewardSum += k.reward;
            std::printf("  song %d (%.0f bpm, %.0f s, %s, difficulty %d): kept %zu of %zu, best reward %.2f\n", i, score.bpm, A.duration,
                        gen::styleName(opt.style), int(opt.difficulty), kept.size(), scored.size(), kept.empty() ? 0.0 : double(kept[0].reward));
            std::fflush(stdout);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int good = levels.load() - unbeatable.load();
    std::printf("self-play: %d songs, %d levels generated (%d failed verification), %d sequences kept -> %s (%.1f s)\n", songs, levels.load(),
                unbeatable.load(), sequences.load(), out.c_str(), sec);
    std::printf("SUMMARY {\"levels\": %d, \"failed\": %d, \"mean_reward\": %.4f, \"mean_kept_reward\": %.4f, \"mean_dropped\": %.4f, \"mean_alignment\": %.4f, \"seconds\": %.1f}\n",
                levels.load(), unbeatable.load(), rewardSum / std::max(1, good), keptRewardSum / std::max(1, sequences.load()), dropSum / std::max(1, good),
                alignSum / std::max(1, good), sec);
    return 0;
}

bool extractOne(const std::string& gmdPath, const std::string& songPath, int style, float difficulty, const std::string& out) {
    std::string xml;
    if (!readFile(gmdPath, xml)) { std::printf("  %s: cannot read\n", gmdPath.c_str()); return false; }
    std::string err;
    auto lvl = level::Level::fromGmd(xml, &err);
    if (!lvl) { std::printf("  %s: not a level file (%s)\n", gmdPath.c_str(), err.c_str()); return false; }
    audio::MusicAnalysis A;
    if (!audio::analyzeFile(songPath, A, err)) { std::printf("  %s: song %s: %s\n", gmdPath.c_str(), songPath.c_str(), err.c_str()); return false; }
    data::ExtractOptions eo;
    eo.style = style;
    eo.difficulty = difficulty;
    data::ExtractReport rep;
    auto seq = data::fromLevel(*lvl, A, eo, rep);
    std::printf("  %s: %s\n", fs::path(gmdPath).filename().string().c_str(), rep.summary().c_str());
    if (!rep.ok) return false;
    return data::appendJsonl(out, {seq});
}

std::string findSong(const fs::path& gmd) {
    for (const char* ext : {".mp3", ".ogg", ".wav", ".MP3", ".OGG", ".WAV"}) {
        fs::path p = gmd;
        p.replace_extension(ext);
        if (fs::exists(p)) return p.string();
    }
    return {};
}

int cmdExtractDir(const std::string& dir, const std::string& out) {
    int ok = 0, total = 0;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".gmd") continue;
        ++total;
        std::string song = findSong(e.path());
        if (song.empty()) { std::printf("  %s: no song file next to it (same name, .mp3/.ogg/.wav)\n", e.path().filename().string().c_str()); continue; }
        // labels: <name>.json {"style": "flow", "difficulty": 7}, else the folder names
        float diff = -1;
        int style = 0;
        fs::path meta = e.path();
        meta.replace_extension(".json");
        std::string text;
        if (fs::exists(meta) && readFile(meta.string(), text)) {
            if (auto j = Json::parse(text)) {
                diff = float(j->get("difficulty", -1.0));
                style = styleFromName(j->get("style", ""));
            }
        }
        for (auto p = e.path().parent_path(); diff < 0 && !p.empty() && p != fs::path(dir).parent_path(); p = p.parent_path()) diff = difficultyFromName(p.filename().string());
        if (extractOne(e.path().string(), song, style, diff, out)) ++ok;
    }
    std::printf("extracted %d of %d levels -> %s\n", ok, total, out.c_str());
    return 0;
}

// Style profile (generator v2) of every .gmd under `path` (or of one level).
int cmdProfile(const std::string& path, const std::string& out) {
    gen::StyleProfile total;
    int ok = 0, n = 0;
    std::vector<fs::path> files;
    if (fs::is_directory(path)) {
        for (auto& e : fs::recursive_directory_iterator(path)) if (e.is_regular_file() && e.path().extension() == ".gmd") files.push_back(e.path());
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(path);
    }
    for (auto& f : files) {
        ++n;
        std::string text, err;
        if (!readFile(f.string(), text)) { std::printf("  %s: cannot read\n", f.filename().string().c_str()); continue; }
        auto lvl = level::Level::fromGmd(text, &err);
        if (!lvl) { std::printf("  %s: not a level (%s)\n", f.filename().string().c_str(), err.c_str()); continue; }
        gen::StyleProfile p;
        std::string why;
        auto t0 = std::chrono::steady_clock::now();
        if (!gen::profileFromLevel(*lvl, p, why)) { std::printf("  %s: skipped, %s\n", f.filename().string().c_str(), why.c_str()); continue; }
        total.add(p);
        ++ok;
        std::printf("  %s: %s (%.1f s)\n", f.filename().string().c_str(), p.summary().c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    if (!writeFile(out, total.toJson().dump(1))) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    std::printf("profile of %d of %d levels: %s -> %s\n", ok, n, total.summary().c_str(), out.c_str());
    return ok > 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    if (cmd == "vocab" && argc >= 3) return cmdVocab(argv[2]);
    if (cmd == "selfplay") return cmdSelfplay(flags(argc, argv, 2));
    if (cmd == "extract" && argc >= 4) {
        auto f = flags(argc, argv, 4);
        if (!f.count("--out")) return usage();
        return extractOne(argv[2], argv[3], int(num(f, "--style", 0)), float(num(f, "--difficulty", -1)), f["--out"]) ? 0 : 1;
    }
    if (cmd == "extract-dir" && argc >= 3) {
        auto f = flags(argc, argv, 3);
        if (!f.count("--out")) return usage();
        return cmdExtractDir(argv[2], f["--out"]);
    }
    if (cmd == "profile" && argc >= 3) {
        auto f = flags(argc, argv, 3);
        if (!f.count("--out")) return usage();
        return cmdProfile(argv[2], f["--out"]);
    }
    return usage();
}
