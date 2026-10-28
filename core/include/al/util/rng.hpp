#pragma once
// Deterministic random numbers. Every random decision in AutoLayout draws from a Rng
// seeded from (user seed, purpose, index) so results are reproducible across platforms
// and compilers (no std:: distributions, whose output is implementation-defined).
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

namespace al {

// 64-bit mixing function (splitmix64 finalizer).
constexpr uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr uint64_t hashStr(std::string_view s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (char c : s) { h ^= uint8_t(c); h *= 0x100000001B3ull; }
    return h;
}

constexpr uint64_t combineSeed(uint64_t a, uint64_t b) { return mix64(a ^ mix64(b + 0x632BE59BD9B4E019ull)); }

// PCG32 (O'Neill), XSH-RR variant.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x853C49E6748FEA9Bull, uint64_t stream = 0xDA3E39CB94B95BDBull) { reseed(seed, stream); }

    void reseed(uint64_t seed, uint64_t stream = 0xDA3E39CB94B95BDBull) {
        m_state = 0;
        m_inc = (stream << 1u) | 1u;
        next();
        m_state += seed;
        next();
    }

    // Derive an independent child stream (e.g. one per section) without consuming this one.
    Rng fork(uint64_t tag) const { return Rng(combineSeed(m_state, tag), combineSeed(m_inc, ~tag)); }

    uint32_t next() {
        uint64_t old = m_state;
        m_state = old * 6364136223846793005ull + m_inc;
        uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
    }

    // Uniform in [0, bound) without modulo bias.
    uint32_t below(uint32_t bound) {
        if (bound <= 1) return 0;
        uint32_t threshold = (0u - bound) % bound;
        for (;;) {
            uint32_t r = next();
            if (r >= threshold) return r % bound;
        }
    }

    int range(int lo, int hi) { // inclusive
        if (hi <= lo) return lo;
        return lo + int(below(uint32_t(hi - lo + 1)));
    }

    double uniform() { return (next() >> 8) * (1.0 / 16777216.0); }           // [0,1)
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }
    bool chance(double p) { return uniform() < p; }

    // Approximately normal (sum of 4 uniforms, deterministic and cheap).
    double gauss(double mean = 0.0, double sd = 1.0) {
        double s = uniform() + uniform() + uniform() + uniform() - 2.0;
        return mean + sd * s * 1.7320508075688772; // var of sum of 4 U(0,1) = 1/3
    }

    // Sample an index from non-negative weights. Returns -1 if all weights are zero.
    int weighted(const double* w, size_t n) {
        double total = 0;
        for (size_t i = 0; i < n; ++i) total += w[i] > 0 ? w[i] : 0;
        if (total <= 0) return -1;
        double r = uniform() * total;
        for (size_t i = 0; i < n; ++i) {
            double wi = w[i] > 0 ? w[i] : 0;
            if (r < wi) return int(i);
            r -= wi;
        }
        for (size_t i = n; i-- > 0;) if (w[i] > 0) return int(i);
        return -1;
    }
    int weighted(const std::vector<double>& w) { return weighted(w.data(), w.size()); }
    int weighted(const std::vector<float>& w) { return weighted(w.data(), w.size()); }
    int weighted(const float* w, size_t n) {
        std::vector<double> tmp(w, w + n);
        return weighted(tmp.data(), tmp.size());
    }

    template <class T> void shuffle(std::vector<T>& v) {
        for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[below(uint32_t(i))]);
    }

    uint64_t state() const { return m_state; }

private:
    uint64_t m_state = 0, m_inc = 0;
};

} // namespace al
