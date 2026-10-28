#pragma once
// Own radix-2 FFT (iterative Cooley-Tukey) with cached twiddles, plus a real-input
// transform built on a half-size complex FFT. No external dependencies.
#include <complex>
#include <cstddef>
#include <vector>

namespace al::audio {

using cfloat = std::complex<float>;

class Fft {
public:
    explicit Fft(size_t n); // n must be a power of two >= 2
    size_t size() const { return m_n; }
    // In-place forward transform (unnormalized). inverse=true computes the unnormalized inverse.
    void transform(std::vector<cfloat>& data, bool inverse = false) const;

private:
    size_t m_n;
    std::vector<cfloat> m_twiddle;
    std::vector<size_t> m_rev;
};

// Real FFT of length n (power of two >= 4): output has n/2+1 bins.
class RealFft {
public:
    explicit RealFft(size_t n);
    size_t size() const { return m_n; }
    void forward(const float* in, std::vector<cfloat>& out) const;
    // Magnitude spectrum convenience (n/2+1 values).
    void magnitude(const float* in, std::vector<float>& mag) const;

private:
    size_t m_n;
    Fft m_half;
    std::vector<cfloat> m_w; // post-processing twiddles
    mutable std::vector<cfloat> m_buf;
    mutable std::vector<cfloat> m_spec;
};

std::vector<float> hannWindow(size_t n);
bool isPow2(size_t n);

} // namespace al::audio
