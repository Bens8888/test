#pragma once
// Tap-along click tracks, like GD's Guideline Creator: the song plays, the player taps along, and
// every tap is a click at that moment of the song (no gameplay in between, so nothing can drift).
// Several takes are merged into one track; the track is shown in the editor as GD guidelines and
// handed to the generator as a recording.
#include <al/rec/recording.hpp>
#include <al/util/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace al::rec {

struct Tap {
    double time = 0;     // press, seconds of song time
    double release = 0;  // release (time + a few ms for a plain tap)
    bool operator==(const Tap&) const = default;
};

class TapTrack {
public:
    // Adds a take. A tap of the take within `mergeSec` of a tap already in the track replaces it
    // (the newer take wins, so re-tapping a passage corrects it instead of doubling it).
    void merge(std::vector<Tap> take, double mergeSec = 0.06);
    // Undoes the last merged take (false if there is none).
    bool undoLastTake();
    void clear();
    const std::vector<Tap>& taps() const { return m_taps; }
    int takes() const { return int(m_history.size()); }
    bool empty() const { return m_taps.empty(); }

    // A recording for the generator: jump presses and releases in song time (source "taps").
    Recording toRecording(const std::string& levelName = "") const;
    // GD guidelines ("time~color~time~color~", level time = song time - songOffset; 0.8 orange,
    // 0.9 yellow, 1.0 green). Holds get a yellow line at the release.
    std::string toGuidelines(double songOffset) const;
    // Taps from a guideline string (GD's own Guideline Creator, or one written by toGuidelines).
    static std::vector<Tap> fromGuidelines(const std::string& guidelines, double songOffset);

    Json toJson() const;
    static std::optional<TapTrack> fromJson(const Json& j);

private:
    std::vector<Tap> m_taps;
    std::vector<std::vector<Tap>> m_history; // the track before each take
};

// Song time from the audio engine. The engine reports the song position coarsely (in mixer
// blocks, once per frame); a precise local clock fills in between: song = clock + offset, the
// offset being the median over the last second or so of samples. A jump (seek, stall) restarts it.
class SongClock {
public:
    void reset();
    // The engine reported `songSec` when the local clock read `clockSec`.
    void sample(double clockSec, double songSec);
    bool valid() const { return !m_window.empty(); }
    double songTimeAt(double clockSec) const { return clockSec + m_offset; }
    double offset() const { return m_offset; }

private:
    std::vector<double> m_window; // recent offsets, oldest first
    double m_offset = 0;
};

} // namespace al::rec
