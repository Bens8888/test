#pragma once
// Click and state recordings: what the player pressed, when (physics step at 240/s), and the
// state of both players at that moment. Compact versioned binary format (.alrec) plus a JSON
// export, imports of GDR replays and plain frame lists, and the practice-mode stitcher that
// turns a practice run (checkpoints, deaths, respawns) into one clean successful path.
#include <al/bot/bot.hpp>
#include <al/util/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace al::rec {

enum class Button : uint8_t { Jump = 1, Left = 2, Right = 3 };

struct PlayerSnap {
    float x = 0, y = 0;
    float yVel = 0;           // GD units per step (m_yVelocity)
    uint8_t mode = 0;         // level::Mode
    uint8_t speed = 1;        // 0 = 0.5x ... 4 = 4x
    bool mini = false;
    bool upsideDown = false;
    bool onGround = false;
    bool operator==(const PlayerSnap&) const = default;
};

struct RecEvent {
    int32_t tick = 0;         // physics step since the attempt start (240 per second)
    double time = 0;          // level time in seconds (GD's m_levelTime)
    Button button = Button::Jump;
    bool press = true;
    bool player2 = false;     // second player's button (2-player levels)
    bool dual = false;        // dual mode active
    PlayerSnap p1, p2;
    int32_t orbId = 0;        // object id of the orb/pad this press activated (0 = none)
    bool operator==(const RecEvent&) const = default;
};

struct Recording {
    static constexpr uint16_t kVersion = 1;
    std::string levelName;
    int32_t levelId = 0;
    uint64_t levelHash = 0;   // hash of the level string the recording was made on (0 = unknown)
    double tps = 240.0;       // physics steps per second the ticks refer to
    double songOffset = 0;    // seconds into the song at tick 0
    std::string source = "recorded"; // recorded | gdr | frames | json
    bool completed = false;   // the run reached the end of the level
    double duration = 0;      // seconds of gameplay covered
    std::vector<RecEvent> events;

    // Binary .alrec: magic "ALRC", version, header, events (delta-coded ticks), CRC-32.
    std::string serialize() const;
    static std::optional<Recording> deserialize(const std::string& bytes, std::string* error = nullptr);
    Json toJson() const;
    static std::optional<Recording> fromJson(const Json& j, std::string* error = nullptr);

    // Jump-button track of one player at 240 steps/s (for the simulator and bot).
    bot::InputTrack toInputTrack(bool player2 = false) const;
    // Press times (seconds of song time) of the jump button: the click rhythm.
    std::vector<double> pressTimes(bool player2 = false) const;
};

// Imports. GDR replays: JSON or MessagePack ("inputs": [{frame, btn, 2p, down}], "framerate").
std::optional<Recording> importGdr(const std::string& bytes, std::string* error = nullptr);
// Plain frame lists, one input per line: "frame down", "frame down p2" or "frame btn down p2"
// (comma or space separated, '#' comments, optional "fps N" / "tps N" header line).
std::optional<Recording> importFrameList(const std::string& text, double defaultFps = 240.0, std::string* error = nullptr);
// Any supported file by content (.alrec, JSON recording, GDR JSON/MessagePack, frame list).
std::optional<Recording> importAny(const std::string& bytes, std::string* error = nullptr);

// Practice-mode stitching. Fed with the live events of a play session; keeps exactly the inputs
// of the path that finally succeeded: everything after the checkpoint the player respawns at is
// discarded, a full restart discards everything.
class Stitcher {
public:
    void begin(const std::string& levelName, int32_t levelId, uint64_t levelHash, double songOffset);
    void onButton(const RecEvent& e);
    // An orb or pad was used at `tick`: attached to the latest press within a few steps.
    void onOrb(int32_t tick, int32_t objectId);
    void onCheckpoint(int32_t tick);          // a checkpoint was placed at this step
    void onCheckpointRemoved();               // the latest checkpoint was deleted
    // Respawn at the latest checkpoint; `holding*` is the actual button state after respawning.
    void onRespawn(int32_t tick, bool holdingP1, bool holdingP2);
    void onRestart();                         // full restart from the beginning
    void onDeath(int32_t tick);
    void onComplete(int32_t tick, double time);
    bool active() const { return m_active; }
    int attempts() const { return m_attempts; }
    size_t size() const { return m_events.size(); }
    const std::vector<int32_t>& checkpoints() const { return m_checkpoints; }
    // The stitched recording (complete or not).
    Recording result() const;
    void stop() { m_active = false; }

private:
    void truncateAfter(int32_t tick);
    bool heldAfter(bool p2) const;
    Recording m_meta;
    std::vector<RecEvent> m_events;
    std::vector<int32_t> m_checkpoints;
    bool m_active = false;
    bool m_completed = false;
    int32_t m_endTick = 0;
    double m_endTime = 0;
    int m_attempts = 0;
};

} // namespace al::rec
