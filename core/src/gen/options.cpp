#include <al/gen/options.hpp>

#include <algorithm>
#include <cmath>

namespace al::gen {

const char* difficultyName(Difficulty d) {
    switch (d) {
        case Difficulty::Easy: return "Easy";
        case Difficulty::Normal: return "Normal";
        case Difficulty::Hard: return "Hard";
        case Difficulty::Harder: return "Harder";
        case Difficulty::Insane: return "Insane";
        case Difficulty::EasyDemon: return "Easy Demon";
        case Difficulty::MediumDemon: return "Medium Demon";
        case Difficulty::HardDemon: return "Hard Demon";
        case Difficulty::InsaneDemon: return "Insane Demon";
        case Difficulty::ExtremeDemon: return "Extreme Demon";
        case Difficulty::Auto: return "Auto";
    }
    return "?";
}

const char* styleName(Style s) {
    switch (s) {
        case Style::Auto: return "Auto";
        case Style::Flow: return "Flow";
        case Style::Tight: return "Tight";
        case Style::Chaotic: return "Chaotic";
        case Style::Minimal: return "Minimal";
        case Style::Retro: return "Retro";
        case Style::Modern: return "Modern";
        case Style::Memory: return "Memory";
        default: return "?";
    }
}

static const DifficultyProfile kProfiles[10] = {
    //  calm  intense  window  clear  gap   spikes orbs  triple speed
    {0.55f, 1.10f, 230.f, 48.f, 0.44f, 1, false, false, 0.0f},  // Easy
    {0.70f, 1.45f, 200.f, 40.f, 0.40f, 2, true, false, 0.1f},   // Normal
    {0.85f, 1.85f, 170.f, 34.f, 0.32f, 2, true, false, 0.2f},   // Hard
    {1.00f, 2.30f, 145.f, 28.f, 0.26f, 3, true, true, 0.35f},   // Harder
    {1.20f, 2.90f, 125.f, 23.f, 0.21f, 3, true, true, 0.5f},    // Insane
    {1.45f, 3.40f, 108.f, 19.f, 0.18f, 3, true, true, 0.6f},    // Easy demon
    {1.70f, 4.00f, 93.f, 15.f, 0.15f, 3, true, true, 0.7f},     // Medium demon
    {2.00f, 4.70f, 78.f, 12.f, 0.13f, 3, true, true, 0.8f},     // Hard demon
    {2.30f, 5.60f, 63.f, 9.f, 0.11f, 3, true, true, 0.9f},      // Insane demon
    {2.70f, 6.80f, 48.f, 6.f, 0.09f, 3, true, true, 1.0f},      // Extreme demon
};

const DifficultyProfile& profile(Difficulty d) {
    int i = d == Difficulty::Auto ? 4 : std::clamp(int(d), 0, 9);
    return kProfiles[i];
}

DifficultyProfile profileAt(float d) {
    d = std::clamp(d, 0.0f, 9.0f);
    int a = int(std::floor(d));
    int b = std::min(9, a + 1);
    float f = d - float(a);
    const auto& A = kProfiles[a];
    const auto& B = kProfiles[b];
    auto L = [f](float x, float y) { return x + (y - x) * f; };
    DifficultyProfile p = f < 0.5f ? A : B;
    p.clicksCalm = L(A.clicksCalm, B.clicksCalm);
    p.clicksIntense = L(A.clicksIntense, B.clicksIntense);
    p.minWindowMs = L(A.minWindowMs, B.minWindowMs);
    p.clearance = L(A.clearance, B.clearance);
    p.minClickGap = L(A.minClickGap, B.minClickGap);
    p.speedBias = L(A.speedBias, B.speedBias);
    return p;
}

Json GenOptions::toJson() const {
    Json j = Json::object();
    j["version"] = 1;
    j["difficulty"] = int(difficulty);
    j["density"] = density;
    j["style"] = int(style);
    Json mw = Json::array();
    for (float w : modeWeights) mw.push(w);
    j["modeWeights"] = mw;
    j["speedChanges"] = int(speedChanges);
    j["startSpeed"] = startSpeed;
    j["portalFrequency"] = portalFrequency;
    j["gravityFlips"] = int(gravityFlips);
    j["sizeChanges"] = int(sizeChanges);
    j["dual"] = int(dual);
    j["holdBias"] = holdBias;
    Json so = Json::array();
    for (auto& o : sectionOverrides) {
        Json e;
        e["sectionType"] = o.sectionType;
        e["difficultyDelta"] = o.difficultyDelta;
        e["forcedMode"] = o.forcedMode;
        so.push(e);
    }
    j["sectionOverrides"] = so;
    j["seed"] = double(seed);
    j["variants"] = variants;
    j["input"] = int(input);
    j["faithfulness"] = faithfulness;
    j["engine"] = engine;
    j["switchRate"] = switchRate;
    j["look"] = look;
    return j;
}

GenOptions GenOptions::fromJson(const Json& j) {
    GenOptions o;
    auto d = j.get("difficulty", int(o.difficulty));
    o.difficulty = (d == 255 || (d >= 0 && d <= 9)) ? Difficulty(d) : Difficulty::Auto;
    o.density = float(j.get("density", double(o.density)));
    o.style = Style(std::clamp(j.get("style", 0), 0, int(Style::Count) - 1));
    const Json& mw = j["modeWeights"];
    for (size_t i = 0; i < o.modeWeights.size() && i < mw.size(); ++i) o.modeWeights[i] = float(mw[i].asNumber(-1));
    o.speedChanges = SpeedChanges(std::clamp(j.get("speedChanges", 0), 0, 3));
    o.startSpeed = std::clamp(j.get("startSpeed", -1), -1, 4);
    o.portalFrequency = float(j.get("portalFrequency", -1.0));
    o.gravityFlips = Tri(std::clamp(j.get("gravityFlips", 0), 0, 2));
    o.sizeChanges = Tri(std::clamp(j.get("sizeChanges", 0), 0, 2));
    o.dual = Tri(std::clamp(j.get("dual", 0), 0, 2));
    o.holdBias = float(std::clamp(j.get("holdBias", 0.0), -1.0, 1.0));
    for (auto& e : j["sectionOverrides"].items()) {
        SectionOverride so;
        so.sectionType = e.get("sectionType", -1);
        so.difficultyDelta = std::clamp(e.get("difficultyDelta", 0), -5, 5);
        so.forcedMode = std::clamp(e.get("forcedMode", -1), -1, kModeCount - 1);
        o.sectionOverrides.push_back(so);
    }
    o.seed = uint64_t(j.get("seed", 1.0));
    o.variants = std::clamp(j.get("variants", 5), 1, 16);
    o.input = InputSource(std::clamp(j.get("input", 0), 0, 1));
    o.faithfulness = float(std::clamp(j.get("faithfulness", 0.8), 0.0, 1.0));
    o.engine = std::clamp(j.get("engine", 2), 1, 2);
    o.switchRate = float(std::clamp(j.get("switchRate", -1.0), -1.0, 1.0));
    o.look = std::clamp(j.get("look", 0), 0, 1);
    return o;
}

} // namespace al::gen
