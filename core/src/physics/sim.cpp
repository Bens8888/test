// GD 2.2081 classic physics. Section references (§) are to docs/research/physics_spec.md.
#include <al/physics/sim.hpp>

#include <algorithm>
#include <cmath>

namespace al::phys {

using level::GravityPortalType;
using level::OrbType;
using level::PadType;

const char* deathCauseName(DeathCause c) {
    switch (c) {
        case DeathCause::None: return "none";
        case DeathCause::Hazard: return "hazard";
        case DeathCause::Wall: return "wall";
        case DeathCause::HeadHit: return "head";
        case DeathCause::Ceiling: return "ceiling";
        case DeathCause::Floor: return "floor";
        case DeathCause::KillHeight: return "kill-height";
        case DeathCause::Slope: return "slope";
        case DeathCause::Saw: return "saw";
    }
    return "?";
}

// §2 — updateTimeMod
static const SpeedParams kSpeeds[5] = {
    {0.7f, 5.980002, 10.620032, 0.940199},
    {0.9f, double(5.770002f), double(11.180032f), double(0.958199f)},
    {1.1f, 5.870002, 11.420032, 0.957199},
    {1.3f, 6.000002, 11.230032, 0.961199},
    {1.6f, 6.000002, 11.230032, 0.961199},
};

const SpeedParams& speedParams(int s) { return kSpeeds[std::clamp(s, 0, 4)]; }

static constexpr float kG = 0.958199f;      // flying/ball/spider gravity
static constexpr float kDtJump = 0.25f * 0.9f; // 0.225f
static constexpr float kKillY = 1476.3f;

// ---------------------------------------------------------------- PlayerState helpers

float PlayerState::width() const {
    float base = 30.0f;
    if (mode == Mode::Wave) base = 10.0f;
    else if (mode == Mode::Spider) base = 27.0f;
    return mini ? base * 0.6f : base;
}

bool PlayerState::wasActivated(int idx) const {
    for (int i = 0; i < activatedCount; ++i) if (activated[size_t(i)] == idx) return true;
    return false;
}

void PlayerState::markActivated(int idx) {
    activated[activatedHead] = idx;
    activatedHead = uint8_t((activatedHead + 1) % activated.size());
    if (activatedCount < activated.size()) ++activatedCount;
}

// ---------------------------------------------------------------- World

World::World(const level::Level& lvl, const SimConfig& cfg) : m_cfg(cfg), m_settings(lvl.settings) {
    m_objects.reserve(lvl.objects.size());
    float lastX = 0;
    bool haveEnd = false;
    float endTriggerX = 0;
    for (size_t i = 0; i < lvl.objects.size(); ++i) {
        const auto& o = lvl.objects[i];
        const auto& info = o.info();
        if (o.props.count(121) && o.prop(121) == "1") continue;  // no touch
        if (o.props.count(134) && o.prop(134) == "1" && (info.kind == Kind::Solid)) continue; // passable
        if (info.kind == Kind::EndTrigger) { haveEnd = true; endTriggerX = std::max(endTriggerX, float(o.x)); continue; }
        if (info.kind == Kind::StartPos) {
            // Use the first enabled start pos only if requested by callers; by default the
            // level starts at the level start (GD behaviour in normal mode).
            continue;
        }
        if (info.kind == Kind::Deco || info.kind == Kind::Trigger || info.kind == Kind::Coin) {
            if (info.kind == Kind::Coin) lastX = std::max(lastX, float(o.x));
            continue;
        }
        SimObject s;
        s.x = float(o.x);
        s.y = float(o.y);
        s.kind = info.kind;
        s.sub = info.sub;
        s.levelIndex = int(i);
        double sx = o.scale * o.scaleX, sy = o.scale * o.scaleY;
        double w = info.w * std::fabs(sx), h = info.h * std::fabs(sy);
        double rot = std::fmod(o.rotation, 360.0);
        if (rot < 0) rot += 360;
        int quarter = int(std::lround(rot / 90.0)) % 4;
        bool axis = std::fabs(rot - quarter * 90.0) < 0.01 || std::fabs(rot - 360.0) < 0.01;
        if (info.kind == Kind::Saw) {
            s.hw = s.hh = float(info.w * std::max(std::fabs(sx), std::fabs(sy)));
        } else if (axis) {
            if (quarter % 2 == 1) std::swap(w, h);
            s.hw = float(w / 2);
            s.hh = float(h / 2);
        } else {
            s.obb = true;
            s.hw = float(w / 2);
            s.hh = float(h / 2);
            double r = -rot * 3.14159265358979323846 / 180.0; // GD rotation is clockwise
            s.cosR = float(std::cos(r));
            s.sinR = float(std::sin(r));
        }
        s.facingDown = (quarter == 2) != o.flipY;
        if (o.props.count(99)) s.multiActivate = o.prop(99) == "1";
        if (info.kind == Kind::Slope || info.kind == Kind::HazardSlope) {
            int ori = quarter;
            if (o.flipX && o.flipY) ori += 2;
            else if (o.flipX) ori += 1;
            else if (o.flipY) ori += 3;
            s.slopeOrient = int8_t(ori % 4);
            // Slopes keep their unswapped size for the surface (60x30 rotated 90 becomes 30x60).
        }
        if (info.kind == Kind::Orb && (info.sub == uint8_t(OrbType::DashGreen) || info.sub == uint8_t(OrbType::DashPink))) {
            double a = -o.rotation;
            if (o.flipX) a += 180;
            s.dashAngle = float(a);
        }
        if (info.kind == Kind::TeleportPortal) s.teleportOffset = float(o.propNum(54, 0));
        if (info.kind != Kind::Trigger) lastX = std::max(lastX, s.x);
        m_objects.push_back(s);
    }
    m_endX = haveEnd ? endTriggerX : lastX + m_cfg.levelEndPadding;
    if (m_objects.empty() && !haveEnd) m_endX = 600;
    build();
}

void World::build() {
    float maxX = 0;
    for (auto& o : m_objects) maxX = std::max(maxX, o.maxX());
    size_t cols = size_t(std::max(1.0f, (maxX + 64) / m_colW)) + 2;
    m_columns.assign(cols, {});
    for (size_t i = 0; i < m_objects.size(); ++i) {
        auto& o = m_objects[i];
        float ext = o.obb ? std::sqrt(o.hw * o.hw + o.hh * o.hh) : o.hw;
        long c0 = long(std::floor((o.x - ext) / m_colW)), c1 = long(std::floor((o.x + ext) / m_colW));
        c0 = std::max(0L, c0);
        c1 = std::min(long(cols) - 1, c1);
        for (long c = c0; c <= c1; ++c) m_columns[size_t(c)].push_back(int(i));
    }
}

void World::query(float x0, float x1, std::vector<int>& out) const {
    out.clear();
    if (m_columns.empty()) return;
    long c0 = std::max(0L, long(std::floor(x0 / m_colW)));
    long c1 = std::min(long(m_columns.size()) - 1, long(std::floor(x1 / m_colW)));
    for (long c = c0; c <= c1; ++c) for (int i : m_columns[size_t(c)]) out.push_back(i);
    if (c1 > c0) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
}

// ---------------------------------------------------------------- velocity helpers (§1.5)

static inline double roundVel(double v) {
    double ip = double(static_cast<long long>(v));
    if (ip != v) v = ip + std::round((v - ip) * 1000.0) / 1000.0;
    return v;
}
static inline void setV(PlayerState& p, double v) { p.v = roundVel(v); }
static inline void addV(PlayerState& p, double d) { p.v = roundVel(p.v + d); }

static inline double cubeGravity(const PlayerState& p) { return speedParams(p.speed).gravity; }

static bool playerIsFalling(const PlayerState& p, double t) { return p.upsideDown ? (p.v > -t) : (p.v < t); }

static bool fallingBugged(const World& W, const GameState& gs, const PlayerState& p) {
    double g = cubeGravity(p);
    if (p.mode == Mode::Swing || W.settings().fixGravityBug) return playerIsFalling(p, 2 * g);
    double thr = 2 * (gs.dual ? -g : g);
    return p.upsideDown ? p.v > thr : p.v < thr;
}

static void flipGravity(PlayerState& p, bool ud, double now) {
    if (p.upsideDown == ud) return;
    p.upsideDown = ud;
    p.lastFlipTime = now;
    p.v *= 0.5;
    p.canJump = false;
    p.events |= EvGravity;
}

static void hitGround(PlayerState& p) {
    p.v = 0;
    p.onGround = true;
    p.canJump = true;
    p.isJumping = false;
    p.events |= EvLand;
}

// ---------------------------------------------------------------- spider teleport (§3.9, estimated model)

static void spiderTeleport(const World& W, GameState& gs, PlayerState& p) {
    flipGravity(p, !p.upsideDown, gs.time);
    const float h = p.height();
    const float halfW = p.width() * 0.5f;
    std::vector<int> cand;
    W.query(p.x - halfW, p.x + halfW, cand);
    const auto& objs = W.objects();
    float target;
    if (!p.upsideDown) {
        // falling down to the nearest top surface below the feet
        float feet = p.y - h * 0.5f;
        target = p.floorY;
        for (int i : cand) {
            const auto& o = objs[size_t(i)];
            if (o.kind != Kind::Solid && o.kind != Kind::Breakable) continue;
            if (o.maxX() < p.x - halfW + 1 || o.minX() > p.x + halfW - 1) continue;
            if (o.maxY() <= feet + 1.0f && o.maxY() > target) target = o.maxY();
        }
        p.y = target + h * 0.5f;
    } else {
        float head = p.y + h * 0.5f;
        target = p.bounded ? p.ceilY : kKillY + 100;
        for (int i : cand) {
            const auto& o = objs[size_t(i)];
            if (o.kind != Kind::Solid && o.kind != Kind::Breakable) continue;
            if (o.maxX() < p.x - halfW + 1 || o.minX() > p.x + halfW - 1) continue;
            if (o.minY() >= head - 1.0f && o.minY() < target) target = o.minY();
        }
        p.y = target - h * 0.5f;
    }
    p.lastY = p.y;
    p.v = 0;
    p.onGround = true;
    p.canJump = false;
    p.isJumping = false;
    p.jumpUnused = false;
    p.holding = false; // a new press is needed for the next teleport
    p.lastSpiderTime = gs.time;
    p.events |= EvTeleport | EvJump;
}

// ---------------------------------------------------------------- updateJump (§3)

static void updateJump(const World& W, GameState& gs, PlayerState& p, float dt) {
    const int flip = p.flip();
    const float S = p.mini ? 0.8f : 1.0f;
    if (p.isFlying()) {
        const double scaleMod = p.mini ? 0.85 : 1.0;
        const double maxV = 8.0 / scaleMod, minV = -6.4 / scaleMod;
        if (!p.upsideDown) {
            if ((p.v >= 0 && p.v < maxV) || (p.v <= 0 && p.v > minV)) p.uncapped = false;
        } else {
            if ((p.v <= 0 && p.v > -maxV) || (p.v >= 0 && p.v < -minV)) p.uncapped = false;
        }
        switch (p.mode) {
            case Mode::Ship: {
                double shipMod;
                bool fb = fallingBugged(W, gs, p);
                if (p.holding && !p.uncapped) {
                    shipMod = -1;
                } else {
                    shipMod = p.uncapped ? ((flip > 0 ? p.v < 0 : p.v > 0) ? -1 : 0.8) : 0.8;
                    if (!p.holding && !fb) shipMod = 1.2;
                }
                double boostMod = (p.holding && fb) ? 0.5 : 0.4;
                addV(p, -(double(kG) * dt * flip * shipMod * boostMod) / scaleMod);
                if (p.holding) p.onGround = false;
                break;
            }
            case Mode::Ufo: {
                if (p.jumpUnused && p.holding) {
                    p.jumpUnused = false;
                    double jv = (p.mini ? 8.0 : 7.0) * flip * scaleMod;
                    if (flip > 0 ? jv > p.v : jv < p.v) {
                        setV(p, jv);
                        p.isJumping = true;
                        p.onGround = false;
                        p.canJump = false;
                        p.events |= EvUfoJump;
                    }
                }
                double birdMod = fallingBugged(W, gs, p) ? 0.8 : 1.2;
                addV(p, -(double(kG) * dt * flip * birdMod * 0.5) / scaleMod);
                break;
            }
            case Mode::Wave: {
                const auto& sp = speedParams(p.speed);
                setV(p, double(sp.playerSpeed) * sp.speedMult * flip * (p.holding ? 1.0 : -1.0));
                break;
            }
            case Mode::Swing: {
                if (p.jumpUnused && p.holding) {
                    p.jumpUnused = false;
                    double pre = p.v;
                    flipGravity(p, !p.upsideDown, gs.time);
                    setV(p, pre * 0.8);
                    p.events |= EvSwingFlip;
                }
                const int f2 = p.flip();
                addV(p, -((p.mini ? 0.6 : 0.4) * double(kG) * dt * f2));
                break;
            }
            default: break;
        }
        if (!p.uncapped && p.mode != Mode::Wave) {
            const double modeScale = p.mode == Mode::Swing ? 1.0 : 0.8;
            const double sm = p.mode == Mode::Swing ? 1.0 : scaleMod;
            if (!p.upsideDown) p.v = std::clamp(p.v, -8.0 * modeScale / sm, 8.0 / sm);
            else p.v = std::clamp(p.v, -8.0 / sm, 8.0 * modeScale / sm);
        }
        if (fallingBugged(W, gs, p)) p.isJumping = false;
        return;
    }

    // ground modes: cube, robot, ball, spider (§3.6)
    const bool ball = p.mode == Mode::Ball, spider = p.mode == Mode::Spider, robot = p.mode == Mode::Robot;
    const float gs_ = (ball || spider) ? 0.6f : (robot ? 0.9f : 1.0f);
    const float grav = (ball || spider) ? kG : float(cubeGravity(p));
    const bool isHoldingJump = robot ? (p.holding && p.jumpUnused) : p.holding;
    if (isHoldingJump && p.canJump && !p.dashing) {
        if (spider) {
            spiderTeleport(W, gs, p);
            return;
        }
        p.isJumping = true;
        p.onGround = false;
        p.canJump = false;
        p.jumpUnused = false;
        p.robotInvalid = false;
        p.robotCharge = 0;
        double jv = double(float(speedParams(p.speed).yStart)) * (robot ? 0.5 : 1.0);
        setV(p, jv * flip * S);
        if ((p.onSlope || p.wasOnSlope) && p.slopeObj >= 0) {
            // slope jump bonus (§10): +0.25 x modified slope velocity, capped at 1.4x
            const auto& so = W.objects()[size_t(p.slopeObj)];
            bool uphill = (so.slopeOrient == 0 && !p.upsideDown) || (so.slopeOrient == 3 && p.upsideDown);
            if (uphill) {
                const auto& sp = speedParams(p.speed);
                double w = so.hw * 2, hgt = so.hh * 2;
                double ang = std::atan2(hgt, w);
                double slopeYVel = hgt * double(sp.playerSpeed) * sp.speedMult / w;
                double sv = std::min(1.12 / ang, 1.54) * slopeYVel;
                double t = gs.time - p.slopeStartTime;
                double mod = t < 0.1 ? std::max(0.4, 10.0 * t) : 1.0;
                double cap = 1.4 * std::fabs(p.v);
                double nv = std::fabs(p.v) + 0.25 * sv * mod;
                setV(p, std::min(nv, cap) * flip);
            }
        }
        p.events |= EvJump;
        if (ball) {
            flipGravity(p, !p.upsideDown, gs.time);
            p.v *= double(0.6f);
            p.holding = false;
        }
        return;
    }
    if (p.isJumping) {
        if (robot && p.holding && !p.robotInvalid && p.robotCharge < 1.5f) {
            p.robotCharge += dt * 0.1f;
            addV(p, double(grav) * dt * flip * gs_);
        }
        addV(p, -(double(gs_) * grav * dt * flip));
        if (fallingBugged(W, gs, p)) { p.isJumping = false; p.onGround = false; }
    } else {
        if (fallingBugged(W, gs, p)) p.canJump = false;
        addV(p, -(double(gs_) * grav * dt * flip));
        if (!p.upsideDown) p.v = std::max(p.v, -15.0);
        else p.v = std::min(p.v, 15.0);
        if (fallingBugged(W, gs, p) && (p.upsideDown ? p.v > 4 : p.v < -4)) p.onGround = false;
    }
}

// ---------------------------------------------------------------- mode switching (§7.1, §7.2)

static bool modeIsFlying(Mode m) { return m == Mode::Ship || m == Mode::Ufo || m == Mode::Wave || m == Mode::Swing; }
static float modeBoundsHeight(Mode m) {
    switch (m) {
        case Mode::Ship: case Mode::Ufo: case Mode::Wave: case Mode::Swing: return 300;
        case Mode::Ball: case Mode::Spider: return 240;
        default: return 0;
    }
}

static void setBoundsFromPortal(PlayerState& p, float portalY, float H) {
    float floor = std::max(0.0f, 30.0f * std::ceil((portalY - (H / 2 + 30)) / 30.0f));
    p.floorY = floor;
    p.ceilY = floor + H;
    p.bounded = true;
}

static void switchMode(const World& W, GameState& gs, PlayerState& p, Mode m, float portalY, bool fromPortal) {
    const bool cfgSim = W.config().waveExitSimStyle;
    if (p.mode != m) {
        Mode old = p.mode;
        if (cfgSim && old == Mode::Wave) {
            double div = (m == Mode::Cube || m == Mode::Ship) ? 4.0 : (m == Mode::Ufo ? 2.0 : 1.0);
            p.v = p.v * 0.9 / div;
        } else {
            if (modeIsFlying(old)) p.v *= 0.5;
            if (modeIsFlying(m)) p.v *= 0.5;
        }
        p.mode = m;
        p.onGround = false;
        p.canJump = false;
        p.isJumping = false;
        p.dashing = false;
        if (m == Mode::Robot || m == Mode::Spider) p.robotCharge = 1.5f;
        p.lastModeTime = gs.time;
        p.onSlope = false;
    }
    float H = modeBoundsHeight(m);
    if (H > 0 && fromPortal) setBoundsFromPortal(p, portalY, H);
    else if (H == 0 && !gs.dual) { p.bounded = false; p.floorY = 0; p.ceilY = 1e9f; }
}

// ---------------------------------------------------------------- orbs (§5)

static void startDash(PlayerState& p, float angleDeg) {
    double a = std::fmod(double(angleDeg), 360.0);
    if (a > 180) a -= 360;
    if (a < -180) a += 360;
    if (std::fabs(a) > 90) a = (a > 0 ? 180.0 : -180.0) - a;
    a = std::clamp(a, -70.0, 70.0);
    double r = a * 3.14159265358979323846 / 180.0;
    p.dashing = true;
    p.dashY = float(std::tan(r));
    p.v = 0;
}

static void ringJump(const World& W, GameState& gs, PlayerState& p, const SimObject& o, int objIndex) {
    const OrbType t = OrbType(o.sub);
    const Mode m = p.mode;
    const float S = p.mini ? 0.8f : 1.0f;
    const double yStart = double(float(speedParams(p.speed).yStart));
    auto keepFactor = [&]() { return (m == Mode::Ball || m == Mode::Spider) ? double(0.7f) : (m == Mode::Swing ? double(0.6f) : 1.0); };
    auto factorFor = [&](OrbType ty) -> double {
        switch (ty) {
            case OrbType::Yellow:
                return m == Mode::Robot ? 0.9 : 1.0;
            case OrbType::Pink:
                if (m == Mode::Ship) return 0.37;
                if (m == Mode::Ufo) return 0.42;
                if (m == Mode::Ball) return 0.77;
                return 0.72;
            case OrbType::Red:
                if (m == Mode::Ship) return p.mini ? 1.4 : 1.0;
                if (m == Mode::Ufo) return p.mini ? 1.36 : 1.02;
                if (m == Mode::Ball || m == Mode::Spider) return 1.34;
                if (m == Mode::Robot) return 1.28;
                return 1.38;
            default: return 1.0;
        }
    };
    p.touchedRing = true;
    p.jumpUnused = false;
    p.canRingJump = false;
    p.events |= EvOrb;
    p.eventObj = objIndex;
    switch (t) {
        case OrbType::Yellow: case OrbType::Pink: case OrbType::Red: {
            setV(p, p.flip() * yStart * factorFor(t) * S);
            p.v *= keepFactor();
            if (t == OrbType::Red) p.uncapped = true;
            break;
        }
        case OrbType::Blue: {
            double f = m == Mode::Robot ? 0.8 : 0.8;
            setV(p, p.flip() * yStart * f * S);
            p.v *= keepFactor();
            flipGravity(p, !p.upsideDown, gs.time);
            break;
        }
        case OrbType::Green: {
            flipGravity(p, !p.upsideDown, gs.time);
            double f = m == Mode::Ship ? 0.7 : 1.0;
            setV(p, p.flip() * yStart * f * S);
            p.v *= keepFactor();
            break;
        }
        case OrbType::Black: {
            double v;
            if (m == Mode::Spider) v = 16.5;
            else if (m == Mode::Ship || m == Mode::Wave || m == Mode::Swing) v = 14.0;
            else if (m == Mode::Ufo) v = 11.2;
            else v = 15.0;
            setV(p, -v * p.flip());
            p.uncapped = true;
            if (m == Mode::Ball || m == Mode::Swing) p.holding = false;
            break;
        }
        case OrbType::DashGreen: startDash(p, o.dashAngle); break;
        case OrbType::DashPink: flipGravity(p, !p.upsideDown, gs.time); startDash(p, o.dashAngle); break;
        case OrbType::Spider: {
            if (o.facingDown != p.upsideDown) flipGravity(p, !p.upsideDown, gs.time);
            spiderTeleport(W, gs, p);
            break;
        }
        case OrbType::Teleport: case OrbType::Toggle: break;
    }
    if (t != OrbType::DashGreen && t != OrbType::DashPink && t != OrbType::Toggle && t != OrbType::Teleport) {
        p.isJumping = true;
        p.canJump = false;
        p.onGround = false;
        if (m == Mode::Ball || m == Mode::Spider || m == Mode::Swing) p.holding = false;
    }
    if (m == Mode::Wave && t != OrbType::Blue && t != OrbType::Green && t != OrbType::DashGreen && t != OrbType::DashPink) {
        // waves can't use non-gravity orbs (velocity is ignored by wave motion)
    }
}

// ---------------------------------------------------------------- pads (§6)

static void bumpPlayer(const World& W, GameState& gs, PlayerState& p, const SimObject& o) {
    const PadType t = PadType(o.sub);
    const Mode m = p.mode;
    const float S = p.mini ? 0.8f : 1.0f;
    if (t == PadType::Blue) {
        // only a pad facing the player's current gravity flips it
        if (o.facingDown != p.upsideDown) return;
        setV(p, 0.8 * 16.0 * p.flip() * S);
        if (m == Mode::Ball || m == Mode::Spider || m == Mode::Swing) p.v *= double(0.6f);
        flipGravity(p, !p.upsideDown, gs.time);
    } else if (t == PadType::Spider) {
        if (o.facingDown != p.upsideDown) flipGravity(p, !p.upsideDown, gs.time);
        flipGravity(p, !p.upsideDown, gs.time); // spiderTeleport flips back to the teleport direction
        spiderTeleport(W, gs, p);
        return;
    } else {
        double mod = 1.0;
        if (t == PadType::Pink) mod = m == Mode::Ship ? 0.35 : (m == Mode::Ufo ? 0.4 : (m == Mode::Ball ? 0.7 : 0.65));
        if (t == PadType::Red) {
            if (m == Mode::Ship) mod = p.mini ? 0.95 : 0.63;
            else if (m == Mode::Ufo) mod = p.mini ? 0.98 : 0.6;
            else mod = 1.25;
        }
        setV(p, mod * 16.0 * p.flip() * S);
        if (m == Mode::Ball || m == Mode::Spider || m == Mode::Swing) p.v *= double(0.6f);
        p.uncapped = t == PadType::Red;
    }
    p.isJumping = true;
    p.onGround = false;
    p.canJump = false;
    p.onSlope = p.wasOnSlope = false;
    if (m == Mode::Robot && (!W.settings().fixRobotJump)) p.robotInvalid = true;
    p.events |= EvPad;
}

// ---------------------------------------------------------------- geometry helpers

struct Rect { float x0, y0, x1, y1; };
static inline Rect rectOf(float cx, float cy, float hw, float hh) { return {cx - hw, cy - hh, cx + hw, cy + hh}; }
static inline bool overlaps(const Rect& a, const Rect& b) { return !(a.x1 < b.x0 || b.x1 < a.x0 || a.y1 < b.y0 || b.y1 < a.y0); }
static inline Rect objRect(const SimObject& o) { return rectOf(o.x, o.y, o.hw, o.hh); }

// SAT test between an axis-aligned rect and an oriented rect.
static bool overlapsOBB(const Rect& r, const SimObject& o) {
    float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
    float hx = (r.x1 - r.x0) * 0.5f, hy = (r.y1 - r.y0) * 0.5f;
    float dx = o.x - cx, dy = o.y - cy;
    float ux = o.cosR, uy = o.sinR, vx = -o.sinR, vy = o.cosR;
    // axes of the AABB
    float ex = o.hw * std::fabs(ux) + o.hh * std::fabs(vx);
    float ey = o.hw * std::fabs(uy) + o.hh * std::fabs(vy);
    if (std::fabs(dx) > hx + ex) return false;
    if (std::fabs(dy) > hy + ey) return false;
    // axes of the OBB
    float pu = std::fabs(dx * ux + dy * uy), pv = std::fabs(dx * vx + dy * vy);
    if (pu > o.hw + hx * std::fabs(ux) + hy * std::fabs(uy)) return false;
    if (pv > o.hh + hx * std::fabs(vx) + hy * std::fabs(vy)) return false;
    return true;
}

static bool hitsObject(const Rect& r, const SimObject& o) { return o.obb ? overlapsOBB(r, o) : overlaps(r, objRect(o)); }

static bool sawHits(const World& W, const PlayerState& p, const Rect& r, const SimObject& o) {
    float rad = o.hw;
    if (W.settings().fixRadius) {
        float dx = p.x - o.x, dy = p.y - o.y;
        float lim = rad + p.width() * 0.5f;
        return dx * dx + dy * dy <= lim * lim;
    }
    if (o.x >= r.x0 && o.x <= r.x1 && o.y >= r.y0 && o.y <= r.y1) return true;
    const float cx[2] = {r.x0, r.x1}, cy[2] = {r.y0, r.y1};
    for (float px : cx) for (float py : cy) {
        float dx = px - o.x, dy = py - o.y;
        if (dx * dx + dy * dy < rad * rad) return true;
    }
    return false;
}

static void die(PlayerState& p, DeathCause c, int obj) {
    if (p.dead) return;
    p.dead = true;
    p.deathCause = c;
    p.deathObj = obj;
    p.events |= EvDeath;
}

// ---------------------------------------------------------------- slopes (§10, approximate model)

// Surface y at x for a slope, extended beyond its rect.
static float slopeSurface(const SimObject& s, float x) {
    float x0 = s.minX(), y0 = s.minY(), y1 = s.maxY();
    float w = s.hw * 2, h = s.hh * 2;
    float t = (x - x0) / std::max(1e-3f, w);
    switch (s.slopeOrient) {
        case 0: return y0 + t * h;          // floor, rising
        case 1: return y1 - t * h;          // floor, falling
        case 2: return y0 + t * h;          // ceiling, solid above the rising line
        default: return y1 - t * h;         // ceiling, solid above the falling line
    }
}
static bool slopeSolidBelow(const SimObject& s) { return s.slopeOrient == 0 || s.slopeOrient == 1; }

// Returns true if the player was placed on / pushed by this slope.
static bool collideSlope(const World& W, GameState& gs, PlayerState& p, const SimObject& s, int idx) {
    const float h = p.height(), halfW = p.width() * 0.5f;
    const int g = p.flip();
    const bool walkable = (g > 0) == slopeSolidBelow(s);
    float xc = std::clamp(p.x, s.minX(), s.maxX());
    float surf = slopeSurface(s, xc);
    float ang = std::atan2(s.hh * 2, s.hw * 2);
    float off = (h * 0.5f) / std::cos(ang);
    if (s.kind == Kind::HazardSlope || p.mode == Mode::Wave) {
        // contact with the slope surface kills (wave dies on any slope)
        Rect r = rectOf(p.x, p.y, halfW, h * 0.5f);
        float lo = std::max(r.x0, s.minX()), hi = std::min(r.x1, s.maxX());
        if (lo > hi) return false;
        float sa = slopeSurface(s, lo), sb = slopeSurface(s, hi);
        float extreme = slopeSolidBelow(s) ? std::max(sa, sb) : std::min(sa, sb);
        float pad = s.kind == Kind::HazardSlope ? -4.0f : 0.0f; // hazard slopes are offset 4 units toward the free side (inverse sign = smaller)
        bool touch = slopeSolidBelow(s) ? (r.y0 < extreme + pad) : (r.y1 > extreme - pad);
        if (touch && r.y1 > s.minY() && r.y0 < s.maxY()) { die(p, DeathCause::Slope, idx); return true; }
        return false;
    }
    if (walkable) {
        // feet relative to the gravity-floor surface
        float feet = p.y - g * h * 0.5f;
        float target = surf + g * off;
        float depth = g * (surf - feet); // > 0 when feet are below the surface (normal gravity)
        float tol = (p.wasOnSlope ? 4.0f : 1.0f) + std::fabs(float(p.v) * kDtJump) + 10.0f;
        if (depth >= -0.5f && depth <= tol && g * p.v <= 5.0) {
            if (p.x < s.minX() - halfW || p.x > s.maxX() + halfW) return false;
            if (p.x < s.minX() || p.x > s.maxX()) {
                // beyond the slope's rect: only the flat end acts (like a block top)
                return false;
            }
            p.y = target;
            if (!p.isJumping || g * p.v <= 0) {
                if (!p.onSlope && !p.wasOnSlope) p.slopeStartTime = gs.time;
                p.v = 0;
                p.onGround = true;
                p.canJump = true;
                p.isJumping = false;
                p.onSlope = true;
                p.slopeObj = idx;
                p.events |= EvSlope;
            }
            return true;
        }
        // deep inside the solid part: ran into it
        Rect inner = rectOf(p.x, p.y, 4.5f, 4.5f);
        float sx = std::clamp(p.x, s.minX(), s.maxX());
        float sy = slopeSurface(s, sx);
        if (inner.x1 > s.minX() && inner.x0 < s.maxX() && (g > 0 ? inner.y0 < sy - 1 && inner.y1 > s.minY() : inner.y1 > sy + 1 && inner.y0 < s.maxY())) {
            if (gs.time - p.lastFlipTime < 0.1 || gs.time - p.lastModeTime < 0.1) return false;
            die(p, DeathCause::Slope, idx);
            return true;
        }
        return false;
    }
    // head-side slope: flying modes and ball are pushed, others die when the inner box enters it
    Rect r = rectOf(p.x, p.y, halfW, h * 0.5f);
    float lo = std::max(r.x0, s.minX()), hi = std::min(r.x1, s.maxX());
    if (lo > hi) return false;
    float sa = slopeSurface(s, lo), sb = slopeSurface(s, hi);
    float extreme = slopeSolidBelow(s) ? std::max(sa, sb) : std::min(sa, sb);
    bool inside = slopeSolidBelow(s) ? r.y0 < extreme : r.y1 > extreme;
    if (!inside) return false;
    if (p.isFlying() || p.mode == Mode::Ball) {
        p.y = slopeSolidBelow(s) ? extreme + h * 0.5f : extreme - h * 0.5f;
        if (g * p.v > 0) p.v = 0;
        p.events |= EvHeadBump;
        return true;
    }
    Rect inner = rectOf(p.x, p.y, 4.5f, 4.5f);
    bool innerIn = slopeSolidBelow(s) ? inner.y0 < extreme : inner.y1 > extreme;
    if (innerIn && gs.time - p.lastFlipTime >= 0.1) { die(p, DeathCause::Slope, idx); return true; }
    return false;
}

// ---------------------------------------------------------------- blocks (§9)

// Cube stair snapping (OSGD checkSnapJumpToObject): when landing on a different block whose
// offset from the previously snapped block matches a standard stair step, nudge x (by at most
// the tolerance) so the player keeps the same relative position as on the previous step.
static void checkSnapJump(const World& W, PlayerState& p, const SimObject& o, int idx) {
    if (p.mode != Mode::Cube) return;
    if (p.lastSnapObj >= 0 && p.lastSnapObj != idx) {
        static const float table[5][5] = {
            // step-up dx, step-up dx (mini), step-down dx, double-step-up dx, tolerance
            {90, 90, 120, 60, 1}, {120, 90, 150, 90, 1}, {150, 90, 195, 120, 2}, {180, 90, 225, 135, 2}, {120, 120, 150, 90, 1}};
        const float* t = table[std::clamp<int>(p.speed, 0, 4)];
        const float tol = t[4];
        const SimObject& prev = W.objects()[size_t(p.lastSnapObj)];
        const float dx = o.x - prev.x, dy = (o.y - prev.y) * float(p.flip());
        bool match = false;
        if (std::fabs(dy - 30) <= tol && std::fabs(dx - (p.mini ? t[1] : t[0])) <= tol) match = true;
        else if (std::fabs(dy + 30) <= tol && std::fabs(dx - t[2]) <= tol) match = true;
        else if (std::fabs(dy - 60) <= tol && std::fabs(dx - t[3]) <= tol) match = true;
        if (match) {
            float target = o.x + p.snapDistance;
            float diff = std::clamp(target - p.x, -tol, tol);
            p.x += diff;
        }
    }
    p.lastSnapObj = idx;
    p.snapDistance = p.x - o.x;
}

static void collideSolid(const World& W, GameState& gs, PlayerState& p, const SimObject& o, int idx, bool& landed) {
    const float h = p.height(), hw = p.width() * 0.5f;
    Rect r = rectOf(p.x, p.y, hw, h * 0.5f);
    Rect b = objRect(o);
    if (!overlaps(r, b)) return;
    const int g = p.flip();
    const float T = p.isFlying() ? 6.0f : 10.0f;
    const float feet = p.y - g * h * 0.5f, head = p.y + g * h * 0.5f;
    const float d = p.y - p.lastY;
    const float floorFace = g > 0 ? b.y1 : b.y0;
    const float headFace = g > 0 ? b.y0 : b.y1;
    const bool wave = p.mode == Mode::Wave;
    if (!wave) {
        float reach = feet + g * T;
        bool canLand = g * (reach - floorFace) >= 0 || g * (reach - d - floorFace) >= 0;
        if (canLand) {
            if (g * p.v > 0 && !p.onSlope) return; // moving away from the floor (pad/orb launch)
            p.y = floorFace + g * h * 0.5f;
            hitGround(p);
            landed = true;
            checkSnapJump(W, p, o, idx);
            return;
        }
        // head bump for modes that can hit their head
        bool bumpMode = p.mode == Mode::Ship || p.mode == Mode::Ufo || p.mode == Mode::Swing || p.mode == Mode::Ball;
        if (bumpMode) {
            bool near = g * (headFace - head) >= -T || g * (headFace - (head - d)) >= -T;
            if (near && g * p.v >= 0) {
                p.y = headFace - g * h * 0.5f;
                p.v = 0;
                p.events |= EvHeadBump;
                return;
            }
        }
    }
    Rect inner = rectOf(p.x, p.y, 4.5f, 4.5f); // 0.3 x 30 regardless of mini (§8.2)
    if (p.mode == Mode::Wave) inner = rectOf(p.x, p.y, p.mini ? 0.9f : 1.5f, p.mini ? 0.9f : 1.5f);
    else if (p.mode == Mode::Spider) inner = rectOf(p.x, p.y, 4.05f, 4.05f);
    if (!overlaps(inner, b)) return;
    if (o.kind == Kind::Breakable) return; // destroyed, no death (state not tracked: treat as passable)
    // gravity-flip grace: a head-side block within 0.1 s of a flip pushes instead of killing
    if (gs.time - p.lastFlipTime < 0.1) {
        // the block is on the head side (the side gravity now points away from)
        bool headSide = g > 0 ? (b.y0 >= p.y - 1) : (b.y1 <= p.y + 1);
        if (headSide) {
            p.y = g > 0 ? b.y0 - h * 0.5f : b.y1 + h * 0.5f;
            hitGround(p);
            return;
        }
    }
    if (p.mode == Mode::Spider && gs.time - p.lastSpiderTime < 0.04 && b.x1 < inner.x1) return;
    // ran into the side of the block vs. hit it with the head
    bool side = p.lastX + 4.5f <= b.x0 + 0.5f;
    die(p, side ? DeathCause::Wall : DeathCause::HeadHit, idx);
}

// ---------------------------------------------------------------- per-player step

static thread_local std::vector<int> tl_cand;
static thread_local std::vector<int> tl_solids;
static thread_local std::vector<int> tl_hazards;

static void collidePlayer(const World& W, GameState& gs, PlayerState& p) {
    const auto& objs = W.objects();
    const float h = p.height(), hw = p.width() * 0.5f;
    W.query(p.x - 40, p.x + 40, tl_cand);
    tl_solids.clear();
    tl_hazards.clear();
    std::array<int32_t, 4> touchingNow{};
    uint8_t touchingNowCount = 0;
    Rect r = rectOf(p.x, p.y, hw, h * 0.5f);

    // pass 1: interactive objects in level order
    for (int i : tl_cand) {
        const SimObject& o = objs[size_t(i)];
        switch (o.kind) {
            case Kind::Solid: case Kind::Breakable: case Kind::Slope:
                tl_solids.push_back(i);
                continue;
            case Kind::Hazard: case Kind::Saw: case Kind::HazardSlope:
                tl_hazards.push_back(i);
                continue;
            default: break;
        }
        if (!hitsObject(r, o)) continue;
        switch (o.kind) {
            case Kind::Orb: {
                if (touchingNowCount < touchingNow.size()) touchingNow[touchingNowCount++] = i;
                if (p.wasActivated(i) && !o.multiActivate) break;
                if (!p.canRingJump || !p.holdingDelayed || p.touchedRing || p.dashing) break;
                ringJump(W, gs, p, o, i);
                if (!o.multiActivate) p.markActivated(i);
                break;
            }
            case Kind::Pad: {
                if (p.wasActivated(i) && !o.multiActivate) break;
                p.markActivated(i);
                bumpPlayer(W, gs, p, o);
                p.eventObj = i;
                break;
            }
            case Kind::ModePortal: {
                if (p.wasActivated(i)) break;
                p.markActivated(i);
                switchMode(W, gs, p, Mode(o.sub), o.y, true);
                p.events |= EvPortal;
                p.eventObj = i;
                break;
            }
            case Kind::GravityPortal: {
                if (p.wasActivated(i)) break;
                p.markActivated(i);
                auto t = GravityPortalType(o.sub);
                bool target = t == GravityPortalType::Flip ? true : (t == GravityPortalType::Normal ? false : !p.upsideDown);
                flipGravity(p, target, gs.time);
                p.events |= EvPortal;
                p.eventObj = i;
                break;
            }
            case Kind::SizePortal: {
                if (p.wasActivated(i)) break;
                p.markActivated(i);
                p.mini = o.sub == 1;
                p.events |= EvPortal;
                p.eventObj = i;
                break;
            }
            case Kind::SpeedPortal: {
                if (p.wasActivated(i)) break;
                p.markActivated(i);
                p.speed = o.sub;
                p.events |= EvPortal;
                p.eventObj = i;
                break;
            }
            case Kind::DualPortal: {
                if (p.wasActivated(i)) break;
                p.markActivated(i);
                p.events |= EvPortal;
                p.eventObj = i;
                if (o.sub == 1 && !gs.dual) {
                    gs.dual = true;
                    PlayerState q = p;
                    q.upsideDown = !p.upsideDown;
                    q.v = 0;
                    q.onGround = false;
                    q.canJump = false;
                    // dual bounds: use the mode's bounds, or 300 for unbounded modes
                    float H = modeBoundsHeight(p.mode);
                    if (H == 0) H = 300;
                    setBoundsFromPortal(p, o.y, H);
                    q.floorY = p.floorY;
                    q.ceilY = p.ceilY;
                    q.bounded = true;
                    gs.p[1] = q;
                    gs.p[1].markActivated(i);
                } else if (o.sub == 0 && gs.dual) {
                    gs.dual = false;
                    if (modeBoundsHeight(gs.p[0].mode) == 0) { gs.p[0].bounded = false; gs.p[0].floorY = 0; gs.p[0].ceilY = 1e9f; }
                }
                break;
            }
            case Kind::TeleportPortal: {
                if (p.wasActivated(i)) break;
                p.markActivated(i);
                p.y += o.teleportOffset;
                p.lastY = p.y;
                p.onGround = false;
                p.events |= EvPortal;
                break;
            }
            default: break;
        }
        if (p.dead) return;
    }
    p.touchingRings = touchingNow;
    p.touchingCount = touchingNowCount;

    // pass 2: slopes then solids (solids in reverse order, as gd-sim found the game does)
    bool landed = false;
    for (int i : tl_solids) {
        const SimObject& o = objs[size_t(i)];
        if (o.kind != Kind::Slope) continue;
        Rect rr = rectOf(p.x, p.y, hw, h * 0.5f);
        Rect big = rectOf(o.x, o.y, o.hw * 1.2f + 2, o.hh * 1.1f + 12);
        if (!overlaps(rr, big)) continue;
        collideSlope(W, gs, p, o, i);
        if (p.dead) return;
    }
    for (size_t k = tl_solids.size(); k-- > 0;) {
        int i = tl_solids[k];
        const SimObject& o = objs[size_t(i)];
        if (o.kind == Kind::Slope) continue;
        if (p.onSlope) {
            // blocks at a slope junction within 2 units of the surface are ignored
            const SimObject& so = objs[size_t(p.slopeObj)];
            float top = p.flip() > 0 ? o.maxY() : o.minY();
            if (std::fabs(top - slopeSurface(so, std::clamp(p.x, so.minX(), so.maxX()))) < 2.5f) continue;
        }
        collideSolid(W, gs, p, o, i, landed);
        if (p.dead) return;
    }

    // pass 3: hazards
    r = rectOf(p.x, p.y, hw, h * 0.5f);
    for (int i : tl_hazards) {
        const SimObject& o = objs[size_t(i)];
        if (o.kind == Kind::Saw) {
            if (sawHits(W, p, r, o)) { die(p, DeathCause::Saw, i); return; }
        } else if (o.kind == Kind::HazardSlope) {
            collideSlope(W, gs, p, o, i);
            if (p.dead) return;
        } else if (hitsObject(r, o)) {
            die(p, DeathCause::Hazard, i);
            return;
        }
    }

    // level floor / ceiling (§9.6)
    const int g = p.flip();
    const bool wave = p.mode == Mode::Wave;
    const float half = wave ? h : h * 0.5f;
    const float floorY = p.bounded ? p.floorY : 0.0f;
    const bool hasCeil = p.bounded;
    const float ceilY = p.ceilY;
    const bool groundMode = p.mode == Mode::Cube || p.mode == Mode::Robot;
    if (g > 0) {
        if (p.y - half <= floorY) {
            if (p.v <= 0 || wave) {
                p.y = floorY + half;
                if (!wave) hitGround(p);
                else p.v = 0;
            }
        }
        if (hasCeil && p.y + half >= ceilY) {
            if (groundMode) {
                if (p.y + 4.5f >= ceilY) { die(p, DeathCause::Ceiling, -1); return; }
            } else {
                p.y = ceilY - half;
                if (p.v > 0) p.v = 0;
                p.events |= EvHeadBump;
            }
        }
    } else {
        if (hasCeil && p.y + half >= ceilY) {
            if (p.v >= 0 || wave) {
                p.y = ceilY - half;
                if (!wave) hitGround(p);
                else p.v = 0;
            }
        }
        if (p.y - half <= floorY) {
            if (groundMode) {
                if (p.y - 4.5f <= floorY) { die(p, DeathCause::Floor, -1); return; }
            } else {
                p.y = floorY + half;
                if (p.v < 0) p.v = 0;
                p.events |= EvHeadBump;
            }
        }
    }
    if (p.y > kKillY) { die(p, DeathCause::KillHeight, -1); return; }

    // leaving a slope: launch / fall velocities
    if (p.wasOnSlope && !p.onSlope && p.slopeObj >= 0 && !(p.events & (EvJump | EvOrb | EvPad))) {
        const SimObject& so = objs[size_t(p.slopeObj)];
        const auto& sp = speedParams(p.speed);
        double w = so.hw * 2, hh = so.hh * 2;
        double ang = std::atan2(hh, w);
        double slopeYVel = hh * double(sp.playerSpeed) * sp.speedMult / w;
        bool uphill = (so.slopeOrient == 0 && g > 0) || (so.slopeOrient == 3 && g < 0);
        if (uphill && p.x > so.maxX()) {
            double t = gs.time - p.slopeStartTime;
            double mod = t < 0.1 ? std::max(0.4, 10.0 * t) : 1.0;
            double sv = std::min(1.12 / ang, 1.54) * slopeYVel * mod;
            if (p.isFlying() || p.mode == Mode::Ball) sv *= 0.75;
            setV(p, sv * g);
            p.isJumping = true;
            p.onGround = false;
            p.canJump = false;
        } else if (!uphill && p.x > so.maxX()) {
            setV(p, -slopeYVel * g);
        }
    }
    (void)landed;
}

static void pushButton(const World& W, GameState& gs, PlayerState& p) {
    p.holding = true;
    p.jumpUnused = true;
    auto tryRings = [&]() {
        // A fresh press can hit an orb overlapped during the previous step.
        for (int k = 0; k < p.touchingCount; ++k) {
            int i = p.touchingRings[size_t(k)];
            const SimObject& o = W.objects()[size_t(i)];
            if (p.wasActivated(i) && !o.multiActivate) continue;
            if (p.touchedRing || p.dashing) break;
            p.canRingJump = true;
            p.holdingDelayed = true;
            ringJump(W, gs, p, o, i);
            if (!o.multiActivate) p.markActivated(i);
            break;
        }
    };
    if (W.config().orbBeforeJumpOnPress) tryRings();
    if (W.config().pressJumpCheck && p.jumpUnused) updateJump(W, gs, p, 0.0f);
    if (!W.config().orbBeforeJumpOnPress && p.jumpUnused) tryRings();
}

static void releaseButton(PlayerState& p) {
    p.holding = false;
    p.jumpUnused = false;
    if (p.dashing) { p.dashing = false; p.v = 0; }
}

static void updatePlayer(const World& W, GameState& gs, PlayerState& p) {
    p.v = std::clamp(p.v, -1000.0, 1000.0);
    p.lastX = p.x;
    p.lastY = p.y;
    if (!p.dashing) updateJump(W, gs, p, kDtJump);
    if (p.dead) return;
    const auto& sp = speedParams(p.speed);
    double dx = double(sp.playerSpeed) * sp.speedMult * 0.25;
    double dy = p.v * double(kDtJump);
    if (p.mode == Mode::Wave && !p.dashing) {
        dy = std::fabs(dx) * p.flip() * (p.holding ? 1.0 : -1.0);
        if (p.mini) dy += dy;
    } else if (p.dashing) {
        p.v = 0;
        dy = dx * p.dashY;
    }
    p.x = float(p.x + float(dx));
    p.y = float(p.y + float(dy));
    // updateJumpVariables
    p.holdingDelayed = p.holding;
    p.canRingJump = p.jumpUnused;
    p.touchedRing = false;
}

GameState initialState(const World& W) {
    GameState gs;
    const auto& st = W.settings();
    PlayerState& p = gs.p[0];
    p.x = W.startX();
    p.mode = st.startMode;
    p.speed = uint8_t(std::clamp(st.startSpeed, 0, 4));
    p.mini = st.startMini;
    p.upsideDown = st.startFlipped;
    float H = modeBoundsHeight(p.mode);
    if (H > 0) { p.bounded = true; p.floorY = 0; p.ceilY = H; }
    float h = p.height();
    p.y = p.upsideDown && p.bounded ? p.ceilY - h * 0.5f : (p.mode == Mode::Wave ? h : h * 0.5f);
    if (p.mode == Mode::Wave && p.upsideDown && p.bounded) p.y = p.ceilY - h;
    p.lastX = p.x;
    p.lastY = p.y;
    p.onGround = true;
    p.canJump = true;
    if (p.mode == Mode::Robot || p.mode == Mode::Spider) p.robotCharge = 1.5f;
    if (st.startDual) {
        gs.dual = true;
        float DH = H > 0 ? H : 300;
        p.bounded = true;
        p.floorY = 0;
        p.ceilY = DH;
        PlayerState q = p;
        q.upsideDown = !p.upsideDown;
        q.y = q.upsideDown ? DH - h * 0.5f : h * 0.5f;
        q.lastY = q.y;
        gs.p[1] = q;
    }
    return gs;
}

void step(const World& W, GameState& gs, bool pressed) {
    if (gs.dead || gs.won) return;
    gs.time += kStepSec;
    ++gs.tick;
    const int n = gs.dual ? 2 : 1;
    for (int i = 0; i < n; ++i) gs.p[size_t(i)].events = 0;
    if (pressed != gs.pressed) {
        for (int i = 0; i < n; ++i) {
            if (pressed) pushButton(W, gs, gs.p[size_t(i)]);
            else releaseButton(gs.p[size_t(i)]);
        }
        gs.pressed = pressed;
    }
    for (int i = 0; i < (gs.dual ? 2 : 1); ++i) {
        PlayerState& p = gs.p[size_t(i)];
        updatePlayer(W, gs, p);
        if (!p.dead) {
            p.wasOnSlope = p.onSlope;
            p.onSlope = false;
            collidePlayer(W, gs, p);
        }
        if (p.dead) { gs.dead = true; return; }
    }
    if (gs.p[0].x >= W.endX()) gs.won = true;
}

} // namespace al::phys
