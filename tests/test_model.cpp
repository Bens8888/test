// Model tests: PyTorch/C++ parity (input layout and logits, on a fixture exported by
// training/make_test_fixture.py), file validation, and generation with a model plugged in.
#include "test.hpp"

#include <algorithm>
#include <al/audio/analysis.hpp>
#include <al/audio/synth.hpp>
#include <al/gen/generator.hpp>
#include <al/model/model.hpp>
#include <al/util/codec.hpp>
#include <al/util/json.hpp>

#include <chrono>

using namespace al;

namespace {
std::string fixture(const char* name) { return std::string(AL_SOURCE_DIR) + "/tests/data/" + name; }
} // namespace

TEST(model_pytorch_parity) {
    std::string err;
    auto m = model::PatternModel::load(fixture("model_fixture.alm"), &err);
    if (!m) std::printf("    %s\n", err.c_str());
    REQUIRE(m != nullptr);
    std::string text;
    REQUIRE(readFile(fixture("model_fixture_parity.json"), text));
    auto j = Json::parse(text);
    REQUIRE(j.has_value());
    double maxLogit = 0, maxInput = 0;
    int cases = 0;
    for (const Json& c : j->items()) {
        // 1. the step inputs built by C++ equal PyTorch's
        const Json& feats = c["features"];
        const Json& prev = c["prev"];
        std::vector<std::vector<float>> inputs;
        for (size_t t = 0; t < feats.size(); ++t) {
            float f[gen::kContextFeatures];
            for (int i = 0; i < gen::kContextFeatures; ++i) f[i] = float(feats[t][size_t(i)].asNumber());
            std::vector<float> x(static_cast<size_t>(m->inputSize()));
            m->stepInput(f, c.get("style", 0), float(c.get("density", -1.0)), prev[t].asInt(), x.data());
            for (size_t i = 0; i < x.size(); ++i) maxInput = std::max(maxInput, std::fabs(double(x[i]) - c["inputs"][t][i].asNumber()));
            inputs.push_back(std::move(x));
        }
        // 2. the logits of the last step
        std::vector<float> logits;
        m->forward(inputs, logits);
        const Json& ref = c["logits"];
        REQUIRE(logits.size() == ref.size());
        for (size_t v = 0; v < logits.size(); ++v) maxLogit = std::max(maxLogit, std::fabs(double(logits[v]) - ref[v].asNumber()));
        ++cases;
    }
    std::printf("    %d cases, %zu parameters: max |input diff| %.2e, max |logit diff| %.2e\n", cases, m->parameterCount(), maxInput, maxLogit);
    CHECK(cases >= 10);
    CHECK(maxInput < 1e-6);
    CHECK(maxLogit < 1e-4);
}

TEST(model_file_validation) {
    std::string bytes, err;
    REQUIRE(readFile(fixture("model_fixture.alm"), bytes));
    CHECK(model::PatternModel::fromBytes(bytes, &err) != nullptr);
    std::string bad = bytes;
    bad[bad.size() / 2] ^= 0x10;
    CHECK(model::PatternModel::fromBytes(bad, &err) == nullptr);
    CHECK(err.find("checksum") != std::string::npos);
    CHECK(model::PatternModel::fromBytes("nonsense", &err) == nullptr);
    CHECK(model::PatternModel::fromBytes(bytes.substr(0, 100), &err) == nullptr);
}

TEST(model_drives_generation) {
    auto m = model::PatternModel::load(fixture("model_fixture.alm"));
    REQUIRE(m != nullptr);
    auto A = audio::analyze(audio::render(audio::randomSong(5)));
    gen::GenOptions opt;
    opt.difficulty = gen::Difficulty::Hard;
    opt.seed = 3;
    auto t0 = std::chrono::steady_clock::now();
    auto r = gen::generateVariant(A, opt, 3, m.get());
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto rr = gen::generateVariant(A, opt, 3, nullptr);
    std::printf("    with the (random) model: %d clicks, %d dropped, %.2f s, %s; rules: %d clicks\n", r.report.clicks, r.report.dropped, sec,
                r.report.intendedInputs ? "designed inputs win" : "NEEDS BOT", rr.report.clicks);
    CHECK(r.report.beatable);
    CHECK(r.report.intendedInputs);
    CHECK(r.model.find("model:") == 0);
    // the model changes the choices
    int differ = 0, n = 0;
    for (size_t i = 0; i < std::min(r.clicks.size(), rr.clicks.size()); ++i) {
        if (!r.clicks[i].decision || !rr.clicks[i].decision) continue;
        ++n;
        differ += r.clicks[i].pattern != rr.clicks[i].pattern ? 1 : 0;
    }
    CHECK(n > 10);
    CHECK(differ > 0);
}
