#pragma once
// Small deterministic synthesizer used for unit tests (signals with known ground truth) and
// for the bundled demo song (generated at runtime, so no copyrighted audio ships with the mod).
#include <al/audio/decode.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace al::audio {

enum class Drum : uint8_t { Kick, Snare, HiHat, Clap };

struct DrumHit { double time; Drum kind; float velocity; };
struct SynthNote { double time; double duration; float midi; float velocity; int voice; }; // voice 0 = lead, 1 = bass, 2 = pad

struct SectionTruth { double start, end; std::string label; };

struct SynthScore {
    double bpm = 128;
    double duration = 0;
    std::vector<DrumHit> drums;
    std::vector<SynthNote> notes;
    std::vector<SectionTruth> sections; // ground truth labels
    std::vector<double> beats;          // ground truth beat times
};

AudioBuffer render(const SynthScore& score, int sampleRate = 22050, uint64_t seed = 1);

// Metronome clicks (short broadband pulses) at bpm starting at offset.
SynthScore clickTrack(double bpm, double duration, double offset = 0.5);
// A tempo-changing click track: bpm1 until switchTime, then bpm2.
SynthScore clickTrackTempoChange(double bpm1, double bpm2, double switchTime, double duration);
// Four-on-the-floor drum loop with snares on 2 and 4 and 8th hats.
SynthScore drumLoop(double bpm, int bars, double offset = 0.25);
// ~100 s electronic demo track: intro, verse, build-up, drop, break, build-up, drop, outro.
SynthScore demoSong(uint64_t seed = 7);

} // namespace al::audio
