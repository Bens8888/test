#pragma once
// Tap along (like GD's Guideline Creator): plays the song in the editor, records a tap at the
// song position of every press (clicks, touches, Space/Up/W) and the matching release (holds),
// merges takes into one track per level, saves it, and shows it as GD guidelines (one line per
// tap) in the editor. No gameplay runs while tapping, so the taps sit exactly on the song.
#include <Geode/Geode.hpp>

#include <al/rec/taps.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace al::mod {

class TapAlong {
public:
    static TapAlong& get();

    // The track of this level (loaded from the save folder; if none was saved and the level has
    // guidelines, those become the first take, so GD's own Guideline Creator works as input too).
    rec::TapTrack& track(LevelEditorLayer* lel);
    // Recording of the level's track for the generator (nullopt when there are no taps).
    std::optional<rec::Recording> recordingFor(LevelEditorLayer* lel);

    // ---- a take
    // Plays `song` from `startSec` of level time (0 = where the level starts) and records.
    geode::Result<> start(LevelEditorLayer* lel, std::filesystem::path const& song, double startSec);
    // Ends the take: merges it, saves, updates the guidelines. Returns the number of taps taken.
    int stop();
    bool recording() const { return m_recording; }
    void press();
    void release();
    // Per frame while recording: follows the song clock, stops at the end of the song.
    // Returns false when the take ended by itself.
    bool update();
    double songTime() const;       // current song time (seconds) while recording
    size_t takeSize() const { return m_take.size(); }

    // ---- track edits (saved, guidelines updated)
    void clear(LevelEditorLayer* lel);
    bool undoTake(LevelEditorLayer* lel);
    // Writes the track into the level's guidelines (song offset applied).
    void showGuidelines(LevelEditorLayer* lel);

    static std::filesystem::path folder();

private:
    std::string keyFor(LevelEditorLayer* lel) const;
    void save();
    double now() const;

    std::string m_key;
    rec::TapTrack m_track;
    LevelEditorLayer* m_editor = nullptr;

    bool m_recording = false;
    rec::SongClock m_clock;
    double m_startSong = 0;        // song time the take started at
    double m_startClock = 0;
    double m_lastSongPos = -1;
    int m_stalledFrames = 0;
    std::vector<rec::Tap> m_take;
    bool m_down = false;
    std::chrono::steady_clock::time_point m_epoch = std::chrono::steady_clock::now();
};

} // namespace al::mod
