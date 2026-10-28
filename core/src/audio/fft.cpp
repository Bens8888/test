#include <al/audio/fft.hpp>

#include <cmath>
#include <stdexcept>

namespace al::audio {

static constexpr double kPi = 3.14159265358979323846;

bool isPow2(size_t n) { return n >= 1 && (n & (n - 1)) == 0; }

Fft::Fft(size_t n) : m_n(n) {
    if (n < 2 || !isPow2(n)) throw std::invalid_argument("Fft size must be a power of two");
    m_twiddle.resize(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        double a = -2.0 * kPi * double(k) / double(n);
        m_twiddle[k] = cfloat(float(std::cos(a)), float(std::sin(a)));
    }
    m_rev.resize(n);
    size_t bits = 0;
    while ((size_t(1) << bits) < n) ++bits;
    for (size_t i = 0; i < n; ++i) {
        size_t r = 0;
        for (size_t b = 0; b < bits; ++b) if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
        m_rev[i] = r;
    }
}

void Fft::transform(std::vector<cfloat>& a, bool inverse) const {
    const size_t n = m_n;
    if (a.size() != n) a.resize(n);
    for (size_t i = 0; i < n; ++i) if (i < m_rev[i]) std::swap(a[i], a[m_rev[i]]);
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1;
        size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < half; ++j) {
                cfloat w = m_twiddle[j * step];
                if (inverse) w = std::conj(w);
                cfloat u = a[i + j];
                cfloat v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
        }
    }
}

RealFft::RealFft(size_t n) : m_n(n), m_half(n / 2) {
    if (n < 4 || !isPow2(n)) throw std::invalid_argument("RealFft size must be a power of two >= 4");
    m_w.resize(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        double a = -2.0 * kPi * double(k) / double(n);
        m_w[k] = cfloat(float(std::cos(a)), float(std::sin(a)));
    }
    m_buf.resize(n / 2);
}

void RealFft::forward(const float* in, std::vector<cfloat>& out) const {
    const size_t h = m_n / 2;
    for (size_t i = 0; i < h; ++i) m_buf[i] = cfloat(in[2 * i], in[2 * i + 1]);
    m_half.transform(m_buf, false);
    out.resize(h + 1);
    // Split the packed spectrum: X[k] = (Z[k] + conj(Z[h-k]))/2 + W^k (Z[k] - conj(Z[h-k]))/(2i)
    for (size_t k = 0; k <= h; ++k) {
        cfloat zk = m_buf[k % h];
        cfloat zc = std::conj(m_buf[(h - k) % h]);
        cfloat even = (zk + zc) * 0.5f;
        cfloat odd = (zk - zc) * cfloat(0.0f, -0.5f);
        cfloat w = (k < h) ? m_w[k] : cfloat(-1.0f, 0.0f);
        out[k] = even + w * odd;
    }
}

void RealFft::magnitude(const float* in, std::vector<float>& mag) const {
    forward(in, m_spec);
    mag.resize(m_spec.size());
    for (size_t i = 0; i < m_spec.size(); ++i) mag[i] = std::abs(m_spec[i]);
}

std::vector<float> hannWindow(size_t n) {
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i) w[i] = float(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(n)));
    return w;
}

} // namespace al::audio
