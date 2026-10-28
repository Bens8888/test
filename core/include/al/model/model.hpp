#pragma once
// Trained pattern model: a small GRU sequence model over the level's pattern decisions, run by a
// hand-written inference engine (see docs/DECISIONS.md D8). Per decision the input is the context
// features (music, click, gamemode state, difficulty), the style one-hot, the density and the
// embedding of the previous decision's pattern; the output is a logit per pattern token. Only the
// candidates the builder offers (already valid for the situation) are scored, so the model can
// never pick something the rules forbid, and the builder still verifies every placement with the
// simulator.
#include <al/gen/style.hpp>

#include <memory>
#include <string>
#include <vector>

namespace al::model {

struct ModelInfo {
    std::string name;            // e.g. "all", "flow"
    int style = -1;              // style the model was fine-tuned for (-1 = all styles)
    std::string metadata;        // JSON: training data, epochs, accuracy, hardware, time
};

class PatternModel final : public gen::StyleModel {
public:
    static constexpr uint32_t kVersion = 1;
    static std::unique_ptr<PatternModel> load(const std::string& path, std::string* error = nullptr);
    static std::unique_ptr<PatternModel> fromBytes(const std::string& bytes, std::string* error = nullptr);

    // StyleModel: softmax over the candidates' logits with a temperature tied to style/intensity.
    void score(const gen::PatternContext& ctx, const std::vector<const gen::PatternDef*>& candidates, std::vector<float>& weights) const override;
    std::string name() const override { return "model:" + m_info.name; }

    // Logits for the current step, given the history (oldest first, at most window() used).
    void logits(const gen::PatternContext& ctx, std::vector<float>& out) const;
    // Raw forward pass on explicit input vectors (one per step, inputSize() floats): logits of
    // the last step. Used by the parity test.
    void forward(const std::vector<std::vector<float>>& inputs, std::vector<float>& out) const;
    // Input vector of a step (features + style one-hot + density + previous-token embedding).
    void stepInput(const float* features, int style, float density, int prevToken, float* out) const;

    const ModelInfo& info() const { return m_info; }
    int inputSize() const { return m_I; }
    int hidden() const { return m_H; }
    int vocab() const { return m_V; }
    int window() const { return m_W; }
    size_t parameterCount() const;
    static float temperature(gen::Style style, float intensity);

private:
    ModelInfo m_info;
    int m_F = 0, m_S = 0, m_E = 0, m_I = 0, m_H = 0, m_V = 0, m_W = 16;
    std::vector<float> m_emb;            // (V + 1) x E   (row V = "no previous decision")
    std::vector<float> m_wih, m_whh;     // 3H x I, 3H x H (gates r, z, n)
    std::vector<float> m_bih, m_bhh;     // 3H
    std::vector<float> m_wout, m_bout;   // V x H, V
};

// The models shipped with the mod (or retrained by the user): one general model plus optional
// per-style fine-tunes; picks the style's model when present.
class ModelSet final : public gen::StyleModel {
public:
    // Loads every *.alm in `dir` (model_all.alm, model_flow.alm, ...). Returns the number loaded.
    int loadDirectory(const std::string& dir, std::string* error = nullptr);
    void add(std::unique_ptr<PatternModel> m);
    bool empty() const { return m_models.empty(); }
    const PatternModel* forStyle(gen::Style s) const;
    void score(const gen::PatternContext& ctx, const std::vector<const gen::PatternDef*>& candidates, std::vector<float>& weights) const override;
    std::string name() const override;
    const std::vector<std::unique_ptr<PatternModel>>& models() const { return m_models; }

private:
    std::vector<std::unique_ptr<PatternModel>> m_models;
};

} // namespace al::model
