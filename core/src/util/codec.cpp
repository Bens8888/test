#include <al/util/codec.hpp>

#include <miniz.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace al {

static const char* kB64Std = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char* kB64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64Encode(std::string_view data, bool urlSafe) {
    const char* tbl = urlSafe ? kB64Url : kB64Std;
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (uint8_t(data[i]) << 16) | (uint8_t(data[i + 1]) << 8) | uint8_t(data[i + 2]);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += tbl[n & 63];
        i += 3;
    }
    size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = uint8_t(data[i]) << 16;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        uint32_t n = (uint8_t(data[i]) << 16) | (uint8_t(data[i + 1]) << 8);
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63]; out += tbl[(n >> 6) & 63]; out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    int8_t rev[256];
    std::memset(rev, -1, sizeof(rev));
    for (int i = 0; i < 64; ++i) { rev[uint8_t(kB64Std[i])] = int8_t(i); rev[uint8_t(kB64Url[i])] = int8_t(i); }
    std::string out;
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int8_t v = rev[uint8_t(c)];
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xFF);
        }
    }
    return out;
}

uint32_t crc32(std::string_view data) {
    return uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size()));
}

std::string gzipCompress(std::string_view data, int level) {
    // Raw deflate body via tdefl, wrapped in a gzip header/trailer.
    size_t outLen = 0;
    int flags = tdefl_create_comp_flags_from_zip_params(level, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    void* body = tdefl_compress_mem_to_heap(data.data(), data.size(), &outLen, flags);
    std::string out;
    const unsigned char hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0x0b};
    out.append(reinterpret_cast<const char*>(hdr), 10);
    if (body) {
        out.append(static_cast<const char*>(body), outLen);
        mz_free(body);
    } else {
        // Empty input or allocation failure: emit a single empty final stored block.
        const unsigned char empty[5] = {1, 0, 0, 0xff, 0xff};
        out.append(reinterpret_cast<const char*>(empty), 5);
    }
    uint32_t crc = crc32(data);
    uint32_t size = uint32_t(data.size());
    for (int i = 0; i < 4; ++i) out += char((crc >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; ++i) out += char((size >> (8 * i)) & 0xFF);
    return out;
}

static std::optional<std::string> inflateRaw(const unsigned char* p, size_t n) {
    size_t outLen = 0;
    void* res = tinfl_decompress_mem_to_heap(p, n, &outLen, 0);
    if (!res) return std::nullopt;
    std::string out(static_cast<const char*>(res), outLen);
    mz_free(res);
    return out;
}

std::optional<std::string> inflateAny(std::string_view data) {
    auto p = reinterpret_cast<const unsigned char*>(data.data());
    size_t n = data.size();
    if (n >= 18 && p[0] == 0x1f && p[1] == 0x8b && p[2] == 8) {
        uint8_t flg = p[3];
        size_t off = 10;
        if (flg & 4) { if (off + 2 > n) return std::nullopt; size_t xlen = p[off] | (p[off + 1] << 8); off += 2 + xlen; }
        if (flg & 8) { while (off < n && p[off]) ++off; ++off; }
        if (flg & 16) { while (off < n && p[off]) ++off; ++off; }
        if (flg & 2) off += 2;
        if (off >= n) return std::nullopt;
        return inflateRaw(p + off, n - off);
    }
    if (n >= 2 && (p[0] & 0x0F) == 8 && ((p[0] << 8) | p[1]) % 31 == 0) {
        size_t outLen = 0;
        void* res = tinfl_decompress_mem_to_heap(p, n, &outLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (res) {
            std::string out(static_cast<const char*>(res), outLen);
            mz_free(res);
            return out;
        }
    }
    return inflateRaw(p, n);
}

std::string gdCompress(std::string_view plain) { return base64Encode(gzipCompress(plain), true); }

std::optional<std::string> gdDecompress(std::string_view text) {
    text = trim(text);
    // Plain level strings start with the header object ("kS38,..." / "kA..." / "1,").
    if (text.empty() || text.substr(0, 2) == "kS" || text.substr(0, 2) == "kA" || text.substr(0, 2) == "1,")
        return std::string(text);
    auto raw = base64Decode(text);
    if (!raw) return std::nullopt;
    return inflateAny(*raw);
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string_view trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool parseDouble(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty()) return false;
    // std::from_chars for double is not available on every libc++ we target (older macOS SDKs),
    // so use strtod on a bounded copy.
    char buf[64];
    if (s.size() >= sizeof(buf)) return false;
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    if (end == buf) return false;
    out = v;
    return true;
}

bool parseInt(std::string_view s, int& out) {
    s = trim(s);
    if (s.empty()) return false;
    int v = 0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc()) {
        double d;
        if (!parseDouble(s, d)) return false;
        v = int(d);
    }
    out = v;
    return true;
}

std::string fmtNum(double v) {
    if (std::isnan(v) || std::isinf(v)) return "0";
    double r = std::round(v);
    if (std::fabs(v - r) < 1e-9 && std::fabs(r) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(r));
        return buf;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string xmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string xmlUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            auto rest = s.substr(i);
            struct { const char* k; char v; } ents[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool done = false;
            for (auto& e : ents) {
                size_t len = std::strlen(e.k);
                if (rest.substr(0, len) == e.k) { out += e.v; i += len - 1; done = true; break; }
            }
            if (!done && rest.size() > 3 && rest[1] == '#') {
                size_t semi = rest.find(';');
                if (semi != std::string_view::npos) {
                    int code = 0;
                    bool hex = rest[2] == 'x';
                    auto num = rest.substr(hex ? 3 : 2, semi - (hex ? 3 : 2));
                    auto r = std::from_chars(num.data(), num.data() + num.size(), code, hex ? 16 : 10);
                    if (r.ec == std::errc() && code > 0 && code < 128) { out += char(code); i += semi; done = true; }
                }
            }
            if (!done) out += '&';
        } else {
            out += s[i];
        }
    }
    return out;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const std::string& path, std::string_view data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), std::streamsize(data.size()));
    return bool(f);
}

} // namespace al
