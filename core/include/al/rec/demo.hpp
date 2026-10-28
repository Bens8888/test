#pragma once
// A deterministic "human" click recording for the bundled demo song: a player tapping along
// (kicks and snares, lead notes in the break, holds on long notes) with timing jitter and a
// constant latency. Used by the integration test and shipped as the demo recording.
#include <al/audio/synth.hpp>
#include <al/rec/recording.hpp>

namespace al::rec {

Recording demoRecording(const audio::SynthScore& song, uint64_t seed = 3, double latencySec = 0.025, double jitterSec = 0.010);

} // namespace al::rec
