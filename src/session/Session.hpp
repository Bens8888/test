#pragma once
// Generation session: options (persisted), the song and its analysis (cached), background jobs
// (analysis, generation, re-roll, bot playtest) with progress and cancellation, the variants, and
// the trained models. Plain C++ + a few GD calls on the main thread only; workers never touch
// cocos objects (the popup polls the job state every frame).
#include <Geode/Geode.hpp>

#include <al/audio/analysis.hpp>
#include <al/gen/fusion.hpp>
#include <al/gen/generator.hpp>
#include <al/gen/profile.hpp>
#include <al/model/model.hpp>
#include <al/rec/recording.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace al::mod {

struct Job {
    enum class Kind { Analyze, Generate, Reroll, Playtest, Learn };
    Kind kind = Kind::Generate;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<float> progress{0.f};
    mutable std::mutex mutex;
    std::string stage;               // guarded by mutex
    std::string error;               // guarded by mutex (empty = ok)
    // results (read after done)
    std::shared_ptr<const audio::MusicAnalysis> analysis;
    std::vector<gen::GenResult> results;
    std::optional<gen::ClickFusion> fusion;
    // playtest
    bool beatable = false;
    bot::Failure failure;
    bot::InputTrack solution;
    // learning (style profile from real levels)
    std::shared_ptr<gen::StyleProfile> profile;
    bool mergeProfile = false;
    int learned = 0, skipped = 0;
    std::string learnLog;            // guarded by mutex
    double seconds = 0;
    std::string stageText() const { std::lock_guard<std::mutex> lk(mutex); return stage; }
    std::string errorText() const { std::lock_guard<std::mutex> lk(mutex); return error; }
};

struct SongChoice {
    std::filesystem::path path;  // resolved audio file
    std::string label;           // shown in the UI
    bool custom = false;         // picked by the user (not the level's song)
};

class Session {
public:
    static Session& get();

    // ---- options (saved in the mod's saved values; presets as JSON files)
    gen::GenOptions& options() { return m_opt; }
    void saveOptions();
    void loadOptions();
    std::filesystem::path presetDir() const;
    geode::Result<> savePreset(std::string const& name);
    geode::Result<> loadPreset(std::string const& name);
    std::vector<std::string> presets() const;

    // ---- song
    // The level's song (custom NG / library song or official track). Err with a reason.
    geode::Result<SongChoice> levelSong(GJGameLevel* level) const;
    void setCustomSong(std::filesystem::path const& p);
    void clearCustomSong() { m_customSong.reset(); }
    std::optional<SongChoice> const& customSong() const { return m_customSong; }
    // Custom song if picked, else the level's song.
    geode::Result<SongChoice> song(GJGameLevel* level) const;
    std::shared_ptr<const audio::MusicAnalysis> cachedAnalysis(std::filesystem::path const& p) const;

    // ---- recording mode
    bool useRecording() const { return m_opt.input == gen::InputSource::Recording; }

    // ---- jobs
    std::shared_ptr<Job> job() const { return m_job; }
    bool busy() const { return m_job && !m_job->done; }
    void cancel() { if (m_job) m_job->cancel = true; }
    // Analyse (cached) + generate opt.variants variants on a worker thread.
    std::shared_ptr<Job> startGenerate(GJGameLevel* level);
    // Re-roll one part of the selected variant (new random stream for that part).
    std::shared_ptr<Job> startReroll(int partIndex);
    // Bot playtest of a level string (the editor's live level).
    std::shared_ptr<Job> startPlaytest(std::string levelString);
    // Called by the popup when a job finished: moves the results into the session.
    void collect(std::shared_ptr<Job> const& job);

    // ---- results
    std::vector<gen::GenResult> const& variants() const { return m_variants; }
    int selected() const { return m_selected; }
    void select(int i) { if (i >= 0 && i < int(m_variants.size())) m_selected = i; }
    gen::GenResult const* current() const { return m_selected >= 0 && m_selected < int(m_variants.size()) ? &m_variants[size_t(m_selected)] : nullptr; }
    std::shared_ptr<const audio::MusicAnalysis> analysis() const { return m_analysis; }
    std::optional<gen::ClickFusion> const& fusion() const { return m_fusion; }
    void clearResults() { m_variants.clear(); m_selected = -1; }

    // ---- models
    model::ModelSet const& models() const { return m_models; }
    std::string modelSummary() const;
    int reloadModels();
    bool useModel() const { return m_useModel && !m_models.empty(); }
    void setUseModel(bool on) { m_useModel = on; }

    // ---- style profile (generator v2, learned from real levels)
    std::shared_ptr<const gen::StyleProfile> profile() const { return m_profile; }
    std::string profileSummary() const;
    bool useProfile() const { return m_useProfile && m_profile && !m_profile->empty(); }
    void setUseProfile(bool on) { m_useProfile = on; }
    std::filesystem::path profilePath() const;
    void loadProfile();
    void forgetProfile();
    // Learn from these levels (.gmd files) on a worker thread; merge = add to the current profile.
    std::shared_ptr<Job> startLearn(std::vector<std::filesystem::path> files, bool merge);

    // Called when a new editor session starts (no state leaks between levels).
    void resetForLevel();

private:
    Session();
    gen::GenOptions m_opt;
    std::optional<SongChoice> m_customSong;
    std::shared_ptr<Job> m_job;
    std::vector<gen::GenResult> m_variants;
    int m_selected = -1;
    std::shared_ptr<const audio::MusicAnalysis> m_analysis;
    std::optional<gen::ClickFusion> m_fusion;
    // analysis cache: (path, file size, mtime) -> analysis
    struct CacheEntry { std::string key; std::shared_ptr<const audio::MusicAnalysis> analysis; };
    mutable std::mutex m_cacheMutex;
    std::vector<CacheEntry> m_cache;
    model::ModelSet m_models;
    bool m_useModel = true;
    std::shared_ptr<const gen::StyleProfile> m_profile;
    bool m_useProfile = true;

public:
    // used by workers
    std::string cacheKey(std::filesystem::path const& p) const;
    void storeAnalysis(std::string const& key, std::shared_ptr<const audio::MusicAnalysis> a);
};

// Human-readable names for the UI.
char const* difficultyLabel(gen::Difficulty d);
char const* roleLabel(gen::Role r);

} // namespace al::mod
