#pragma once
// Music analysis: turns decoded audio into a MusicAnalysis — per-frame feature curves plus
// one time-sorted timeline of typed musical events (beats, downbeats, onsets per band,
// kick/snare/hi-hat, melody notes and sustains, sections, build-ups and drops).
#include <al/audio/decode.hpp>
#include <al/util/progress.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace al::audio {

enum class Band : uint8_t { Bass = 0, LowMid, Mid, High, Count };

enum class EventType : uint8_t {
    Beat,
    Downbeat,
    Onset,     // generic onset (any band); band field says where most of the flux was
    Kick,
    Snare,
    HiHat,
    Note,      // melody note start (pitch, duration)
    Sustain,   // a melody/harmonic note held for >= sustainMin seconds (duration set)
    SectionStart,
    BuildUp,   // start of a build-up (duration = length until the drop)
    Drop,      // the moment a drop hits
    TempoChange,
    Silence,   // start of a quiet gap (duration set)
};
const char* eventTypeName(EventType t);

enum class SectionType : uint8_t { Intro, Verse, Chorus, BuildUp, Drop, Break, Outro };
const char* sectionTypeName(SectionType t);

struct MusicEvent {
    double time = 0;      // seconds
    EventType type = EventType::Onset;
    float strength = 0;   // 0..1 normalized salience
    Band band = Band::Mid;
    float pitch = 0;      // MIDI note number (Note/Sustain), 0 otherwise
    float duration = 0;   // seconds (Note/Sustain/BuildUp/Silence)
    int index = -1;       // beat index for Beat/Downbeat, section index for SectionStart
};

struct TempoSegment {
    double start = 0, end = 0;
    double bpm = 120;
};

struct Section {
    double start = 0, end = 0;
    SectionType type = SectionType::Verse;
    float energy = 0;      // mean intensity 0..1
    float power = 0;       // loudness + low-end weight 0..1 (what makes a drop a drop)
    float density = 0;     // onsets per second
    float trend = 0;       // intensity slope over the section (per 10 s)
    int firstBar = 0, barCount = 0;
    double duration() const { return end - start; }
};

struct AnalysisOptions {
    int analysisRate = 22050;
    float minBpm = 60, maxBpm = 220;
    float preferredBpm = 130;    // centre of the tempo prior (log-normal, 1 octave sigma)
    float sustainMin = 0.30f;    // seconds; notes longer than this become Sustain events
    int beatsPerBar = 0;         // 0 = detect (3 or 4)
    bool fineOnsets = true;      // time-domain refinement of onset times (~1.5 ms resolution)
};

struct MusicAnalysis {
    double duration = 0;
    double hop = 0;                 // seconds per feature frame (fine frames)
    double hopCoarse = 0;           // seconds per coarse frame (pitch/HPSS)

    // Fine frame curves (hop)
    std::vector<float> rmsDb;
    std::vector<float> onsetEnv;    // normalized total spectral flux
    std::array<std::vector<float>, size_t(Band::Count)> bandOnset;
    std::vector<float> intensity;   // 0..1 smoothed energy/density curve

    // Coarse frame curves (hopCoarse)
    std::vector<float> percRatio;   // percussive / (perc + harm) energy
    std::vector<float> bassPercRatio; // same, restricted to 30-150 Hz (kick vs bass note)
    std::vector<float> pitchMidi;   // 0 = unvoiced
    std::vector<float> pitchConf;   // salience 0..1
    std::vector<std::array<float, 12>> chroma;

    std::vector<double> beats;          // seconds
    std::vector<float> beatStrength;    // onset salience at each beat
    std::vector<int> beatInBar;         // 0 = downbeat
    std::vector<double> downbeats;
    int beatsPerBar = 4;
    std::vector<TempoSegment> tempo;
    std::vector<Section> sections;
    std::vector<MusicEvent> events;     // sorted by time

    std::vector<float> waveformMin, waveformMax; // preview envelope (fixed bin count)

    double mainBpm() const;
    float intensityAt(double t) const;
    float pitchAt(double t, float* conf = nullptr) const;
    float onsetAt(double t) const;
    int sectionAt(double t) const;
    double beatPeriodAt(double t) const;
    // Nearest beat / subdivision grid position to t (div = subdivisions per beat).
    double snapToGrid(double t, int div) const;
    // Events of a type in [t0, t1)
    std::vector<const MusicEvent*> eventsIn(double t0, double t1, EventType type) const;
};

MusicAnalysis analyze(const AudioBuffer& audio, const AnalysisOptions& opt = {}, const Progress* progress = nullptr);

// Convenience: decode + analyze a file. Returns false (and sets error) on decode failure.
bool analyzeFile(const std::string& path, MusicAnalysis& out, std::string& error,
                 const AnalysisOptions& opt = {}, const Progress* progress = nullptr);

} // namespace al::audio
