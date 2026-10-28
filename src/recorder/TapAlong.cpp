#include "TapAlong.hpp"

#include <Geode/utils/file.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace al::mod {

namespace {
constexpr int kChannel = 0; // the main music channel
} // namespace

TapAlong& TapAlong::get() {
    static TapAlong t;
    return t;
}

std::filesystem::path TapAlong::folder() {
    auto dir = Mod::get()->getSaveDir() / "taps";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string TapAlong::keyFor(LevelEditorLayer* lel) const {
    if (!lel || !lel->m_level) return "unknown";
    int id = int(lel->m_level->m_levelID.value());
    if (id > 0) return fmt::format("id{}", id);
    std::string name = std::string(lel->m_level->m_levelName);
    for (char& c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    return "local_" + (name.empty() ? std::string("level") : name);
}

double TapAlong::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_epoch).count();
}

rec::TapTrack& TapAlong::track(LevelEditorLayer* lel) {
    m_editor = lel;
    const std::string key = keyFor(lel);
    if (key == m_key) return m_track;
    m_key = key;
    m_track = rec::TapTrack{};
    auto path = folder() / (key + ".json");
    bool loaded = false;
    if (std::filesystem::exists(path)) {
        if (auto text = file::readString(path)) {
            if (auto j = al::Json::parse(text.unwrap())) {
                if (auto t = rec::TapTrack::fromJson(*j)) {
                    m_track = std::move(*t);
                    loaded = true;
                }
            }
        }
    }
    if (!loaded && lel && lel->m_levelSettings) {
        std::string g = lel->m_levelSettings->m_guidelineString;
        auto taps = rec::TapTrack::fromGuidelines(g, lel->m_levelSettings->m_songOffset);
        if (!taps.empty()) {
            m_track.merge(std::move(taps));
            log::info("AutoLayout tap along: {} taps taken from the level's guidelines", m_track.taps().size());
        }
    }
    return m_track;
}

std::optional<rec::Recording> TapAlong::recordingFor(LevelEditorLayer* lel) {
    auto& t = track(lel);
    if (t.empty()) return std::nullopt;
    auto r = t.toRecording(lel && lel->m_level ? std::string(lel->m_level->m_levelName) : std::string());
    if (lel && lel->m_level) r.levelId = int32_t(lel->m_level->m_levelID.value());
    return r;
}

void TapAlong::save() {
    if (m_key.empty()) return;
    auto path = folder() / (m_key + ".json");
    if (auto res = file::writeString(path, m_track.toJson().dump(1)); !res)
        log::error("AutoLayout tap along: cannot save {}: {}", geode::utils::string::pathToString(path), res.unwrapErr());
}

void TapAlong::showGuidelines(LevelEditorLayer* lel) {
    if (!lel || !lel->m_levelSettings) return;
    auto st = lel->m_levelSettings;
    st->m_guidelineString = track(lel).toGuidelines(st->m_songOffset);
    st->m_guidelinesUpdated = true;
}

geode::Result<> TapAlong::start(LevelEditorLayer* lel, std::filesystem::path const& song, double startSec) {
    if (m_recording) return Ok();
    if (!lel || !lel->m_levelSettings) return Err("open the level in the editor first");
    if (!std::filesystem::exists(song)) return Err("the song file is missing: download the song first");
    track(lel);
    const double songOffset = lel->m_levelSettings->m_songOffset;
    m_startSong = std::max(0.0, songOffset + std::max(0.0, startSec));
    auto fm = FMODAudioEngine::sharedEngine();
    fm->stopAllMusic(true);
    fm->playMusic(geode::utils::string::pathToString(song), false, 0.f, kChannel);
    fm->setMusicTimeMS(unsigned(std::lround(m_startSong * 1000.0)), true, kChannel);
    m_clock.reset();
    m_take.clear();
    m_down = false;
    m_lastSongPos = -1;
    m_stalledFrames = 0;
    m_startClock = now();
    m_recording = true;
    log::info("AutoLayout tap along: recording from {:.2f} s of the song", m_startSong);
    return Ok();
}

bool TapAlong::update() {
    if (!m_recording) return false;
    auto fm = FMODAudioEngine::sharedEngine();
    const double pos = fm->getMusicTimeMS(kChannel) / 1000.0;
    const double t = now();
    // the engine needs a moment to load and seek: the clock starts once the position moves on
    // from where playback started
    if (m_lastSongPos >= 0 && pos != m_lastSongPos && pos >= m_startSong - 0.05) {
        m_clock.sample(t, pos);
        m_stalledFrames = 0;
    } else if (m_clock.valid()) {
        ++m_stalledFrames;
    }
    m_lastSongPos = pos;
    // the end of the song (the position stops for half a second or so)
    const double length = fm->getMusicLengthMS(kChannel) / 1000.0;
    const bool atEnd = length > 1.0 && songTime() >= length - 0.05;
    if ((m_clock.valid() && m_stalledFrames > 30) || atEnd) {
        stop();
        return false;
    }
    return true;
}

double TapAlong::songTime() const {
    if (m_clock.valid()) return m_clock.songTimeAt(now());
    return m_startSong;
}

void TapAlong::press() {
    if (!m_recording || m_down) return; // key repeat
    if (!m_clock.valid()) return;       // the song has not started yet
    const double t = songTime();
    m_take.push_back({t, t + 0.05});
    m_down = true;
}

void TapAlong::release() {
    if (!m_recording || !m_down) return;
    m_down = false;
    if (m_take.empty()) return;
    m_take.back().release = std::max(m_take.back().time + 0.01, songTime());
}

int TapAlong::stop() {
    if (!m_recording) return 0;
    release();
    m_recording = false;
    FMODAudioEngine::sharedEngine()->stopAllMusic(true);
    const int n = int(m_take.size());
    if (n > 0) {
        m_track.merge(std::move(m_take));
        save();
        showGuidelines(m_editor);
    }
    m_take.clear();
    log::info("AutoLayout tap along: take of {} taps, track has {}", n, m_track.taps().size());
    return n;
}

void TapAlong::clear(LevelEditorLayer* lel) {
    track(lel).clear();
    save();
    showGuidelines(lel);
}

bool TapAlong::undoTake(LevelEditorLayer* lel) {
    bool ok = track(lel).undoLastTake();
    if (ok) {
        save();
        showGuidelines(lel);
    }
    return ok;
}

} // namespace al::mod
