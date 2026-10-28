#pragma once
// Deterministic re-implementation of Geometry Dash 2.2 (2.2081) classic-mode player physics.
// Spec and sources: docs/research/physics_spec.md (primary source: the MIT-licensed
// open-source-gd-project reconstructions). Coordinates are level-string coordinates: ground
// top at y = 0, object centres, 30 units per block. One step = 1/240 s.
#include <al/level/level.hpp>
#include <al/level/objects.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace al::phys {

using level::Kind;
using level::Mode;

constexpr int kTPS = 240;
constexpr double kStepSec = 1.0 / kTPS;

struct SpeedParams {
    float playerSpeed;     // m_playerSpeed
    double speedMult;      // m_speedMultiplier
    double yStart;         // cube jump velocity
    double gravity;        // cube/robot gravity
    double unitsPerSec() const { return double(playerSpeed) * speedMult * 60.0; }
    double unitsPerStep() const { return double(playerSpeed) * speedMult * 0.25; }
};
const SpeedParams& speedParams(int speedIndex); // 0 = 0.5x ... 4 = 4x

// Simulation-ready object.
struct SimObject {
    float x = 0, y = 0;          // centre
    float hw = 0, hh = 0;        // half extents (Saw: hw = radius)
    float cosR = 1, sinR = 0;    // for rotated (non-axis-aligned) hazards
    bool obb = false;            // rotated by a non-multiple of 90 degrees
    Kind kind = Kind::Deco;
    uint8_t sub = 0;
    bool facingDown = false;     // pads/orbs/portals rotated 180 / flipped Y
    bool multiActivate = false;
    int8_t slopeOrient = 0;      // 0 floor-uphill, 1 floor-downhill, 2/3 ceiling variants
    float dashAngle = 0;         // dash orbs: degrees
    float teleportOffset = 0;    // teleport portal key 54
    int levelIndex = -1;         // index into Level::objects
    float minX() const { return x - hw; }
    float maxX() const { return x + hw; }
    float minY() const { return y - hh; }
    float maxY() const { return y + hh; }
};

struct SimConfig {
    bool pressJumpCheck = true;   // pushButton runs a zero-dt jump check (fresh-press jumps get a gravity step)
    bool orbBeforeJumpOnPress = true;
    bool waveExitSimStyle = false; // false = OSGD toggle scaling (default)
    float levelEndPadding = 340.0f;
};

class World {
public:
    World() = default;
    explicit World(const level::Level& lvl, const SimConfig& cfg = {});

    const std::vector<SimObject>& objects() const { return m_objects; }
    const SimConfig& config() const { return m_cfg; }
    const level::LevelSettings& settings() const { return m_settings; }
    float endX() const { return m_endX; }
    void setEndX(float x) { m_endX = x; }

    // Candidate objects whose x-extent overlaps [x0, x1], sorted by index (deduplicated).
    void query(float x0, float x1, std::vector<int>& out) const;

    // Incremental editing (used by the generator while it builds and backtracks).
    // Returns the simulation index, or -1 if the object has no gameplay effect.
    int addObject(const level::LevelObject& o, int levelIndex);
    size_t size() const { return m_objects.size(); }
    void truncate(size_t simCount);   // remove the most recently added objects
    void setSettings(const level::LevelSettings& s) { m_settings = s; }

    // Start position (from a start-pos object if present, else (0, 15)).
    float startX() const { return m_startX; }
    float startY() const { return m_startY; }

    // Convert a level object into a simulation object (false if it has no gameplay effect).
    static bool makeSimObject(const level::LevelObject& o, int levelIndex, SimObject& out);

private:
    void build();
    void insertColumns(int index);
    std::vector<SimObject> m_objects;
    std::vector<std::vector<int>> m_columns;
    float m_colW = 32.0f;
    float m_endX = 0;
    float m_startX = 0, m_startY = 15;
    SimConfig m_cfg;
    level::LevelSettings m_settings;
};

enum class DeathCause : uint8_t { None, Hazard, Wall, HeadHit, Ceiling, Floor, KillHeight, Slope, Saw };
const char* deathCauseName(DeathCause c);

enum StepEventBits : uint32_t {
    EvJump = 1u << 0,        // ground jump (cube/robot/ball switch/spider teleport)
    EvLand = 1u << 1,
    EvOrb = 1u << 2,
    EvPad = 1u << 3,
    EvPortal = 1u << 4,
    EvGravity = 1u << 5,
    EvDeath = 1u << 6,
    EvHeadBump = 1u << 7,
    EvUfoJump = 1u << 8,
    EvSwingFlip = 1u << 9,
    EvTeleport = 1u << 10,
    EvSlope = 1u << 11,
};

struct PlayerState {
    float x = 0, y = 15;
    float lastX = 0, lastY = 15;
    double v = 0;
    Mode mode = Mode::Cube;
    uint8_t speed = 1;
    bool mini = false;
    bool upsideDown = false;
    bool onGround = true, canJump = true, isJumping = false;
    bool holding = false, jumpUnused = false;
    bool holdingDelayed = false, canRingJump = false, touchedRing = false;
    bool uncapped = false;
    bool dead = false;
    bool dashing = false;
    bool robotInvalid = false;
    bool onSlope = false, wasOnSlope = false;
    bool bounded = false;
    float robotCharge = 1.5f;
    float floorY = 0, ceilY = 1e9f;
    float dashY = 0;
    double lastFlipTime = -1, lastSpiderTime = -1, lastModeTime = -1, slopeStartTime = -1;
    int slopeObj = -1;
    int lastSnapObj = -1;
    float snapDistance = 0;
    // once-per-object activation (ring buffer of recent object indices)
    std::array<int32_t, 24> activated{};
    uint8_t activatedCount = 0, activatedHead = 0;
    std::array<int32_t, 4> touchingRings{};  // orbs overlapped in the previous step
    uint8_t touchingCount = 0;
    // events of the last step
    uint32_t events = 0;
    int32_t eventObj = -1;
    DeathCause deathCause = DeathCause::None;
    int32_t deathObj = -1;

    float width() const;  // outer hitbox (accounts for mode + mini)
    float height() const { return width(); }
    int flip() const { return upsideDown ? -1 : 1; }
    bool isFlying() const { return mode == Mode::Ship || mode == Mode::Ufo || mode == Mode::Wave || mode == Mode::Swing; }
    bool wasActivated(int idx) const;
    void markActivated(int idx);
};

struct GameState {
    std::array<PlayerState, 2> p{};
    bool dual = false;
    bool won = false;
    bool dead = false;
    bool pressed = false;   // input state applied in the last step
    int tick = 0;
    double time = 0;
};

// Initial state from the level settings (start mode/speed/mini/gravity/dual).
GameState initialState(const World& world);

// Advance one 240 Hz step with the given button state (true = held).
void step(const World& world, GameState& gs, bool pressed);

// Recompute which orbs each player overlaps (normally carried over from the previous step); used
// when a state is moved into a world with different objects.
void refreshTouching(const World& world, GameState& gs);

// Convenience: horizontal speed in units/second for a speed index.
inline double unitsPerSecond(int speedIndex) { return speedParams(speedIndex).unitsPerSec(); }

} // namespace al::phys
