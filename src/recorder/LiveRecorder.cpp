#include "LiveRecorder.hpp"

#include <al/level/objects.hpp>
#include <al/util/rng.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

using namespace geode::prelude;

namespace al::mod {

LiveRecorder& LiveRecorder::get() {
    static LiveRecorder r;
    return r;
}

void LiveRecorder::setArmed(bool armed) {
    m_armed = armed;
    if (!armed && m_stitcher.active()) endSession(GJBaseGameLayer::get(), false);
    Notification::create(armed ? "AutoLayout: click recording ON (starts with the next attempt)" : "AutoLayout: click recording OFF",
                         armed ? NotificationIcon::Info : NotificationIcon::None)
        ->show();
}

void LiveRecorder::toggle() { setArmed(!m_armed); }

int32_t LiveRecorder::tickOf(GJBaseGameLayer* layer) {
    if (!layer) return 0;
    return int32_t(layer->m_gameState.m_currentProgress);
}

rec::PlayerSnap LiveRecorder::snap(PlayerObject* p) {
    rec::PlayerSnap s;
    if (!p) return s;
    auto pos = p->getPosition();
    s.x = pos.x;
    s.y = pos.y;
    s.yVel = float(p->m_yVelocity);
    level::Mode m = level::Mode::Cube;
    if (p->m_isShip) m = level::Mode::Ship;
    else if (p->m_isBall) m = level::Mode::Ball;
    else if (p->m_isBird) m = level::Mode::Ufo;
    else if (p->m_isDart) m = level::Mode::Wave;
    else if (p->m_isRobot) m = level::Mode::Robot;
    else if (p->m_isSpider) m = level::Mode::Spider;
    else if (p->m_isSwing) m = level::Mode::Swing;
    s.mode = uint8_t(m);
    // m_playerSpeed: 0.7, 0.9, 1.1, 1.3, 1.6 for 0.5x .. 4x
    static const float speeds[5] = {0.7f, 0.9f, 1.1f, 1.3f, 1.6f};
    int best = 1;
    for (int i = 0; i < 5; ++i)
        if (std::fabs(p->m_playerSpeed - speeds[i]) < std::fabs(p->m_playerSpeed - speeds[best])) best = i;
    s.speed = uint8_t(best);
    s.mini = p->m_vehicleSize < 0.9f;
    s.upsideDown = p->m_isUpsideDown;
    s.onGround = p->m_isOnGround;
    return s;
}

void LiveRecorder::beginSession(GJBaseGameLayer* layer, GJGameLevel* level, bool editorPlaytest) {
    if (!m_armed || !layer || !level) return;
    m_editor = editorPlaytest;
    std::string levelString = level->m_levelString;
    float offset = layer->m_levelSettings ? layer->m_levelSettings->m_songOffset : 0.f;
    m_stitcher.begin(std::string(level->m_levelName), int32_t(level->m_levelID.value()), levelString.empty() ? 0 : hashStr(levelString), offset);
    m_songMinusLevel.clear();
    // Native click-between-steps splits steps: off while recording so every input lands on a
    // whole physics step (deterministic replay in the simulator).
    layer->m_clickBetweenSteps = false;
    log::info("AutoLayout recorder: session started ({})", editorPlaytest ? "editor playtest" : "play");
}

void LiveRecorder::onButton(GJBaseGameLayer* layer, bool down, int button, bool isPlayer1) {
    if (!m_stitcher.active() || !layer) return;
    rec::RecEvent e;
    e.tick = tickOf(layer);
    e.time = layer->m_gameState.m_levelTime;
    e.button = rec::Button(std::clamp(button, 1, 3));
    e.press = down;
    e.player2 = !isPlayer1;
    e.p1 = snap(layer->m_player1);
    e.dual = layer->m_gameState.m_isDualMode;
    if (e.dual) e.p2 = snap(layer->m_player2);
    m_stitcher.onButton(e);
    sampleSongOffset(layer);
}

void LiveRecorder::sampleSongOffset(GJBaseGameLayer* layer) {
    if (!layer || layer->m_isPracticeMode) return; // practice mode plays the practice song
    auto fm = FMODAudioEngine::sharedEngine();
    if (!fm || !fm->isMusicPlaying(0)) return;
    const double song = fm->getMusicTimeMS(0) / 1000.0;
    if (song <= 0) return;
    m_songMinusLevel.push_back(song - layer->m_gameState.m_levelTime);
}

void LiveRecorder::onOrb(GJBaseGameLayer* layer, int objectId) {
    if (!m_stitcher.active()) return;
    m_stitcher.onOrb(tickOf(layer), objectId);
}

void LiveRecorder::onCheckpoint(int32_t tick) { m_stitcher.onCheckpoint(tick); }
void LiveRecorder::onCheckpointRemoved() { m_stitcher.onCheckpointRemoved(); }

void LiveRecorder::onRespawn(GJBaseGameLayer* layer, int32_t tick) {
    if (!m_stitcher.active() || !layer) return;
    // after respawning GD does not keep the button held; the stitched path continues released
    m_stitcher.onRespawn(tick, false, false);
}

void LiveRecorder::onRestart() { m_stitcher.onRestart(); }
void LiveRecorder::onDeath(int32_t tick) { m_stitcher.onDeath(tick); }

void LiveRecorder::onComplete(GJBaseGameLayer* layer) {
    if (!m_stitcher.active() || !layer) return;
    m_stitcher.onComplete(tickOf(layer), layer->m_gameState.m_levelTime);
    endSession(layer, true);
}

void LiveRecorder::endSession(GJBaseGameLayer* layer, bool completed) {
    if (!m_stitcher.active()) return;
    if (!completed && layer) m_stitcher.onComplete(tickOf(layer), layer->m_gameState.m_levelTime);
    rec::Recording r = m_stitcher.result();
    if (m_songMinusLevel.size() >= 3) {
        auto v = m_songMinusLevel;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        const double measured = v[v.size() / 2];
        // the level's song offset is exact when the song started at the level's start; a clearly
        // different measurement means it started elsewhere (start position, playtest from the middle)
        if (std::fabs(measured - r.songOffset) > 0.12) {
            log::info("AutoLayout recorder: the song was at {:.2f} s when the level time was 0 (level offset {:.2f} s): using that", measured, r.songOffset);
            r.songOffset = measured;
        }
    }
    m_songMinusLevel.clear();
    // An editor playtest is a click track: whatever was played counts.
    r.completed = completed || m_editor;
    m_stitcher.stop();
    size_t presses = r.pressTimes().size();
    if (presses == 0) {
        log::info("AutoLayout recorder: session ended without clicks, nothing saved");
        return;
    }
    m_lastPath = save(r);
    m_last = std::move(r);
    Notification::create(fmt::format("AutoLayout: recorded {} clicks ({:.1f} s)", presses, m_last->duration), NotificationIcon::Success)->show();
}

std::filesystem::path LiveRecorder::folder() {
    auto dir = Mod::get()->getSaveDir() / "recordings";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path LiveRecorder::save(rec::Recording const& r) {
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string name = r.levelName.empty() ? "level" : r.levelName;
    for (char& c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    auto base = folder() / fmt::format("{}_{}", name, stamp);
    auto bin = base;
    bin += ".alrec";
    auto json = base;
    json += ".json";
    std::string data = r.serialize();
    if (auto res = file::writeBinary(bin, ByteVector(data.begin(), data.end())); !res)
        log::error("AutoLayout recorder: cannot write {}: {}", geode::utils::string::pathToString(bin), res.unwrapErr());
    (void)file::writeString(json, r.toJson().dump(1));
    log::info("AutoLayout recorder: saved {} events to {}", r.events.size(), geode::utils::string::pathToString(bin));
    return bin;
}

geode::Result<rec::Recording> LiveRecorder::load(std::filesystem::path const& path) {
    auto data = file::readBinary(path);
    if (!data) return Err("cannot read the file: " + data.unwrapErr());
    ByteVector raw = std::move(data).unwrap();
    std::string bytes(raw.begin(), raw.end());
    std::string error;
    auto r = rec::importAny(bytes, &error);
    if (!r) return Err(error);
    m_last = *r;
    m_lastPath = path;
    return Ok(*r);
}

} // namespace al::mod
