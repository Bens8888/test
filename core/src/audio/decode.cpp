#include <al/audio/decode.hpp>
#include <al/util/codec.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include <minimp3.h>
#include <minimp3_ex.h>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace al::audio {

const char* decodeErrorString(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "ok";
        case DecodeError::FileNotFound: return "file not found";
        case DecodeError::UnknownFormat: return "unsupported audio format";
        case DecodeError::Corrupt: return "corrupt audio data";
    }
    return "?";
}

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

static DecodeError decodeWav(const uint8_t* d, size_t n, AudioBuffer& out) {
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return DecodeError::UnknownFormat;
    size_t off = 12;
    int channels = 0, rate = 0, bits = 0, fmt = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmLen = 0;
    while (off + 8 <= n) {
        uint32_t len = rd32(d + off + 4);
        const uint8_t* body = d + off + 8;
        size_t avail = std::min<size_t>(len, n - off - 8);
        if (std::memcmp(d + off, "fmt ", 4) == 0 && avail >= 16) {
            fmt = rd16(body);
            channels = rd16(body + 2);
            rate = int(rd32(body + 4));
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && avail >= 26) fmt = rd16(body + 24); // WAVE_FORMAT_EXTENSIBLE subformat
        } else if (std::memcmp(d + off, "data", 4) == 0) {
            pcm = body;
            pcmLen = avail;
        }
        off += 8 + len + (len & 1);
    }
    if (!pcm || channels <= 0 || rate <= 0) return DecodeError::Corrupt;
    int bytesPer = bits / 8;
    if (bytesPer <= 0) return DecodeError::Corrupt;
    size_t frames = pcmLen / size_t(bytesPer * channels);
    out.sampleRate = rate;
    out.samples.assign(frames, 0.0f);
    for (size_t f = 0; f < frames; ++f) {
        float acc = 0;
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p = pcm + (f * channels + c) * bytesPer;
            float v = 0;
            if (fmt == 3 && bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
            else if (fmt == 3 && bits == 64) { double x; std::memcpy(&x, p, 8); v = float(x); }
            else if (bits == 8) v = (p[0] - 128) / 128.0f;
            else if (bits == 16) v = int16_t(rd16(p)) / 32768.0f;
            else if (bits == 24) { int32_t x = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); v = (x >> 8) / 8388608.0f; }
            else if (bits == 32) v = float(int32_t(rd32(p)) / 2147483648.0);
            else return DecodeError::UnknownFormat;
            acc += v;
        }
        out.samples[f] = acc / float(channels);
    }
    return DecodeError::None;
}

static DecodeError decodeMp3(const uint8_t* d, size_t n, AudioBuffer& out) {
    mp3dec_t dec;
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&dec, d, n, &info, nullptr, nullptr) != 0 || !info.buffer || info.samples == 0) {
        if (info.buffer) free(info.buffer);
        return DecodeError::Corrupt;
    }
    int ch = std::max(1, info.channels);
    size_t frames = info.samples / size_t(ch);
    out.sampleRate = info.hz;
    out.samples.resize(frames);
    for (size_t f = 0; f < frames; ++f) {
        float acc = 0;
        for (int c = 0; c < ch; ++c) acc += info.buffer[f * ch + c];
        out.samples[f] = acc / float(ch);
    }
    free(info.buffer);
    return DecodeError::None;
}

static DecodeError decodeOgg(const uint8_t* d, size_t n, AudioBuffer& out) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(d, int(n), &err, nullptr);
    if (!v) return DecodeError::Corrupt;
    stb_vorbis_info info = stb_vorbis_get_info(v);
    int ch = std::max(1, info.channels);
    out.sampleRate = int(info.sample_rate);
    out.samples.clear();
    std::vector<float> chunk(4096 * size_t(ch));
    for (;;) {
        int got = stb_vorbis_get_samples_float_interleaved(v, ch, chunk.data(), int(chunk.size()));
        if (got <= 0) break;
        for (int f = 0; f < got; ++f) {
            float acc = 0;
            for (int c = 0; c < ch; ++c) acc += chunk[size_t(f) * ch + c];
            out.samples.push_back(acc / float(ch));
        }
    }
    stb_vorbis_close(v);
    return out.samples.empty() ? DecodeError::Corrupt : DecodeError::None;
}

DecodeError decodeMemory(const uint8_t* d, size_t n, AudioBuffer& out) {
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0) return decodeWav(d, n, out);
    if (n >= 4 && std::memcmp(d, "OggS", 4) == 0) return decodeOgg(d, n, out);
    // mp3: ID3 tag or frame sync
    if (n >= 3 && (std::memcmp(d, "ID3", 3) == 0 || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))) return decodeMp3(d, n, out);
    // Last resort: try mp3 (some files have junk before the first frame)
    auto r = decodeMp3(d, n, out);
    return r == DecodeError::None ? r : DecodeError::UnknownFormat;
}

DecodeError decodeFile(const std::string& path, AudioBuffer& out) {
    std::string data;
    if (!readFile(path, data)) return DecodeError::FileNotFound;
    return decodeMemory(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out);
}

static double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

AudioBuffer resample(const AudioBuffer& in, int targetRate) {
    AudioBuffer out;
    out.sampleRate = targetRate;
    if (in.sampleRate <= 0 || in.samples.empty()) return out;
    if (in.sampleRate == targetRate) { out.samples = in.samples; return out; }
    const double ratio = double(targetRate) / in.sampleRate;
    const double cutoff = std::min(1.0, ratio) * 0.95; // relative to input Nyquist
    const int half = 16;                                // taps on each side (at input rate, scaled when downsampling)
    const double beta = 8.0;
    const double i0b = besselI0(beta);
    const double scale = std::min(1.0, ratio);
    const int taps = int(std::ceil(half / scale));
    // Precompute a fine-grained kernel table (64 phases per input sample).
    const int phases = 64;
    std::vector<float> table(size_t((2 * taps + 1) * phases + 1));
    for (size_t i = 0; i < table.size(); ++i) {
        double x = double(i) / phases - taps; // distance in input samples
        double t = x * cutoff;
        double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(3.14159265358979 * t) / (3.14159265358979 * t);
        double r = x / taps;
        double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1 - r * r)) / i0b;
        table[i] = float(sinc * w * cutoff);
    }
    size_t outN = size_t(std::floor(double(in.samples.size()) * ratio));
    out.samples.resize(outN);
    const long long inN = (long long)in.samples.size();
    for (size_t o = 0; o < outN; ++o) {
        double center = double(o) / ratio;
        long long c = (long long)std::floor(center);
        double frac = center - double(c);
        double acc = 0;
        for (int k = -taps; k <= taps; ++k) {
            long long idx = c + k;
            if (idx < 0 || idx >= inN) continue;
            double x = double(k) - frac + taps; // table coordinate
            double pos = x * phases;
            int pi = int(pos);
            if (pi < 0 || pi + 1 >= int(table.size())) continue;
            float a = table[size_t(pi)], b = table[size_t(pi + 1)];
            acc += in.samples[size_t(idx)] * (a + (b - a) * float(pos - pi));
        }
        out.samples[o] = float(acc);
    }
    return out;
}

std::string encodeWav16(const AudioBuffer& buf) {
    std::string out;
    auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xFF); };
    auto w16 = [&](uint16_t v) { out += char(v & 0xFF); out += char(v >> 8); };
    uint32_t dataLen = uint32_t(buf.samples.size() * 2);
    out += "RIFF"; w32(36 + dataLen); out += "WAVE";
    out += "fmt "; w32(16); w16(1); w16(1); w32(uint32_t(buf.sampleRate)); w32(uint32_t(buf.sampleRate * 2)); w16(2); w16(16);
    out += "data"; w32(dataLen);
    for (float s : buf.samples) {
        float c = std::max(-1.0f, std::min(1.0f, s));
        w16(uint16_t(int16_t(std::lround(c * 32767.0f))));
    }
    return out;
}

} // namespace al::audio
