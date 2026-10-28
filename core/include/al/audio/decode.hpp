#pragma once
// Audio decoding (mp3 via minimp3, ogg/vorbis via stb_vorbis, PCM/float WAV) to mono float,
// plus resampling to the analysis rate.
#include <cstdint>
#include <string>
#include <vector>

namespace al::audio {

struct AudioBuffer {
    int sampleRate = 0;
    std::vector<float> samples; // mono, [-1, 1]
    double duration() const { return sampleRate > 0 ? double(samples.size()) / sampleRate : 0.0; }
};

enum class DecodeError { None, FileNotFound, UnknownFormat, Corrupt };
const char* decodeErrorString(DecodeError e);

DecodeError decodeFile(const std::string& path, AudioBuffer& out);
DecodeError decodeMemory(const uint8_t* data, size_t size, AudioBuffer& out);

// Band-limited resampling (windowed-sinc, Kaiser) to targetRate.
AudioBuffer resample(const AudioBuffer& in, int targetRate);

// 16-bit PCM mono WAV writer (used for the demo song and debugging).
std::string encodeWav16(const AudioBuffer& buf);

} // namespace al::audio
