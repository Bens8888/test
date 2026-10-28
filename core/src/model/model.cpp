#include <al/model/model.hpp>

#include <al/util/codec.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace al::model {

namespace {

struct Reader {
    const std::string& in;
    size_t pos = 0;
    bool ok = true;
    explicit Reader(const std::string& s) : in(s) {}
    uint32_t u32() {
        if (pos + 4 > in.size()) { ok = false; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(in[pos++])) << (8 * i);
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || pos + n > in.size()) { ok = false; return {}; }
        std::string s = in.substr(pos, n);
        pos += n;
        return s;
    }
    bool floats(std::vector<float>& v, size_t n) {
        if (!ok || pos + n * 4 > in.size()) { ok = false; return false; }
        v.resize(n);
        std::memcpy(v.data(), in.data() + pos, n * 4); // little-endian float32 (all supported platforms)
        pos += n * 4;
        for (float x : v) if (!std::isfinite(x)) { ok = false; return false; }
        return true;
    }
};

void setError(std::string* e, const std::string& m) { if (e) *e = m; }

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

} // namespace

std::unique_ptr<PatternModel> PatternModel::load(const std::string& path, std::string* error) {
    std::string bytes;
    if (!readFile(path, bytes)) { setError(error, "cannot read " + path); return nullptr; }
    return fromBytes(bytes, error);
}

std::unique_ptr<PatternModel> PatternModel::fromBytes(const std::string& bytes, std::string* error) {
    if (bytes.size() < 16 || bytes.compare(0, 4, "ALMD") != 0) { setError(error, "not an AutoLayout model file"); return nullptr; }
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored |= uint32_t(uint8_t(bytes[bytes.size() - 4 + size_t(i)])) << (8 * i);
    if (crc32(std::string_view(bytes).substr(0, bytes.size() - 4)) != stored) { setError(error, "model file is corrupted (checksum)"); return nullptr; }
    Reader r(bytes);
    r.pos = 4;
    uint32_t version = r.u32();
    if (version != kVersion) { setError(error, "model format version " + std::to_string(version) + " is not supported"); return nullptr; }
    auto m = std::make_unique<PatternModel>();
    m->m_F = int(r.u32());
    m->m_S = int(r.u32());
    m->m_E = int(r.u32());
    m->m_H = int(r.u32());
    m->m_V = int(r.u32());
    m->m_W = int(r.u32());
    m->m_info.style = int(int32_t(r.u32()));
    m->m_info.name = r.str();
    m->m_info.metadata = r.str();
    if (!r.ok || m->m_F != gen::kContextFeatures || m->m_H <= 0 || m->m_H > 1024 || m->m_E < 0 || m->m_E > 256 || m->m_V <= 0 || m->m_W <= 0) {
        setError(error, "model header is invalid");
        return nullptr;
    }
    if (m->m_V != gen::vocabSize()) {
        setError(error, "model vocabulary (" + std::to_string(m->m_V) + ") does not match this AutoLayout (" + std::to_string(gen::vocabSize()) + "): retrain it");
        return nullptr;
    }
    m->m_I = m->m_F + m->m_S + 2 + m->m_E;
    const size_t H = size_t(m->m_H), I = size_t(m->m_I), V = size_t(m->m_V), E = size_t(m->m_E);
    r.floats(m->m_emb, (V + 1) * E);
    r.floats(m->m_wih, 3 * H * I);
    r.floats(m->m_whh, 3 * H * H);
    r.floats(m->m_bih, 3 * H);
    r.floats(m->m_bhh, 3 * H);
    r.floats(m->m_wout, V * H);
    r.floats(m->m_bout, V);
    if (!r.ok || r.pos + 4 != bytes.size()) { setError(error, "model data is truncated or invalid"); return nullptr; }
    return m;
}

size_t PatternModel::parameterCount() const {
    return m_emb.size() + m_wih.size() + m_whh.size() + m_bih.size() + m_bhh.size() + m_wout.size() + m_bout.size();
}

float PatternModel::temperature(gen::Style style, float intensity) {
    float t = 1.0f;
    switch (style) {
        case gen::Style::Chaotic: t = 1.5f; break;
        case gen::Style::Minimal: t = 0.7f; break;
        case gen::Style::Tight: t = 0.8f; break;
        case gen::Style::Memory: t = 1.1f; break;
        default: break;
    }
    // calm moments stay closer to the model's favourite, intense ones explore more
    return t * (0.85f + 0.3f * std::clamp(intensity, 0.0f, 1.0f));
}

void PatternModel::stepInput(const float* f, int style, float density, int prevToken, float* out) const {
    int k = 0;
    for (int i = 0; i < m_F; ++i) out[k++] = f[i];
    for (int s = 0; s < m_S; ++s) out[k++] = s == style ? 1.0f : 0.0f;
    out[k++] = density < 0 ? 0.5f : std::clamp(density, 0.0f, 1.0f);
    out[k++] = density < 0 ? 1.0f : 0.0f;
    int row = (prevToken >= 0 && prevToken < m_V) ? prevToken : m_V;
    const float* e = m_emb.data() + size_t(row) * size_t(m_E);
    for (int i = 0; i < m_E; ++i) out[k++] = e[i];
}

void PatternModel::forward(const std::vector<std::vector<float>>& inputs, std::vector<float>& out) const {
    const size_t H = size_t(m_H), I = size_t(m_I);
    std::vector<float> h(H, 0.0f), gi(3 * H), gh(3 * H), hn(H);
    for (const auto& x : inputs) {
        for (size_t g = 0; g < 3 * H; ++g) {
            const float* wi = m_wih.data() + g * I;
            float a = m_bih[g];
            for (size_t i = 0; i < I; ++i) a += wi[i] * x[i];
            gi[g] = a;
            const float* wh = m_whh.data() + g * H;
            float b = m_bhh[g];
            for (size_t j = 0; j < H; ++j) b += wh[j] * h[j];
            gh[g] = b;
        }
        for (size_t j = 0; j < H; ++j) {
            float r = sigmoid(gi[j] + gh[j]);
            float z = sigmoid(gi[H + j] + gh[H + j]);
            float n = std::tanh(gi[2 * H + j] + r * gh[2 * H + j]);
            hn[j] = (1.0f - z) * n + z * h[j];
        }
        h.swap(hn);
    }
    out.assign(size_t(m_V), 0.0f);
    for (size_t v = 0; v < size_t(m_V); ++v) {
        const float* w = m_wout.data() + v * H;
        float a = m_bout[v];
        for (size_t j = 0; j < H; ++j) a += w[j] * h[j];
        out[v] = a;
    }
}

void PatternModel::logits(const gen::PatternContext& ctx, std::vector<float>& out) const {
    std::vector<std::vector<float>> steps;
    const int style = int(ctx.style);
    size_t n = ctx.history ? ctx.history->size() : 0;
    size_t start = n > size_t(m_W) ? n - size_t(m_W) : 0;
    int prev = start > 0 ? (*ctx.history)[start - 1].token : -1;
    for (size_t i = start; i < n; ++i) {
        const auto& hs = (*ctx.history)[i];
        std::vector<float> x(static_cast<size_t>(m_I));
        stepInput(hs.f, style, ctx.density, prev, x.data());
        steps.push_back(std::move(x));
        prev = hs.token;
    }
    float f[gen::kContextFeatures];
    gen::contextFeatures(ctx, f);
    std::vector<float> x(static_cast<size_t>(m_I));
    stepInput(f, style, ctx.density, prev, x.data());
    steps.push_back(std::move(x));
    forward(steps, out);
}

void PatternModel::score(const gen::PatternContext& ctx, const std::vector<const gen::PatternDef*>& cands, std::vector<float>& w) const {
    w.assign(cands.size(), 0.0f);
    if (cands.empty()) return;
    std::vector<float> lg;
    logits(ctx, lg);
    const float T = temperature(ctx.style, ctx.intensity);
    float mx = -1e30f;
    std::vector<float> l(cands.size(), -1e30f);
    for (size_t i = 0; i < cands.size(); ++i) {
        int tok = gen::patternToken(cands[i]);
        if (tok >= 0 && tok < m_V) l[i] = lg[size_t(tok)] / T;
        mx = std::max(mx, l[i]);
    }
    for (size_t i = 0; i < cands.size(); ++i) w[i] = l[i] > -1e29f ? std::exp(l[i] - mx) : 0.0f;
    // variety safety net: the same pattern three times in a row is damped like the rules do
    for (size_t i = 0; i < cands.size(); ++i)
        if (cands[i]->id == ctx.prevPattern && cands[i]->id == ctx.prevPattern2) w[i] *= 0.2f;
}

// ---------------------------------------------------------------- set

int ModelSet::loadDirectory(const std::string& dir, std::string* error) {
    namespace fs = std::filesystem;
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".alm") continue;
        std::string err;
        auto m = PatternModel::load(e.path().string(), &err);
        if (!m) { if (error) *error += e.path().filename().string() + ": " + err + "\n"; continue; }
        add(std::move(m));
        ++n;
    }
    return n;
}

void ModelSet::add(std::unique_ptr<PatternModel> m) { m_models.push_back(std::move(m)); }

const PatternModel* ModelSet::forStyle(gen::Style s) const {
    const PatternModel* general = nullptr;
    for (auto& m : m_models) {
        if (m->info().style == int(s)) return m.get();
        if (m->info().style < 0 && !general) general = m.get();
    }
    return general ? general : (m_models.empty() ? nullptr : m_models.front().get());
}

void ModelSet::score(const gen::PatternContext& ctx, const std::vector<const gen::PatternDef*>& cands, std::vector<float>& w) const {
    if (const auto* m = forStyle(ctx.style)) m->score(ctx, cands, w);
    else gen::RuleStyleModel().score(ctx, cands, w);
}

std::string ModelSet::name() const {
    std::string s = "models[";
    for (size_t i = 0; i < m_models.size(); ++i) s += (i ? "," : "") + m_models[i]->info().name;
    return s + "]";
}

} // namespace al::model
