#include "Session.hpp"

#include "../recorder/LiveRecorder.hpp"
#include "../recorder/TapAlong.hpp"

#include <Geode/binding/LevelTools.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>

#include <al/util/json.hpp>

#include <chrono>
#include <thread>

using namespace geode::prelude;

namespace al::mod {

char const* difficultyLabel(gen::Difficulty d) {
    switch (d) {
        case gen::Difficulty::Easy: return "Easy";
        case gen::Difficulty::Normal: return "Normal";
        case gen::Difficulty::Hard: return "Hard";
        case gen::Difficulty::Harder: return "Harder";
        case gen::Difficulty::Insane: return "Insane";
        case gen::Difficulty::EasyDemon: return "Easy Demon";
        case gen::Difficulty::MediumDemon: return "Medium Demon";
        case gen::Difficulty::HardDemon: return "Hard Demon";
        case gen::Difficulty::InsaneDemon: return "Insane Demon";
        case gen::Difficulty::ExtremeDemon: return "Extreme Demon";
        default: return "Auto (AI decides)";
    }
}

char const* roleLabel(gen::Role r) { return gen::roleName(r); }

Session& Session::get() {
    static Session s;
    return s;
}

Session::Session() {
    loadOptions();
    reloadModels();
    loadProfile();
}

// ---------------------------------------------------------------- options

void Session::saveOptions() {
    Mod::get()->setSavedValue<std::string>("options", m_opt.toJson().dump());
    Mod::get()->setSavedValue<bool>("use-model", m_useModel);
    Mod::get()->setSavedValue<bool>("use-profile", m_useProfile);
}

void Session::loadOptions() {
    auto text = Mod::get()->getSavedValue<std::string>("options", "");
    if (!text.empty()) {
        if (auto j = Json::parse(text)) m_opt = gen::GenOptions::fromJson(*j);
    }
    m_useModel = Mod::get()->getSavedValue<bool>("use-model", true);
    m_useProfile = Mod::get()->getSavedValue<bool>("use-profile", true);
}

std::filesystem::path Session::presetDir() const {
    auto dir = Mod::get()->getSaveDir() / "presets";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

geode::Result<> Session::savePreset(std::string const& name) {
    if (name.empty()) return Err("give the preset a name");
    std::string clean;
    for (char c : name) clean += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    gen::GenOptions o = m_opt;
    o.seed = 1; // a preset is a style, not a particular layout
    return file::writeString(presetDir() / (clean + ".json"), o.toJson().dump(1));
}

geode::Result<> Session::loadPreset(std::string const& name) {
    auto text = file::readString(presetDir() / (name + ".json"));
    if (!text) return Err(text.unwrapErr());
    auto j = Json::parse(text.unwrap());
    if (!j) return Err("the preset file is not valid JSON");
    uint64_t seed = m_opt.seed;
    int variants = m_opt.variants;
    m_opt = gen::GenOptions::fromJson(*j);
    m_opt.seed = seed;
    m_opt.variants = variants;
    saveOptions();
    return Ok();
}

std::vector<std::string> Session::presets() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(presetDir(), ec))
        if (e.path().extension() == ".json") out.push_back(geode::utils::string::pathToString(e.path().stem()));
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- song

geode::Result<SongChoice> Session::levelSong(GJGameLevel* level) const {
    if (!level) return Err("no level");
    SongChoice c;
    if (level->m_songID > 0) {
        auto mdm = MusicDownloadManager::sharedState();
        if (!mdm->isSongDownloaded(level->m_songID))
            return Err(fmt::format("song {} is not downloaded yet: open the level's song settings and download it", level->m_songID));
        c.path = std::filesystem::path(std::string(mdm->pathForSong(level->m_songID)));
        c.label = fmt::format("Level song #{}", level->m_songID);
    } else {
        auto rel = LevelTools::getAudioFileName(level->m_audioTrack);
        c.path = std::filesystem::path(std::string(CCFileUtils::get()->fullPathForFilename(rel.c_str(), false)));
        c.label = fmt::format("Official song: {}", std::string(rel));
    }
    std::error_code ec;
    if (!std::filesystem::exists(c.path, ec)) return Err("the song file was not found: " + geode::utils::string::pathToString(c.path));
    return Ok(c);
}

void Session::setCustomSong(std::filesystem::path const& p) {
    SongChoice c;
    c.path = p;
    c.custom = true;
    c.label = "File: " + geode::utils::string::pathToString(p.filename());
    m_customSong = c;
}

geode::Result<SongChoice> Session::song(GJGameLevel* level) const {
    if (m_customSong) return Ok(*m_customSong);
    return levelSong(level);
}

std::string Session::cacheKey(std::filesystem::path const& p) const {
    std::error_code ec;
    auto size = std::filesystem::file_size(p, ec);
    auto time = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
    return geode::utils::string::pathToString(p) + "|" + std::to_string(static_cast<unsigned long long>(size)) + "|" + std::to_string(static_cast<long long>(time));
}

std::shared_ptr<const audio::MusicAnalysis> Session::cachedAnalysis(std::filesystem::path const& p) const {
    auto key = cacheKey(p);
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    for (auto& e : m_cache) if (e.key == key) return e.analysis;
    return nullptr;
}

void Session::storeAnalysis(std::string const& key, std::shared_ptr<const audio::MusicAnalysis> a) {
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    for (auto& e : m_cache) if (e.key == key) { e.analysis = a; return; }
    m_cache.push_back({key, std::move(a)});
    if (m_cache.size() > 4) m_cache.erase(m_cache.begin());
}

// ---------------------------------------------------------------- jobs

namespace {

void setStage(Job& j, std::string const& s, float p) {
    std::lock_guard<std::mutex> lk(j.mutex);
    j.stage = s;
    j.progress = p;
}

void fail(Job& j, std::string const& e) {
    std::lock_guard<std::mutex> lk(j.mutex);
    j.error = e;
}

} // namespace

std::shared_ptr<Job> Session::startGenerate(GJGameLevel* level) {
    if (busy()) return m_job;
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Generate;
    m_job = job;
    auto songRes = song(level);
    if (songRes.isErr()) {
        fail(*job, songRes.unwrapErr());
        job->done = true;
        return job;
    }
    SongChoice sc = songRes.unwrap();
    // inputs captured by value: the worker never touches GD objects or the session's mutable state
    gen::GenOptions opt = m_opt;
    if (useProfile()) opt.profile = m_profile;
    if (opt.variants < 1) opt.variants = 1;
    std::optional<rec::Recording> recording;
    if (opt.input == gen::InputSource::Taps) {
        if (auto r = TapAlong::get().recordingFor(LevelEditorLayer::get())) recording = std::move(*r);
        else {
            fail(*job, "tap-along mode: no taps yet. Press Tap along..., then Record and tap to the song");
            job->done = true;
            return job;
        }
    } else if (opt.input == gen::InputSource::Recording) {
        if (auto& last = LiveRecorder::get().last()) recording = *last;
        else {
            fail(*job, "click-recording mode: record a run first (pause menu > Rec, or the hotkey), or load a recording");
            job->done = true;
            return job;
        }
    }
    const model::ModelSet* models = useModel() ? &m_models : nullptr;
    int threads = std::max<int>(0, int(Mod::get()->getSettingValue<int64_t>("analysis-threads")));
    std::string key = cacheKey(sc.path);
    auto cached = cachedAnalysis(sc.path);
    std::thread([job, sc, opt, recording, models, threads, key, cached]() {
        auto t0 = std::chrono::steady_clock::now();
        Progress prog;
        prog.cancelFlag = &job->cancel;
        try {
            std::shared_ptr<const audio::MusicAnalysis> A = cached;
            if (!A) {
                setStage(*job, "Analysing the song", 0.f);
                prog.onProgress = [&](std::string const& stage, float f) { setStage(*job, "Analysing: " + stage, 0.35f * f); };
                auto a = std::make_shared<audio::MusicAnalysis>();
                std::string err;
                if (!audio::analyzeFile(geode::utils::string::pathToString(sc.path), *a, err, {}, &prog)) {
                    if (job->cancel) throw Cancelled{};
                    fail(*job, "song analysis failed: " + err);
                    job->done = true;
                    return;
                }
                A = a;
                Session::get().storeAnalysis(key, A);
            }
            std::optional<gen::DesignPlan> plan;
            if (recording) {
                // clicks that start late in the song: the level starts a little before the first
                // one (its song offset) instead of with a long empty run
                rec::Recording clicks = *recording;
                double songStart = 0;
                if (double start = gen::levelStartFor(clicks); start > 0) {
                    A = std::make_shared<const audio::MusicAnalysis>(audio::trimStart(*A, start, &songStart));
                    clicks = gen::shiftRecording(clicks, songStart);
                }
                setStage(*job, "Fusing your clicks with the music", 0.36f);
                auto F = gen::fuseClicks(clicks, *A);
                job->fusion = F;
                Rng rng(opt.seed);
                plan = gen::planFromRecording(F, *A, opt, rng);
                plan->songStart = songStart;
            }
            job->analysis = A;
            prog.onProgress = [&](std::string const& stage, float f) {
                static char const* names[] = {"plan", "build", "verify"};
                std::string s = stage;
                for (auto n : names) if (stage == n) s = std::string(n);
                setStage(*job, "Generating " + std::to_string(opt.variants) + " variant(s): " + s, 0.38f + 0.62f * f);
            };
            job->results = gen::generateVariants(*A, opt, models, &prog, threads, plan ? &*plan : nullptr);
            if (job->results.empty()) fail(*job, "no variant could be generated");
        } catch (Cancelled const&) {
            fail(*job, "cancelled");
        } catch (std::exception const& e) {
            fail(*job, std::string("error: ") + e.what());
        }
        job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        setStage(*job, "Done", 1.f);
        job->done = true;
    }).detach();
    return job;
}

std::shared_ptr<Job> Session::startReroll(int partIndex) {
    if (busy()) return m_job;
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Reroll;
    m_job = job;
    auto cur = current();
    if (!cur || !m_analysis) {
        fail(*job, "generate a layout first");
        job->done = true;
        return job;
    }
    gen::GenResult base = *cur;
    auto A = m_analysis;
    gen::GenOptions opt = m_opt;
    if (useProfile()) opt.profile = m_profile;
    const model::ModelSet* models = useModel() ? &m_models : nullptr;
    uint64_t seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    std::thread([job, base, A, opt, models, partIndex, seed]() {
        auto t0 = std::chrono::steady_clock::now();
        try {
            setStage(*job, fmt::format("Re-rolling section {}", partIndex + 1), 0.2f);
            job->analysis = A;
            if (partIndex < 0) {
                gen::GenOptions o = opt;
                o.seed = seed;
                o.variants = 1;
                job->results.push_back(gen::generateVariant(*A, o, seed, models));
            } else {
                job->results.push_back(gen::rerollPart(base, partIndex, *A, opt, seed, models));
            }
        } catch (std::exception const& e) {
            fail(*job, std::string("error: ") + e.what());
        }
        job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        setStage(*job, "Done", 1.f);
        job->done = true;
    }).detach();
    return job;
}

std::shared_ptr<Job> Session::startPlaytest(std::string levelString) {
    if (busy()) return m_job;
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Playtest;
    m_job = job;
    std::optional<bot::InputTrack> designed;
    if (auto cur = current()) designed = cur->inputs;
    std::thread([job, levelString = std::move(levelString), designed]() {
        auto t0 = std::chrono::steady_clock::now();
        try {
            setStage(*job, "Reading the level", 0.05f);
            auto lvl = level::Level::fromLevelString(levelString);
            if (!lvl) { fail(*job, "cannot read the editor level"); job->done = true; return; }
            phys::World W(*lvl);
            // the designed inputs first (fast); a bot search if they do not finish the level
            if (designed && bot::isBeatable(W, *designed)) {
                job->beatable = true;
                job->solution = *designed;
            } else {
                setStage(*job, "The bot is solving the level", 0.2f);
                Progress prog;
                prog.cancelFlag = &job->cancel;
                prog.onProgress = [&](std::string const&, float f) { setStage(*job, "The bot is solving the level", 0.2f + 0.8f * f); };
                bot::SolveOptions so;
                so.progress = &prog;
                auto res = bot::autoSolve(W, so);
                job->beatable = res.solved;
                job->solution = res.inputs;
                if (!res.solved) job->failure = bot::firstFailure(W, res.inputs);
            }
        } catch (Cancelled const&) {
            fail(*job, "cancelled");
        } catch (std::exception const& e) {
            fail(*job, std::string("error: ") + e.what());
        }
        job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        setStage(*job, "Done", 1.f);
        job->done = true;
    }).detach();
    return job;
}

void Session::collect(std::shared_ptr<Job> const& job) {
    if (!job || !job->done) return;
    if (job->kind == Job::Kind::Learn && job->profile && !job->profile->empty()) {
        auto p = std::make_shared<gen::StyleProfile>();
        if (job->mergeProfile && m_profile) *p = *m_profile;
        p->add(*job->profile);
        auto r = file::writeString(profilePath(), p->toJson().dump(1));
        if (r.isErr()) log::warn("AutoLayout: cannot save the style profile: {}", r.unwrapErr());
        m_profile = p;
        job->profile.reset();
    }
    if (job->analysis) m_analysis = job->analysis;
    if (job->kind == Job::Kind::Generate && !job->results.empty()) {
        m_variants = std::move(job->results);
        m_selected = 0;
        m_fusion = job->fusion;
    } else if (job->kind == Job::Kind::Reroll && !job->results.empty()) {
        if (m_selected >= 0 && m_selected < int(m_variants.size())) m_variants[size_t(m_selected)] = std::move(job->results.front());
        else { m_variants = std::move(job->results); m_selected = 0; }
    }
    job->results.clear();
}

// ---------------------------------------------------------------- style profile

std::filesystem::path Session::profilePath() const { return Mod::get()->getSaveDir() / "style_profile.json"; }

void Session::loadProfile() {
    m_profile.reset();
    auto text = file::readString(profilePath());
    if (text.isErr()) return;
    if (auto j = Json::parse(text.unwrap()))
        if (auto p = gen::StyleProfile::fromJson(*j)) m_profile = std::make_shared<gen::StyleProfile>(*p);
}

void Session::forgetProfile() {
    m_profile.reset();
    std::error_code ec;
    std::filesystem::remove(profilePath(), ec);
}

std::string Session::profileSummary() const {
    if (!m_profile || m_profile->empty()) return "Nothing learned yet: the built-in style is used.";
    return "Learned: " + m_profile->summary();
}

std::shared_ptr<Job> Session::startLearn(std::vector<std::filesystem::path> files, bool merge) {
    if (busy()) return m_job;
    auto job = std::make_shared<Job>();
    job->kind = Job::Kind::Learn;
    job->mergeProfile = merge;
    m_job = job;
    std::thread([job, files = std::move(files)]() {
        auto t0 = std::chrono::steady_clock::now();
        auto total = std::make_shared<gen::StyleProfile>();
        for (size_t i = 0; i < files.size() && !job->cancel; ++i) {
            const auto name = geode::utils::string::pathToString(files[i].filename());
            setStage(*job, fmt::format("Learning from {} ({} of {}): the bot plays it", name, i + 1, files.size()), float(i) / float(std::max<size_t>(1, files.size())));
            auto text = file::readString(files[i]);
            std::string line;
            if (text.isErr()) line = name + ": cannot read";
            else if (auto lvl = level::Level::fromGmd(text.unwrap())) {
                gen::StyleProfile p;
                std::string why;
                if (gen::profileFromLevel(*lvl, p, why)) {
                    total->add(p);
                    ++job->learned;
                    line = name + ": " + p.summary();
                } else {
                    ++job->skipped;
                    line = name + ": skipped, " + why;
                }
            } else {
                ++job->skipped;
                line = name + ": not a level file";
            }
            std::lock_guard<std::mutex> lk(job->mutex);
            job->learnLog += line + "\n";
        }
        job->profile = total;
        job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        setStage(*job, fmt::format("Learned from {} level(s), {} skipped", job->learned, job->skipped), 1.f);
        job->done = true;
    }).detach();
    return job;
}

// ---------------------------------------------------------------- models

int Session::reloadModels() {
    m_models = model::ModelSet();
    std::string err;
    // user-trained models (save dir) take precedence over the shipped ones
    auto user = Mod::get()->getSaveDir() / "models";
    int n = 0;
    std::error_code ec;
    if (std::filesystem::exists(user, ec)) n = m_models.loadDirectory(geode::utils::string::pathToString(user), &err);
    if (n == 0) n = m_models.loadDirectory(geode::utils::string::pathToString(Mod::get()->getResourcesDir()), &err);
    if (!err.empty()) log::warn("AutoLayout models: {}", err);
    log::info("AutoLayout: {} pattern model(s) loaded", n);
    return n;
}

std::string Session::modelSummary() const {
    if (m_models.empty()) return "No trained model found: the rule-based style model is used.";
    std::string s;
    for (auto& m : m_models.models()) {
        if (!s.empty()) s += ", ";
        s += m->info().name;
    }
    return fmt::format("{} model(s): {} ({} parameters each)", m_models.models().size(), s, m_models.models().front()->parameterCount());
}

void Session::resetForLevel() {
    if (m_job) m_job->cancel = true;
    m_job.reset();
    m_variants.clear();
    m_selected = -1;
    m_analysis.reset();
    m_fusion.reset();
    m_customSong.reset();
}

} // namespace al::mod
