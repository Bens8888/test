#include <al/rec/taps.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace al::rec {

namespace {

constexpr double kTapLength = 0.05;   // a plain tap's press length
constexpr double kHoldLine = 0.15;    // presses at least this long are holds (as in fusion)
constexpr size_t kClockWindow = 90;   // about 1.5 s of frames
constexpr double kClockJump = 0.15;   // offset change that means a seek or a stall

void sortTaps(std::vector<Tap>& v) {
    std::stable_sort(v.begin(), v.end(), [](const Tap& a, const Tap& b) { return a.time < b.time; });
}

} // namespace

void TapTrack::merge(std::vector<Tap> take, double mergeSec) {
    for (auto& t : take) t.release = std::max(t.release, t.time + 0.01);
    sortTaps(take);
    m_history.push_back(m_taps);
    std::vector<Tap> kept;
    kept.reserve(m_taps.size() + take.size());
    for (const auto& old : m_taps) {
        auto it = std::lower_bound(take.begin(), take.end(), old.time - mergeSec, [](const Tap& a, double v) { return a.time < v; });
        if (it != take.end() && it->time <= old.time + mergeSec) continue; // re-tapped: the new one wins
        kept.push_back(old);
    }
    kept.insert(kept.end(), take.begin(), take.end());
    sortTaps(kept);
    m_taps = std::move(kept);
}

bool TapTrack::undoLastTake() {
    if (m_history.empty()) return false;
    m_taps = std::move(m_history.back());
    m_history.pop_back();
    return true;
}

void TapTrack::clear() {
    m_taps.clear();
    m_history.clear();
}

Recording TapTrack::toRecording(const std::string& levelName) const {
    Recording r;
    r.levelName = levelName;
    r.source = "taps";
    r.completed = true;
    r.songOffset = 0;
    for (size_t i = 0; i < m_taps.size(); ++i) {
        const Tap& t = m_taps[i];
        double release = std::max(t.release, t.time + 0.01);
        if (i + 1 < m_taps.size()) release = std::min(release, std::max(t.time + 0.005, m_taps[i + 1].time - 0.005));
        RecEvent down;
        down.time = t.time;
        down.tick = int32_t(std::lround(t.time * r.tps));
        down.press = true;
        RecEvent up = down;
        up.time = release;
        up.tick = std::max(down.tick, int32_t(std::lround(release * r.tps)));
        up.press = false;
        r.events.push_back(down);
        r.events.push_back(up);
        r.duration = release;
    }
    return r;
}

std::string TapTrack::toGuidelines(double songOffset) const {
    std::vector<std::pair<double, double>> lines;
    for (const auto& t : m_taps) {
        lines.push_back({t.time - songOffset, 0.8});
        if (t.release - t.time >= kHoldLine) lines.push_back({t.release - songOffset, 0.9});
    }
    std::sort(lines.begin(), lines.end());
    std::string s;
    char buf[48];
    for (auto& [time, color] : lines) {
        if (time < 0) continue; // before the level starts: GD cannot show it
        std::snprintf(buf, sizeof(buf), "%.4f~%.1f~", time, color);
        s += buf;
    }
    return s;
}

std::vector<Tap> TapTrack::fromGuidelines(const std::string& g, double songOffset) {
    std::vector<double> nums;
    size_t pos = 0;
    while (pos <= g.size()) {
        size_t e = g.find('~', pos);
        if (e == std::string::npos) e = g.size();
        if (e > pos) {
            std::string part = g.substr(pos, e - pos);
            char* end = nullptr;
            double v = std::strtod(part.c_str(), &end);
            if (end && end != part.c_str() && std::isfinite(v)) nums.push_back(v);
            else nums.push_back(-1);
        }
        pos = e + 1;
    }
    std::vector<Tap> taps;
    for (size_t i = 0; i + 1 < nums.size(); i += 2) {
        const double time = nums[i], color = nums[i + 1];
        if (time < 0) continue;
        // a yellow line right after a tap marks the end of a hold (written by toGuidelines)
        if (std::fabs(color - 0.9) < 0.01 && !taps.empty() && time - taps.back().time >= kHoldLine - 1e-6 && time - taps.back().time < 4.0 &&
            taps.back().release - taps.back().time < kHoldLine) {
            taps.back().release = time + songOffset;
            continue;
        }
        Tap t;
        t.time = time + songOffset;
        t.release = t.time + kTapLength;
        taps.push_back(t);
    }
    sortTaps(taps);
    return taps;
}

Json TapTrack::toJson() const {
    Json j = Json::object();
    j["format"] = "autolayout-taps";
    j["version"] = 1;
    j["takes"] = takes();
    Json arr = Json::array();
    for (const auto& t : m_taps) {
        Json e = Json::array();
        e.push(std::round(t.time * 10000.0) / 10000.0);
        e.push(std::round(t.release * 10000.0) / 10000.0);
        arr.push(e);
    }
    j["taps"] = arr;
    return j;
}

std::optional<TapTrack> TapTrack::fromJson(const Json& j) {
    if (j.get("format", "") != std::string("autolayout-taps")) return std::nullopt;
    TapTrack t;
    for (const auto& e : j["taps"].items()) {
        if (!e.isArray() || e.size() < 1 || !e[0].isNumber()) continue;
        Tap tap;
        tap.time = e[0].asNumber();
        tap.release = e.size() >= 2 && e[1].isNumber() ? e[1].asNumber() : tap.time + kTapLength;
        tap.release = std::max(tap.release, tap.time + 0.01);
        t.m_taps.push_back(tap);
    }
    sortTaps(t.m_taps);
    return t;
}

void SongClock::reset() {
    m_window.clear();
    m_offset = 0;
}

void SongClock::sample(double clockSec, double songSec) {
    const double off = songSec - clockSec;
    if (!m_window.empty() && std::fabs(off - m_offset) > kClockJump) m_window.clear();
    m_window.push_back(off);
    if (m_window.size() > kClockWindow) m_window.erase(m_window.begin());
    std::vector<double> s = m_window;
    std::nth_element(s.begin(), s.begin() + long(s.size() / 2), s.end());
    m_offset = s[s.size() / 2];
}

} // namespace al::rec
