#pragma once
// In-game click recorder: owns the practice stitcher while armed, samples the players' state at
// every button event, and saves finished runs to <save dir>/recordings (.alrec + .json).
// Works in normal play (PlayLayer, including practice mode) and in editor playtests, where the
// user plays along with the song on an empty or partial level.
#include <Geode/Geode.hpp>

#include <al/rec/recording.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace al::mod {

class LiveRecorder {
public:
    static LiveRecorder& get();

    // Armed: a session starts automatically on the next attempt / playtest.
    bool armed() const { return m_armed; }
    void setArmed(bool armed);
    void toggle();
    bool recording() const { return m_stitcher.active(); }

    // ---- driven by the hooks
    void beginSession(GJBaseGameLayer* layer, GJGameLevel* level, bool editorPlaytest);
    void onButton(GJBaseGameLayer* layer, bool down, int button, bool isPlayer1);
    void onOrb(GJBaseGameLayer* layer, int objectId);
    void onCheckpoint(int32_t tick);
    void onCheckpointRemoved();
    void onRespawn(GJBaseGameLayer* layer, int32_t tick);
    void onRestart();
    void onDeath(int32_t tick);
    void onComplete(GJBaseGameLayer* layer);
    // The session ends (level left, playtest stopped). Saves what was recorded.
    void endSession(GJBaseGameLayer* layer, bool completed);

    // ---- results
    // The most recent saved recording (this game session), and the file it went to.
    std::optional<rec::Recording> const& last() const { return m_last; }
    std::filesystem::path const& lastPath() const { return m_lastPath; }
    static std::filesystem::path folder();
    // Loads a recording or macro file (.alrec, .json, .gdr, frame list) and makes it "last".
    geode::Result<rec::Recording> load(std::filesystem::path const& path);

    static int32_t tickOf(GJBaseGameLayer* layer);
    static rec::PlayerSnap snap(PlayerObject* p);

private:
    std::filesystem::path save(rec::Recording const& r);
    rec::Stitcher m_stitcher;
    bool m_armed = false;
    bool m_editor = false;
    std::optional<rec::Recording> m_last;
    std::filesystem::path m_lastPath;
};

} // namespace al::mod
