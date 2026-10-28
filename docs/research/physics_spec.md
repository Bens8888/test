# GD 2.2 (2.2081) classic-mode player physics — implementation spec

Target: a deterministic C++ re-implementation for the AutoLayout bot (`core/physics/`).
Scope: classic (non-platformer) mode, one or two players, no triggers except where noted.

## 0. Sources, tags, confidence

| Tag | Source | Use |
|---|---|---|
| **OSGD** | StarryDawn72 / iliasHDZ `open-source-gd-project` (MIT), 2.2081 reconstructions verified in-game | **Primary source.** All formulas below are from OSGD unless tagged otherwise. |
| **GDP** | camila314 `gdp` (2.2 decomp): `GJBaseGameLayer::update`, `collidedWithObjectInternal`, `collidedWithSlopeInternal`, `updateTimeMod`, `boostPlayer`, `slopeYPos`, `destroyPlayer` | Tick loop, block/slope collision. |
| **SIM** | camila314 `pathfinder/gd-sim` (validated vs game, **no redistribution licence**) | Numeric facts only: hitbox table, bounds, measured velocities, edge cases. No code reproduced. |
| **REMAKE** | `alpbayrak031-hash/GD-Remake` (no licence) | Cross-check only. |
| **MEM** | Community knowledge / recall, not present in any local source | Treat as unverified. |
| **CALC** | Computed by the stepping script `/tmp/claude-0/scratch/gdnums.py` | §16 |

Confidence markers: **[H]** code-level from OSGD/GDP; **[M]** inferred from code + SIM agreement; **[L]** SIM/REMAKE/MEM only, or sources disagree.

Functions that are **not in any local source** (behaviour must be inferred): `GJBaseGameLayer::checkCollisions` (ground/ceiling, solid & hazard loops), `PlayerObject::pushButton`, `hitGround`, `postCollision`, `spiderTestJumpInternal`, `GJBaseGameLayer::getBumpMod`, `gravBumpPlayer`, `switchToFlyMode/RollMode/RobotMode/SpiderMode`, `playerWillSwitchMode`, `playerTouchedRing`, `toggleDualMode`, `GJBaseGameLayer::flipGravity`, `getModifiedDelta`, `getMaxPortalY`.

Units: positions in GD units (1 block = 30). Vertical velocity `v` is stored in "GD velocity units": one unit moves the player 0.225 units per tick = **54 units/s**. SIM's velocities are GD × 54; SIM accelerations are GD-per-tick × 12960 (= 54 × 240).

---

## 1. Tick structure

### 1.1 Step size [H GDP]
- `GJBaseGameLayer::update(delta)`: `delta = getModifiedDelta(delta)`; `stepCount = (int)max(1, delta*240 / min(timewarp,1))`.
- Per step: `physicsDelta = delta/stepCount` (= 1/240 s), `physicsSecond = delta*60/stepCount` (= **0.25**, float).
- `getModifiedDelta` [MEM]: accumulates frame time and returns a multiple of `min(1,timewarp)/240`, carrying the remainder. ⇒ physics is exactly 240 steps/s and dt is always exactly 0.25 for timewarp 1. The bot should simulate fixed steps of 1/240 s and ignore frame timing.
- dt values per call:

| Call | dt |
|---|---|
| `PlayerObject::update(dt)` | 0.25f |
| `updateJump(dt*0.9f)` | 0.225f (`0.25f*0.9f` = 0.22499999403953552) |
| horizontal move `dx = getCurrentXVelocity()*dt` | 0.25 |
| vertical move `dy = v * (dt*0.9f)` | 0.225f |
| `checkCollisions(player, dt)` | 0.25 (physicsSecond) [M] |
| `updateRotation(physicsDelta*60)` | 0.25 |
| `updateSpecial / updateInternalActions(physicsDelta)` | 1/240 |
| `m_totalTime += physicsDelta` | 1/240 s per step (double) |

### 1.2 Order inside one step [H GDP `GJBaseGameLayer::update`]
1. `m_totalTime += 1/240`; players' `m_totalTime` copied.
2. Spawn/timers, `processCommands` (**queued input applied here**: `pushButton` / `releaseButton`).
3. `player1->resetTouchedRings(false)`, `resetCollisionLog`; same for player 2 if dual.
4. Move triggers / collision blocks update.
5. If not dead: `p1->update(0.25)`; `checkCollisions(p1, 0.25)` (may kill); `p1->updateSpecial`.
6. If dual: `p2->update(0.25)`; `checkCollisions(p2)`; `p2->updateSpecial`. (P1 fully resolves before P2.)
7. `checkSpawnObjects`, then `p1->updateRotation(0.25)`, `p2->updateRotation(0.25)` (**rotation is updated after collisions**; collisions see the previous step's angle).
8. `m_prevMaxPortalY = getMaxPortalY()`, `checkForEnd()`.
9. Camera etc.

### 1.3 `PlayerObject::update(0.25)` [H OSGD]
```
v = clamp(v, -1000, 1000)                 // direct, unrounded
lastPosition = position
updateJump(0.225f)                        // §3; sets v
if (forceTimer>0) ...                     // force blocks (skip for bot)
if (isDashing) setYVelocity(0)
dx = (double)m_playerSpeed * m_speedMultiplier * 0.25      // double
dy = v * 0.225f                                            // double
if (wave && !dashing) { dy = |dx| * flipMod * (holding ? +1 : -1); if (mini) dy *= 2; }
else if (dashing)     dy = dx * m_dashY                    // m_dashY = tan(angle)
if (goingLeft) dx = -dx
position = position + ccp((float)dx, (float)dy)            // CCPoint: float32 add
updateJumpVariables():                                     // end of update
   m_holdingJumpReleaseDelayed = m_holdingJump
   m_canRingJump = m_isJumpUnused
   m_touchedRing = m_touchedCustomRing = m_touchedTeleportRing = false
```

### 1.4 `checkCollisions` order [L — function not in sources]
OSGD `collisionCheckObjects(player, objects)` iterates the candidate list in order and:
- skips decoration / disabled; **defers** `Solid`/`Breakable` into a solid list and `Hazard`/`AnimatedHazard` into a hazard list;
- skips objects already activated by this player (`hasBeenActivatedByPlayer`);
- tests everything else **immediately in list order** (portals, pads, orbs, slopes, coins, modifiers) using the outer player rect (inclusive edges, see §8.1), circle test if `m_objectRadius>0`, plus OBB-both-ways test if the object is rotated (`m_shouldUseOuterOb`).
- Slopes are pre-tested with their rect scaled 2×2, then `collidedWithSlope`.

Recommended order (SIM, matches OSGD deferral): `preCollision()` → non-solid objects in list order → **solids** (SIM processes them in reverse collection order) → **hazards** → level floor/ceiling clamp (§9.6) → kill-above check. Candidate list = objects in the player's section and the two neighbouring sections (section width 100 units, by x) [SIM].

`PlayerObject::preCollision()` [H OSGD]: `wasOnGround=onGround; wasOnSlope=isOnSlope; isOnSlope=false; collidedTop/Bottom/Left/Right = 0; ringJumpingThisTick=false`.

### 1.5 Precision & rounding [H]
- `m_yVelocity`: **double**. `m_yStart`, `m_gravity`, `m_speedMultiplier`: double; `m_playerSpeed`, `m_playerScale`/`m_vehicleSize`: float (bindings 2.2081).
- Positions: `CCPoint` → **float32**; each step `pos = float(pos + float(delta))`.
- **Every `setYVelocity` / `addToYVelocity` rounds to 3 decimals**:
  ```
  setYVelocity(double v): ip = (double)(int)v;  if (ip != v) v = ip + std::round((v-ip)*1000.0)/1000.0;  m_yVelocity = v;
  addToYVelocity(d):       setYVelocity(m_yVelocity + d)
  ```
  (truncate toward zero, `std::round` = half away from zero; symmetric for negatives.)
- Direct, **unrounded** multiplies on `m_yVelocity`: clamp ±1000; `flipGravity` ×0.5; mode toggles ×0.5; ball jump ×0.6f; orb ball/spider ×0.7f; orb swing ×0.6f; pad ball/spider/swing ×0.6f; head-bump `m_yVelocity = 0`.
- updateJump arithmetic is **float**: `float gravity = (ball||flying||spider) ? 0.958199f : (float)m_gravity`; increments like `-(scale*gravity*dt*flipMod)` are float products, promoted to double when added.
- Because `v` lives on a 0.001 grid, the effective per-tick gravity is the rounded increment (e.g. cube 1x 0.21559 → 0.216). SIM's tables are these effective values.

---

## 2. Horizontal movement [H OSGD `updateTimeMod`, GDP]

`updateTimeMod(speed)` sets (`speed` float compared with `==`):

| Portal | ID | kA4 | m_playerSpeed | m_speedMultiplier | m_yStart (jump) | m_gravity | x-vel (ps·sm, GD/upd) | **units/tick** | **units/s** |
|---|---|---|---|---|---|---|---|---|---|
| 0.5x | 200 | 1 | 0.7f | 5.980002 | 10.620032 | 0.940199 | 4.1860013 | 1.0465003 | 251.16008 |
| 1x | 201 | 0 | 0.9f | 5.770002f (=5.77000189) | 11.180032f (=11.1800318) | 0.958199f (=0.958199024) | 5.1930016 | 1.2982504 | 311.58010 |
| 2x | 202 | 2 | 1.1f | 5.870002 | 11.420032 | 0.957199 | 6.4570023 | 1.6142505 | 387.42013 |
| 3x | 203 | 3 | 1.3f | 6.000002 | 11.230032 | 0.961199 | 7.8000023 | 1.9500005 | 468.00013 |
| 4x | 1334 | 4 | 1.6f | 6.000002 | 11.230032 | 0.961199 | 9.6000033 | 2.4000008 | 576.00019 |

- 1x branch uses float literals (GDP prints the float-rounded values); other branches double literals. Unknown speed: multiplier 5.77000189, others unchanged [GDP].
- units/s matches SIM `player_speeds` to 7 digits (251.1600797, 311.5800937, 387.4201404, 468.0001388, 576.0002006).
- `m_gravity` only affects cube & robot gravity and the `playerIsFallingBugged` threshold (2·m_gravity) for every mode. 3x and 4x share yStart/gravity ⇒ identical vertical physics.
- Speed portal hitboxes [SIM]: 200: 35×44, 201: 33×56, 202: 51×56, 203: 65×56, 1334: 69×56. Applied on overlap, once per portal; x-movement of the current step already happened ⇒ new speed from next step.
- Speed change while ball: `runRotateAction` (visual only).

---

## 3. Gamemodes — `updateJump(dt=0.225f)` [H OSGD, GDP agrees]

Notation: `flip = isUpsideDown ? -1 : 1`; `S` = mini factor; `G = 0.958199f`; `g = (float)m_gravity`; all "v" relative signs are physical unless stated.

Common definitions:
```
playerIsFalling(t)      = upsideDown ? (v > -t) : (v < t)
playerIsFallingBugged() = if (sideways||platformer||swing||m_fixGravityBug) playerIsFalling(2g)
                          else { thr = 2g (in dual: -2g … i.e. thr=2*(dual?-g:g)); upsideDown ? v > thr : v < thr }
```
**Legacy upside-down bug** [H]: with `m_fixGravityBug` off (kA32 = 0 [M]) the flipped test is `v > +2g`, *not* `v > -2g`. Consequences: coyote time when flipped (§3.10), flipped ship/UFO use "falling" coefficients while still moving up slowly, etc. Swing always uses the fixed test. In dual mode the legacy threshold sign flips (`-2g`).

Input state (set by `pushButton`/`releaseButton`):
- `m_holdingJump` — button is down.
- `m_isJumpUnused` — set on press [M], cleared on release [H] and when a jump/orb/UFO jump/swing flip consumes it [H].
- Robot: `isHoldingJump = holding && jumpUnused`; every other mode `isHoldingJump = holding`.

### 3.1 Flying modes common (ship, UFO, wave, swing)
```
scaleMod = mini ? 0.85 : 1
maxV = 8/scaleMod, minV = -6.4/scaleMod          (mini 9.4118 / -7.5294)
if v is inside its normal range (0<=v<maxV or minV<v<=0; mirrored when flipped) → m_isVelocityUncapped = false
... mode block ...
if (!uncapped && !wave):  // terminal clamp, modeScale = swing ? 1 : 0.8; swing forces scaleMod=1
   normal : v = clamp(v, -8*modeScale/scaleMod, 8/scaleMod)
   flipped: v = clamp(v, -8/scaleMod, 8*modeScale/scaleMod)
if (playerIsFallingBugged()) isJumping = false
```
Gravity used: `G` (0.958199) regardless of speed (times gravity-trigger mod).

### 3.2 Ship
```
if (holding && !uncapped) shipMod = -1
else { shipMod = uncapped ? ((flip>0 ? v<0 : v>0) ? -1 : 0.8) : 0.8
       if (!holding && !playerIsFallingBugged()) shipMod = 1.2 }
boostMod = (holding && playerIsFallingBugged()) ? 0.5 : 0.4
addToYVelocity( -(G*dt*flip*shipMod*boostMod)/scaleMod )
if (holding) onGround = false
```
Per-tick Δv (1x; raw → effective after rounding):

| state | normal | mini |
|---|---|---|
| hold, v < 2g (1.9164) | +0.107797 → +0.108 | +0.126820 → +0.127 |
| hold, v ≥ 2g | +0.086238 → +0.086 | +0.101456 → +0.101 |
| release, v ≥ 2g | −0.103485 → −0.103 | −0.121747 → −0.122 |
| release, v < 2g | −0.068990 → −0.069 | −0.081165 → −0.081 |
| caps | +8 / −6.4 (432 / −345.6 u/s) | +9.4118 / −7.5294 (508.24 / −406.59 u/s) |

SIM agrees (accelerations 1397.0491/1117.64328/−1341.1719/−894.11464; mini 1643.5872/1314.86976/−1577.85408/−1051.8984; threshold `velocity_thresholds` = 2·m_gravity·54: 101.54, 103.49, 103.38, 103.81, 103.81). No input buffering in ship (holding is level-sensitive; `jumpUnused` is never consumed by the ship).

### 3.3 UFO (bird)
```
if (jumpUnused && holding) { jumpUnused=false
    jv = (mini ? 8 : 7) * flip * scaleMod                 // 7 or 6.8
    if (flip>0 ? jv > v : jv < v) { setYVelocity(jv); slope bonus: if on/was on slope && slopeVel>0: v += slopeVel*0.5, v = min(v, 1.4*v_before) } }
birdMod = playerIsFallingBugged() ? 0.8 : 1.2
addToYVelocity( -(G*dt*flip*birdMod*0.5)/scaleMod )     // same tick as the jump
```
Δv per tick: rising (not falling) −0.129357 → −0.129 (mini −0.152185 → −0.152); falling −0.086238 → −0.086 (mini −0.101 ). Caps −6.4 / +8 (mini −7.53 / +9.41). First-tick velocity after a press from rest: 7 − 0.129 = **6.871** (371.03 u/s), mini 6.648 (358.99 u/s) — matches SIM. A UFO click never *reduces* upward velocity. One jump per press (`jumpUnused`): a press held from a previous mode that never consumed it (ship, wave) jumps immediately on entering UFO (SIM "buffer" rule).

### 3.4 Wave (dart)
- `updateJump` stores `v = xVel*flip*(holding?1:-1)` (xVel = playerSpeed·speedMult, e.g. ±5.193 at 1x) — only meaningful when leaving the wave.
- Actual motion (update, §1.3): `dy = ±|dx|` per tick (45°), **mini doubles it** (63.435°). Hold = up (relative to gravity), release = down. No acceleration, no terminal clamp, no input buffering.
- dy per tick: 1.0465 / 1.29825 / 1.61425 / 1.95 / 2.4 (mini ×2).
- Hitbox 10×10 (mini 6×6). Any slope contact kills unless "allow dart slide" modifier (1755) [H GDP]. Floor/ceiling: SIM keeps the wave centre a full hitbox height (10, mini 6) from the bound [L].

### 3.5 Swing
```
if (jumpUnused && holding) { jumpUnused=false; pre=v; flipGravity(!upsideDown, true) /*v*=0.5*/; setYVelocity(pre*0.8) }
addToYVelocity( -((mini?0.6:0.4) * G*dt*flip) )     // 0.086238 (mini 0.129357) per tick toward current gravity
scaleMod = 1 → caps ±8 for both sizes (modeScale 1, symmetric)
```
One flip per press. Flip keeps 80 % of velocity (sign unchanged ⇒ momentum carries). Swing uses the fixed falling test.

### 3.6 Cube / robot / ball / spider (ground modes)
```
gs = (ball||spider) ? 0.6 : robot ? 0.9 : 1          // (swing also listed for 0.6 but never reaches here)
grav = (ball||spider) ? G : g
if (isHoldingJump && canJump) {
   if (spider && !dashing) spiderTestJump(false)       // §3.9
   else if (!dashing) {
      isJumping=true; onGround=false; canJump=false; jumpUnused=false; robotBoostInvalidated=false; robotBoostCharge=0
      jv = (float)m_yStart * (robot ? 0.5 : 1)
      setYVelocity(jv*flip*S)                             // S = mini ? 0.8 : 1
      slope bonus (on/was on slope, slopeVel*flip>0): v += 0.25*getModifiedSlopeYVel() (ball: raw m_slopeVelocity); cap at 1.4×jump (min/max by gravity)
      if (ball) { flipGravity(!upsideDown, true) /*×0.5*/; v *= 0.6f; holdingJump = false }
      else if (!robot && !spider) runRotateAction (cube spin)
   }
} else if (isJumping) {
   if (robot && holding && !robotBoostInvalidated && robotBoostCharge < 1.5f) { robotBoostCharge += dt*0.1f; addToYVelocity(+grav*dt*flip*gs) }
   addToYVelocity(-(gs*grav*dt*flip))
   if (playerIsFallingBugged()) { isJumping=false; onGround=false }
} else {
   if (playerIsFallingBugged()) canJump = false          // (platformer: 0.05 s grace)
   addToYVelocity(-(gs*grav*dt*flip))
   terminal: normal v = max(v,-15); flipped v = min(v,15)   // only in this branch (not while isJumping)
   cube only: if playerIsFalling(-0.25) && !rotating && !onSlope → start air spin
   if (playerIsFallingBugged() && (flipped ? v>4 : v<-4)) onGround = false
}
```
Notes
- **The jump tick applies no gravity** (jump and gravity are in exclusive branches). But see §5.4/§14: SIM shows fresh clicks get one extra gravity step, consistent with `pushButton` running `updateJump(0)` [L].
- Jump velocities after rounding: 10.62 / 11.18 / 11.42 / 11.23 / 11.23; mini 8.496 / 8.944 / 9.136 / 8.984 / 8.984.
- Cube holds → re-jumps every landing (level-sensitive). Ball & spider clear `holdingJump` ⇒ need a new press (or a press held through landing = buffered). Robot needs `jumpUnused` ⇒ no auto re-jump while held.
- Terminal velocity 15 (810 u/s) cube/ball/robot/spider; not applied while `isJumping` (e.g. under a red orb launch downwards).
- Ground gravity per tick (raw → effective): cube 0.211545→0.212 (0.5x), 0.215595→0.216 (1x), 0.215370→0.215 (2x), 0.216270→0.216 (3x/4x); robot ×0.9: 0.190, 0.194, 0.194, 0.195; ball/spider 0.129357→0.129 (all speeds). SIM cube accelerations −2747.52/−2794.1082/−2786.4/−2799.36 = these ×12960 ✓; SIM ball −1676.46672 ✓.
- Mini does **not** change gravity — only jump/orb/pad velocity (×0.8) and hitbox.

### 3.7 Robot
- Jump v = 0.5·yStart (5.59 at 1x; mini 4.472). Charge starts at 0; while held and `!robotBoostInvalidated` and charge < 1.5: +0.0225 per tick and gravity cancelled (net Δv = 0 after rounding). 1.5/0.0225 ⇒ **67 boosted ticks** (0.279 s) [CALC, float32].
- Portal entry sets `robotBoostCharge = 1.5` (no boost until a jump resets it).
- `bumpPlayer` (pads) sets `robotBoostInvalidated = true` **when `m_isPlatformer || !m_fixRobotJump`** [H]. (kA34 "fix robot jump"; the code reads inverted relative to gddocs' description — implement literally, map `m_fixRobotJump` ← kA34, flag [L].)
- Release mid-boost then re-press: `releaseButton` does not set `robotBoostInvalidated`; robot `isHoldingJump` for *jump* needs `jumpUnused`, but the *boost* branch only checks `m_holdingJump` ⇒ by the code a re-press resumes boosting while charge remains [M; REMAKE assumes it does not — verify in game].
- Yellow orb ×0.9, red orb ×1.28 (§5).

### 3.8 Ball
- Press on ground → `v = flip·yStart·S` → gravity flips (×0.5) → ×0.6f ⇒ **v = 0.3·yStart·S** away from the old floor (3.354 at 1x, 3.186/3.426/3.369; mini 2.6832). Matches SIM `jumpHeights` (−181.116 u/s at 1x).
- Holding is cleared on switch ⇒ one switch per press; a press made in the air is kept (`holdingJump`) and fires on the first grounded tick (buffer). Coyote: §3.10.
- Gravity 0.6·G both directions, terminal 15.
- After the switch `isJumping` ends almost immediately (legacy flipped test v > 2g true).

### 3.9 Spider [L — `spiderTestJumpInternal` not decompiled]
- Trigger: same as cube jump branch (`isHoldingJump && canJump`), spider orb (3004 [MEM]), spider pad (3005 [MEM]); orb/pad first flip gravity so that it matches the object's facing (`isFacingDown()!=upsideDown → flipGravity`).
- Known effects: gravity flips, player is moved instantly along the new gravity direction to the nearest landing surface (solid block face whose x-range overlaps the player, or the mode floor/ceiling), `m_lastSpiderFlipTime = now`, rotation snapped to 0/180, landing as on ground (v = 0) [M].
- Spider grace [H GDP]: for 0.04 s after a spider flip, a solid whose `maxX < innerBox.maxX` (i.e. behind the player's centre) does not kill.
- Recommended model (REMAKE estimate): scan solids overlapping `[x−w/2+1, x+w/2−1]` in the new-down direction, pick nearest face; if none, use the bound; hazards are resolved by the normal hazard pass afterwards. Assume a new press is needed per teleport (like ball).
- Gravity 0.6·G, terminal 15, hitbox 27×27 (mini 16.2) [H OSGD `toggleSpiderMode`]; robot-charge set to 1.5 on entry.

### 3.10 Coyote / buffering summary [H code, M timing]
- **Ground jump buffering**: cube jumps whenever holding on a grounded tick (any press held through landing). Ball/spider: same, but each jump clears `holding`. Robot/UFO/swing: need an unconsumed press (`jumpUnused`).
- **Normal-gravity coyote = 1 tick**: `canJump` from the last landing survives into the first airborne tick's `updateJump` (jump check precedes the `canJump=false` line).
- **Flipped-gravity coyote (legacy bug)**: `canJump` stays true until `v > 2g`. Cube 1x: jumps possible for 10 ticks after leaving a ceiling; ball/spider (0.129/tick): 16 ticks. SIM: cube `coyoteFrames < 10`, ball `< 16` flipped, `< 1` normal. With kA32 (fix gravity bug) the flipped case behaves like normal (1 tick).

### 3.11 Mini summary [H]
Hitbox ×0.6 (`m_spriteWidth/HeightScale = 0.6`, immediate; the 0.5 s elastic scale is visual). Jumps/orbs/pads ×0.8. Flying: accel and caps ÷0.85 (swing: gravity 0.6 instead of 0.4, caps unchanged). UFO jump 8·0.85 = 6.8. Wave slope ×2. Ball/spider rotation visual only. Size portals: 99 normal, 101 mini, hitbox 31×90 [SIM].

---

## 4. Gravity

### 4.1 `PlayerObject::flipGravity(flip, noEffects)` [H OSGD]
If changing: `isUpsideDown = flip; m_lastFlipTime = totalTime; collidedBottomMaxY = collidedTopMinY = 0; if on slope: goingDownSlope = !goingDownSlope; resetCollisionLog; if (!respawning) v *= 0.5; canJump = false; lastGroundedPos = lastEffectObjectPos;` ball: restart ball spin; swing: fire art.
- Velocity keeps its **physical direction**, halved (SIM: `velocity = -velocity/2` in gravity-relative units ✓).
- `hardFlipGravity()`: flip then `setYVelocity(-2*flipMod)` (used by flip-gravity block modifier 2866 on head hit).

### 4.2 Portals [H OSGD `collisionCheckObjects`]
| ID | Type | Effect |
|---|---|---|
| 10 | NormalGravityPortal (blue) | flipGravity(false) |
| 11 | InverseGravityPortal (yellow) | flipGravity(true) |
| 2926 [MEM] | GravityTogglePortal (green) | flipGravity(!current) |
Each once per portal per player (`canBeActivatedByPlayer`). Hitbox 25×75 (SIM, 10/11; 2926 assumed same). Via `GJBaseGameLayer::flipGravity(player, …)` (not decompiled; may affect dual partner [L]).

### 4.3 Gravity flip grace [H GDP]
For **0.1 s** after any flip (`m_totalTime − m_lastFlipTime < 0.1`), a solid that would kill via the inner box *and* lies on the head side (`canSnap`) instead pushes the player out to the head side (`y = block.maxY + h/2` if flipped, `block.minY − h/2` if normal), `hitGround(obj, true)`, `onGround2=false`. Also slopes use `isSafeFlip(0.1)` / `isSafeMode(0.1)` (0.1 s after flip or mode change) to avoid head-first slope deaths; `isSafeHeadTest` uses 0.2 s.

---

## 5. Orbs (rings) — `ringJump` [H OSGD]

### 5.1 Activation rules
`ringJump(orb)` fires only if all hold:
1. not dead, orb not already activated by this player (`activatedRingIDs`, unless multi-activate, key 99);
2. `m_canRingJump` (= `jumpUnused` at end of this step's `update`) and not dashing;
3. `m_holdingJumpReleaseDelayed` (= `holding` at end of `update`);
4. at most **one jumping ring per step** (`m_touchedRing`; custom/teleport orbs tracked separately).
Implications:
- Pressing in the air (any mode that doesn't consume the press: cube/ball/robot/spider while airborne, ship, wave) and **holding** until the orb is reached activates it on first overlap ("click buffering" / pre-click).
- A press consumed by a ground jump in `updateJump` sets `jumpUnused=false` before collisions ⇒ the orb does not fire that step (unless `pushButton` activates touching rings first — see 5.4).
- Releasing clears it. A fresh press is needed per orb.
- `m_touchingRings` keeps rings touched in the previous step (`resetTouchedRings(false)` keeps rings present in `m_touchedRings`) ⇒ a press 1 step after leaving an orb can still hit it (SIM: orb overlap tested on current **or previous** step) [M].

### 5.2 Velocities (`v = flip·yStart·factor·S`, rounded; then ball/spider ×0.7f, swing ×0.6f; sets isJumping, canJump=false, jumpUnused=false)
Factor table:

| Orb | ID | cube | ship | UFO | ball | robot | spider | swing | wave |
|---|---|---|---|---|---|---|---|---|---|
| yellow | 36 | 1 | 1 | 1 | 1 (×0.7) | 0.9 | 1 (×0.7) | 1 (×0.6) | n/a¹ |
| pink | 141 | 0.72 | 0.37 | 0.42 | 0.77 (×0.7) | 0.72 | 0.72 (×0.7) | 0.72 (×0.6) | n/a¹ |
| red | 1333 | 1.38 | 1.0 (mini 1.4) | 1.02 (mini 1.36) | 1.34 (×0.7) | 1.28 | 1.34 (×0.7) | 1.38 (×0.6) | n/a¹ |
| blue | 84 | 0.8 then flip ×0.5 | same | same | 0.8·0.7 then ×0.5 | 0.8 | 0.8·0.7·0.5 | 0.8·0.6·0.5 | flip only |
| green | 1022 | flip first, then 1 (ship 0.7) | | | ×0.7 | 1 | ×0.7 | ×0.6 | flip only |
| black | 1330 | v = −15·flip (spider −16.5) | −14 | −14·0.8 = −11.2 | −15 | −15 | −16.5 | −14 | (−14) |

¹ In code the wave takes the velocity but its motion ignores `v` (§3.4); only the gravity part (blue/green) matters. SIM: "wave can't use non-gravity orbs".
- Black orb: no mini factor; sets `uncapped`; ball & swing: `holding=false`.
- Red orb sets `m_isVelocityUncapped = true` (flying caps off until v re-enters the normal range). Pink/yellow/blue/green leave caps on (ship/UFO clamp to 8 next tick).
- Ball/spider/swing: `holding=false` after the orb.
- Blue: velocity set in *old* gravity's up direction then halved by the flip ⇒ moves toward the new floor at 0.4·yStart (4.472 at 1x). Green: flip happens **before** the velocity set ⇒ full yellow velocity in the new up direction.
- 1x values (GD units; ×54 for u/s): yellow 11.18 (mini 8.944), ball 7.826; pink cube 8.05, ship 4.137, UFO 4.696, ball 6.026; red cube 15.428, ship 11.18 (mini 12.522), UFO 11.404 (mini 12.164), ball 10.487, robot 14.310; blue 4.472 (ball 3.130); robot yellow 10.062.
- SIM cross-check: yellow/pink/blue/black match exactly; SIM red & green values equal these **minus one tick of gravity** (e.g. red cube 821.448/54 = 15.212 = 15.428 − 0.216; red UFO 11.275 = 11.404 − 0.129) — consistent with a fresh-press orb hit getting the step's gravity (§5.4).

### 5.3 Dash orbs [H OSGD `startDashing`, `limitDashRotation`]
- 1704 green dash (DashRing), 1751 pink/gravity dash (GravityDashRing: flips gravity then dashes). Hitbox 36×36 assumed.
- Angle: `a = fmod(−orbRotation (+180 if flipX, +180 if |rotX−rotY|>179), 360)`, normalized; if `|a|>90` mirror forward (`±180 − a`); clamp to **±70°** (offset by 180 if going left). `m_dashY = sin/|cos| = tan(a)`, `m_dashX = |m_dashY|`.
- While dashing: `v = 0` each step, `dy = dx·tan(a)` (|dy/dx| ≤ 2.747), orbs ignored, rotation frozen. Ends on button release (`releaseButton → stopDashing`), dash-stop modifier 1829, or (platformer only) wall contact. After the dash `v = 0` and gravity resumes.
- Can't start a dash while dashing.

### 5.4 Press-time behaviour [L]
`pushButton` is not decompiled. Evidence (SIM: fresh-click jumps get one gravity step; held-button jumps don't; SIM red/green orb values include a gravity step; REMAKE: older decomps call `updateJump(0)` in pushButton) ⇒ **recommended**: on press, set `holding=true, jumpUnused=true`, then call `updateJump(0)` (a zero-dt jump check), then also try `ringJump` on `m_touchingRings` [MEM]. Make the orb-vs-ground-jump priority on the press tick a config flag (default: orb wins).

### 5.5 Other orbs
Spider orb (3004 [MEM]): face-match flip + spider teleport. Teleport orb (3027 [MEM]): teleports, consumes press. Toggle/custom orb (1594): no velocity. Spider/teleport orbs bypass the one-jumping-ring-per-step rule partially (separate flags).

---

## 6. Pads — `GJBaseGameLayer::bumpPlayer` → `PlayerObject::bumpPlayer` → `propellPlayer` [H, bumpMod L]

- No input needed; once per pad per player. Pads set `lastEffectObjectPos = pad + (0,−10)`.
- `propellPlayer(m)`: `isJumping=true; onGround=canJump=false; isOnSlope=wasOnSlope=false; setYVelocity(m·16·flip·S); if (ball||spider||swing) v *= 0.6f`.
- `uncapped = (red pad)`; otherwise false.
- `getBumpMod` (not decompiled) — derived from SIM tables (value/54/16, mini and 0.6 removed); REMAKE derives the same:

| Pad | ID | hitbox (SIM) | cube/robot | ship | UFO | ball | spider/swing |
|---|---|---|---|---|---|---|---|
| yellow | 35 | 25×4 | 1.0 (v 16) | 1.0 → capped 8 | 1.0 → capped 8 | 1.0 (v 9.6) | 1.0 [L] |
| pink | 140 | 25×5 | 0.65 (10.4) | 0.35 (5.6) | 0.4 (6.4) | 0.7 (6.72) | 0.65? [L] |
| red | 1332 | ? (25×6 assumed) | 1.25 (20) | 0.63 (10.08), mini 0.95 (12.16) | 0.6 (9.6), mini 0.98 (12.544) | 1.25 (12.0) | 1.25? [L] |
| blue | 67 | 25×6 | 0.8 → flip ×0.5 ⇒ 6.4 toward new floor | same (SIM 1x+) | same | 0.8·0.6 ⇒ 3.84 | ? |
| spider | 3005 [MEM] | ? | face-match flip + spider teleport | | | | |
- Blue pad (gravBumpPlayer, SIM): only acts if the pad faces the current gravity (floor pad for normal, ceiling pad — rotation 180 / flipY — for flipped); can't flip twice in one step; flips gravity. Wave: velocity sign flip only.
- Robot: `robotBoostInvalidated` per §3.7.
- SIM blue-pad 0.5x ship/UFO/ball entries equal blue-orb values (likely a SIM table error).

---

## 7. Portals

| ID | Portal | Type | Hitbox (SIM) | Effect |
|---|---|---|---|---|
| 12 | cube | CubePortal | 34×86 | `switchedToMode(Cube)` (turns off all others) |
| 13 | ship | ShipPortal | 34×86 | toggleFlyMode, bounds 300 |
| 47 | ball | BallPortal | 34×86 | toggleRollMode, bounds 240 |
| 111 | UFO | UfoPortal | 34×86 | toggleBirdMode, bounds 300 |
| 660 | wave | WavePortal | 34×86 | toggleDartMode, bounds 300 (ignored in platformer) |
| 745 | robot | RobotPortal | 34×86 assumed | toggleRobotMode, no bounds [M] |
| 1331 | spider | SpiderPortal | 34×86 assumed | toggleSpiderMode, bounds 240 [MEM] |
| 1933 | swing | SwingPortal | 34×86 assumed | toggleSwingMode, bounds 300 [MEM] |
| 99 / 101 | normal / mini size | Regular/MiniSizePortal | 31×90 | togglePlayerScale |
| 10 / 11 / 2926 | gravity normal / flip / toggle | | 25×75 | §4.2 |
| 286 / 287 | dual on / off | DualPortal / SoloPortal | ? | toggleDualMode §12 |
| 45 / 46 | mirror on / off | Inverse/NormalMirrorPortal | ? | visual screen flip only — ignore for physics |
| 747 / 749 | teleport in / out | TeleportPortal | ? | `teleportPlayer` to linked portal (+ key 54 y-offset); player rect recomputed; `playerTeleported`: onGround=false |
| 200–203, 1334 | speed | Modifier | §2 | updateTimeMod |

### 7.1 Mode switch velocity scaling [H OSGD toggles; L where SIM disagrees]
Each of `toggleFlyMode / toggleBirdMode / toggleDartMode / toggleSwingMode` multiplies `v *= 0.5` on **both enable and disable**, and sets `onGround=canJump=false`, rotation 0. Ball/robot/spider toggles do not scale velocity. `toggleXMode(true)` calls `switchedToMode`, which disables every other mode first. ⇒ factor = (0.5 if leaving a flying mode) × (0.5 if entering one).

| from \ to | cube/robot/ball/spider | ship/UFO/wave/swing |
|---|---|---|
| cube/robot/ball/spider | ×1 | ×0.5 |
| ship/UFO/wave/swing | ×0.5 | ×0.25 |

SIM (validated) agrees for cube↔ship/UFO, ship↔UFO, ship/UFO→ball, ball→cube; **disagrees for wave exits**: SIM uses v_wave·0.9 (=stored wave v, consistent) then cube ÷4, ship ÷4, UFO ÷2, ball ÷1. Unknown `playerWillSwitchMode` may explain it. Default: follow OSGD; expose a flag. Wave exit velocity matters little (≤ 5.19·0.5).
Hitbox on switch: dart sets 10×10, spider 27×27, `resetPlayerIcon` (fly/bird/dart/swing off) restores 30×30. Spider→other not via resetPlayerIcon — assume 30×30 restored [M].

### 7.2 Mode bounds (floor/ceiling) [SIM, L for spider/swing]
On touching a bounded-mode portal at portal centre y `Py` (level-string coords):
```
floor   = max(0, 30*ceil((Py - (H/2 + 30)) / 30))
ceiling = floor + H        H = 300 (ship, UFO, wave, swing[MEM]), 240 (ball, spider[MEM]), unbounded (cube, robot: floor=0, no ceiling)
```
Recomputed on every mode portal (even same mode) [SIM]. Examples: ship portal at y=165 → floor 0, ceiling 300; at y=255 → floor 90, ceiling 390. Ball portal at y=135 → floor 0, ceiling 240. The level-string vs game-layer 90 offset is a multiple of 30, so the formula is identical in either convention. GD names: `getMaxPortalY`/ground restriction (not decompiled).
Level start (kA2 bounded mode): floor 0, ceiling H [SIM].

---

## 8. Hitboxes

### 8.1 Rect semantics [H OSGD]
`getObjectRect(sx,sy)`: size = `(m_width·m_scaleX·sx, m_height·m_scaleY·sy)`, swapped if `m_hasSwappedDimensions` (90/270° rotation), centred at `realPosition + getBoxOffset()`. Default `getObjectRect()` uses the sprite scale (player: 0.6 when mini). Overlap tests are **inclusive** (`maxX < minX` ⇒ miss; touching edges counts).

### 8.2 Player
| Mode | outer (normal) | outer (mini) | inner = `getObjectRect(0.3,0.3)` |
|---|---|---|---|
| cube, ship, ball, UFO, robot, swing | 30×30 | 18×18 | 9×9 |
| wave | 10×10 | 6×6 | 3×3 |
| spider | 27×27 | 16.2×16.2 | 8.1×8.1 |
- Inner box uses `m_width·0.3` **without** the mini sprite scale ⇒ mini cube inner stays 9×9 [M: by code; SIM hard-codes 9×9 for all states; REMAKE scales it to 5.4 — disagreement].
- Outer box: hazards, orbs, pads, portals, block landing/bumping. Inner box: only "ran into a solid" death (§9).
- Rotated box (`getOrientedBox`, angle = `getRotation()`): used **only** against objects rotated by a non-multiple of 90° (both OBBs must overlap one-way each way). Axis-aligned objects use the unrotated outer AABB.
- Size change applies immediately to the hitbox (SIM: at end of the step).

### 8.3 Hazards (SIM; size = w×h centred on object position; scale keys 32/128/129 multiply; rotation rotates the box)
| IDs | w×h |
|---|---|
| 8, 144, 177, 216 (spike) | 6×12 |
| 39, 205, 217 (small spike) | 6×5.6 |
| 103, 145, 218 (medium spike) | 4×7.6 |
| 9, 1715 (ground spikes) | 9×10.8 |
| 61, 446, 1719, 1728 | 9×7.2 |
| 365, 667, 1716, 1730 | 9×6 |
| 989, 1732 | 9×12 |
| 368, 1722 | 9×4 |
| 421, 1725 | 9×5.2 |
| 422, 1726 | 6×4.4 |
| 243, 1720 | 6×7.2 |
| 244, 1721 | 6×6.8 |
| 178 | 6×6.4 |
| 179 | 4×8 |
| 135, 1711 | 14.1×20 |
| 1712 | 13.5×22.4 |
| 1713 | 11.7×20 |
| 1714 | 11.4×16.4 |
| 392, 458, 459 | 2.6×4.8 |
| 447, 1729 | 5.2×7.2 |
| 768, 1727 | 4.5×5.2 |
| 720, 991, 1731, 1733 | 2.40039063×3.20001221 |
| 919 | 25×6 |
Saws (circle, **radius**): 88, 186, 740, 1705: 32.3 · 89, 1706: 21.6 · 98, 1707: 12 · 183: 15.66 · 184: 20.4 · 185: 2.85 · 187, 741: 21.96 · 188, 742: 12.6 · 397, 1708: 28.9 · 398, 1709: 17.44 · 399, 1710: 12.9 · 675, 1734: 32 · 676, 1735: 17.51 · 677, 1736: 12.48 · 678: 30.4 · 679: 18.54 · 680: 10.8 · 918: 24 · 1582, 1583: 4 · 1619: 25 · 1620: 15 · 1701–1703: 6.
Circle test [H OSGD]: legacy (kA39 off) `objectIntersectsCircle`: hit iff saw centre inside player rect **or** any player-rect corner strictly within radius (a circle grazing a rect edge between corners does *not* hit). kA39 on: `dist(playerCentre, sawCentre) ≤ r + playerWidth/2`.
`getBoxOffset()` non-zero offsets for some objects are not covered locally [L].

### 8.4 Solids (SIM)
| IDs | w×h |
|---|---|
| 1–4, 6, 7, 63, 69–72, 74–78, 81–83, 90–96, 116–119, 121–122, 146, 160–163, 165–169, 173, 175, 207–210, 212–213, 247–250, 252–258, 260–261, 263–265, 267–272, 274–275, 467, 469–471, 1203–1204, 1209–1210, 1221–1222, 1226 | 30×30 |
| 64, 195, 206, 220, 661, 1155–1157, 1208, 1910 | 15×15 |
| 40, 147, 215, 369, 370, 1903–1905 (slabs) | 30×14 |
| 62, 65, 66, 68 | 30×16 |
| 170–172, 174, 192 | 30×21 |
| 662–664 | 30×15 |
| 468, 475, 1260 | 30×1.5 (468 with h=5 → 1.5) |
| 1202, 1262 | 30×3 |
| 1220, 1264 | 30×6 |
| 1227 | 30×7 |
| 1561 | 30×10 |
| 1562 | 30×2 |
| 196, 219, 1911 | 15×8 |
| 204 | 8×15 |
| 1567 | 15×10 |
| 1566 | 12×12 |
| 1565 | 17×17 |
| 328 | 22×22 · 197: 22×21 · 194: 21×21 · 176: 14×21 · 1343: 25×3 · 1340: 27×2 · 34: 37×23 |
| 143 (breakable) | 30×30 |
Solids rotated by 90/270 swap w/h; other angles: OBB (no landing support in SIM).

### 8.5 Interactive objects (SIM)
Orbs 36, 84, 141, 1022, 1330, 1333: 36×36 (1704, 1751, 3004, 3027 assumed 36×36). Pads: §6. Portals: §7.

---

## 9. Block collision — `collidedWithObjectInternal` [H GDP; classic, static block]

Let `g = flip`, `h` = outer height, `feet = y − g·h/2`, `head = y + g·h/2`, `d = y − lastY` (this step's movement), `T` = snap threshold:
```
T = 10 (cube, ball, robot, spider) ; 6 (ship, UFO, wave, swing)   // 15 while m_stateScale≥1 (2 ticks after size change, platformer only); 5 platformer
if wasOnSlope: T += (h/2)/cos(slopeAngle) − h/2
```
Gravity-relative faces: `floorFace` = block top (normal) / block bottom (flipped); `headFace` = the other.

1. **Landing face check** (all modes except wave; in code the physical top face is handled first — landing for normal gravity, head bump for flipped flying/ball — and the bottom face second, mirrored):
   `reach = feet + g·T`. If `reach` (or `reach − d`) is at/past the floor face (`g·(reach − floorFace) ≥ 0` for either) then:
   - if `g·v > 0` (moving away from floor, e.g. pad launch) and not on a slope ⇒ **ignore** (no landing, no death this block);
   - else ⇒ **land**: `y = floorFace + g·h/2`, `hitGround(obj)` (v = 0, onGround, canJump, isJumping=false [M]); cube: `checkSnapJumpToObject(obj)` (§9.4).
   Otherwise the floor face is out of reach ⇒ block is a wall/ceiling candidate.
2. **Head bump** (only ship, UFO, wave², swing, ball; plus any mode with hit-head modifier 1859 or flip modifier 2866): if `headFace` is within `T` of `head` (not deeper, also checked with `−d`) and `g·v ≥ 0` ⇒ `y = headFace − g·h/2`, `hitGround(nullptr)` with onGround/canJump/lastLandTime **restored** (velocity zeroed, ceiling is not ground). With 2866: `hardFlipGravity` (v = −2·flip after flip). Cube/robot/spider: no bump — the head face is ignored and only the inner-box test applies (outer box may overlap the block harmlessly).
   ² wave: `boolJ` is forced true (no landing/bump logic) unless dart-slide (1755) ⇒ wave never lands on blocks, only floor/ceiling bounds stop it [H].
3. **Death test**: if not landed/bumped and block not passable: if `innerBox ∩ block` ⇒
   - within 0.1 s of a gravity flip and the block is on the head side ⇒ push out (§4.3);
   - spider within 0.04 s of a teleport and `block.maxX < inner.maxX` ⇒ ignore;
   - breakable (143) ⇒ destroy block, no death;
   - else **destroyPlayer**.
Notes: equivalent SIM rule: inner 9×9 overlap ⇒ death; else land if `blockTop − feet ≤ 10` (7 for ship/UFO) and `v ≤ 0`; ship/UFO/ball bump if `head − blockBottom ≤ 6`. Blocks at a slope junction are ignored if within 2 units of the slope surface (`slopeYPos ± 2`, slope rect scaled 1.2×1.1) [H GDP].
- UFO upside-down/normal breaking breakable blocks with the inner box: destroys block [H].
- Flying modes landing on a moving block get `v = groundYVelocity`. Moving blocks: `T += |Δy_block|/dt`.

### 9.4 Stair snap — `checkSnapJumpToObject` (cube only, on landing) [H OSGD]
If the previous snapped object was a different Solid:

| speed (playerSpeed) | step-up dx (+30 y) | step-up dx mini | step-down dx (−30 y) | double step-up dx (+60 y) | tolerance |
|---|---|---|---|---|---|
| 0.7 (0.5x) | 90 | 90 | 120 | 60 | 1 |
| 0.9 (1x) | 120 | 90 | 150 | 90 | 1 |
| 1.1 (2x) | 150 | 90 | 195 | 120 | 2 |
| 1.3 (3x) | 180 | 90 | 225 | 135 | 2 |
| 1.6 (4x) | 120 | 120 | 150 | 90 | 1 (falls to defaults) |
(y deltas are `flipMod·30/60`; mini test uses `m_vehicleSize != 1`.) If `|Δx − table| ≤ tol` and `|Δy − table| ≤ tol`: target `x = obj.x + snapDistance`; move player x toward it by at most `tol`. Always: `snappedTo = obj; snapDistance = player.x − obj.x`.

### 9.5 Hazards
Any outer-box overlap (inclusive) with a hazard rect (OBB if rotated, circle for saws) ⇒ death. Hazards processed after solids [SIM].

### 9.6 Level floor / ceiling [SIM; L]
- Floor y = 0 (level-string) or mode floor (§7.2); ceiling only in bounded modes / dual.
- Gravity-floor contact (`feet` past bound, `g·v ≤ 0`): snap `y = bound ± h/2` (wave: ± h), `hitGround`.
- Gravity-ceiling contact: flying modes & ball: snap, `v = 0` if moving into it. **Cube/robot: death** (flipped cube touching the ground dies; SIM cube dies when inner top ≥ ceiling). Ball touching a ceiling bound with input flips gravity (SIM edge case).
- Unbounded flipped cube rises until the kill height.

---

## 10. Slopes [H GDP/OSGD; concise]
- IDs [SIM]: 1×1 (45°, 30×30): 289, 294, 299, 305, 309, 315, 321, 326, 331, 337, 343, 349, 353, 371, 483, 492, 651, 665, 673, 709, 711, 726, 728, 886, 1338, 1341, 1344, 1723, 1743, 1745, 1747, 1749, 1906. 2×1 (26.565°, 60×30): 291, 295, 301, 307, 311, 317, 323, 327, 333, 339, 345, 351, 355, 367, 372, 484, 493, 652, 666, 674, 710, 712, 727, 729, 887, 1339, 1342, 1345, 1724, 1744, 1746, 1748, 1750, 1907. Hazard slopes: 363, 1717 (30×30); 364, 366, 1718 (60×30).
- Orientation: `rot/90` + flips (flipX+flipY: +2, flipX: +1, flipY: +3) mod 4; 0 = floor uphill, 1 = floor downhill, 2/3 = ceiling variants [SIM].
- Surface: `slopeYPos(x)`: linear along the diagonal of the object rect, extended beyond the rect; hazard slopes offset by 4 toward the free side.
- Player on slope: `y = slopeYPos(x) ± (h/2)/cos(angle)`, clamped to `[minY, maxY + h/2]` (floor-top: mirrored). Contact accepted within `float_g` = 1 (4 if already on slope) + slope's own downward speed.
- Flat sides of a slope behave like 1-unit thick walls/floors (`preSlopeCollision`).
- `slopeYVel = h·playerSpeed·speedMult / w`; `m_slopeVelocity = min(1.12/angle_rad, 1.54)·slopeYVel·flip·(±)`; ×0.75 for flying and ball. 45°: factor 1.426; 26.57°: 1.54 ⇒ 1x: 45° → 7.405, 26.57° → 3.999 (GD units).
- `getModifiedSlopeYVel` = slopeVel × (time on slope < 0.1 s ? max(0.4, 10·t) : 1).
- Jump on uphill slope: +0.25·modified (ball: raw), cap 1.4× jump velocity. UFO: +0.5·slopeVel, cap 1.4×.
- Leaving the top of an uphill slope: launched with the (modified) slope velocity [SIM: `0.9·min(1.12/a,1.54)·(h·speed_ups/w)·clamp(10t,0.4,1)`, ship/ball ×0.75, UFO ×0.7499] — postCollision not decompiled [M].
- Leaving the bottom of a downhill slope: `v = −xVel·h/w` (SIM `falls[]`/54 = 4.186, 5.193, 6.457, 7.8 = xVel ✓).
- Landing on slopes: velocity kept if `g·v > 5` (pad/orb boosts survive), else hitGround.
- Deaths: hazard slopes on contact; wave on any slope (unless 1755); cube/robot/spider hitting a ceiling-side slope head-first (allowed if within 2 units → pushed with `v ≤ −2·g`, or within 0.1 s of flip/mode change); transitioning between slopes of opposite floor-top orientation (`isNewSlope`) in classic.
- Flying into a ceiling slope uphill clamps `v` to ∓2; flying on floor slope while holding clamps to ±2.

---

## 11. Death conditions
1. Outer box ∩ hazard (§8.3, §9.5).
2. Inner box ∩ solid without landing/bump (wall, cube head hit) (§9), minus graces (§4.3, §3.9) and breakables.
3. Gravity-ceiling bound for cube/robot (flipped cube on the ground, cube on dual/bounded ceiling) (§9.6).
4. Slopes (§10).
5. `y > 1476.3` (level-string coords; ≈ 1566.3 in game-layer coords) [SIM; raised by "dynamic level height" kA37] [L].
6. In dual mode either player dying ends the attempt (`destroyPlayer` destroys the other) [H GDP].
7. Squeeze (kA31) — only if enabled; not modelled.

---

## 12. Dual mode [L — `toggleDualMode` not decompiled]
- Portal 286 enables, 287 disables; once per portal.
- P2 spawned at P1's position with **opposite gravity**, same mode/size/speed (MEM, REMAKE estimate); `m_isBeingSpawnedByDualPortal` during spawn.
- 1-player mode (kA10 = 0): the jump button drives both players (both receive push/release). 2-player mode: separate buttons.
- Update order: P1 update+collide, then P2 (§1.2). Portals affect only the touching player.
- `playerIsFallingBugged` legacy threshold uses `−2g` in dual (`m_isInDualMode`) [H].
- Bounds: dual adds floor+ceiling (cube/robot get one too); height unknown — REMAKE assumes 300 for cube/robot, mode height otherwise [L].
- `GJBaseGameLayer::flipGravity` may mirror the partner [L].

---

## 13. Level start & coordinates

### 13.1 Header keys (`{levelStart};{objects}`, comma-separated k,v) [gddocs, SIM]
| Key | Meaning |
|---|---|
| kA2 | start mode: 0 cube, 1 ship, 2 ball, 3 UFO, 4 wave, 5 robot, 6 spider, 7 swing |
| kA3 | mini |
| kA4 | speed: **0 = 1x, 1 = 0.5x**, 2 = 2x, 3 = 3x, 4 = 4x (gddocs' u/s column has 0/1 swapped) |
| kA8 | start in dual |
| kA10 | 2-player mode |
| kA11 | start flipped gravity |
| kA20 | reverse gameplay · kA28 mirror · kA29 rotate gameplay (not modelled) |
| kA22 | platformer (reject for classic bot) |
| kA31 | enable player squeeze |
| kA32 | fix gravity bug → `m_fixGravityBug` [M] |
| kA34 | fix robot jump → `m_fixRobotJump` [M] |
| kA37 | dynamic level height |
| kA39 | fix radius collision (saw circle test) |
| kA40 | enable 2.2 changes |
Start Pos object: ID 31 with the same kA keys (kA9 = 1), position from keys 2/3.

Object keys used: 1 id, 2 x, 3 y (object **centre**), 4 flipX, 5 flipY, 6 rotation (deg, CW+), 32 scale, 128 scaleX, 129 scaleY, 13 portal "checked", 54 teleport y-offset, 99 orb multi-activate, 121 no-touch.

### 13.2 Coordinates [SIM H-ish, MEM for the offset]
- Physics can run **entirely in level-string coordinates**: floor top y = 0, x and y are object centres, 1 block = 30.
- Player start: centre `(0, 15)` (feet on the floor for a 30-tall cube) [SIM]. Mini start: SIM keeps y = 15 (feet 6 above floor, falls onto it) [L]. Start Pos: player centre = start-pos (x, y).
- A block on the ground has y = 15; a spike on the ground y = 15; its 6×12 hitbox spans y 9–21.
- In-game the object layer adds **+90** to every y (ground top at layer y = 90, player on ground at 105) [MEM]; physics is translation-invariant, the only absolute constants are the floor (0 ↔ 90) and the kill height (1476.3 ↔ 1566.3).
- Level end: last object x + 340 [MEM, REMAKE].

---

## 14. Edge cases worth replicating
1. **Ground jitter / "falls a frame early"**: each grounded tick gravity is applied (v = −0.216) and the player sinks 0.0485 before collision snaps it back. Walking off an edge therefore starts the fall already at −0.216 (SIM's "fall off one frame early"). Emerges automatically from the OSGD order; don't zero v before integration.
2. Jump-tick gravity: held-button jumps (landing while holding) apply no gravity on the jump tick; fresh presses apparently do (§5.4).
3. Pad while overlapping a block top: `g·v > 0` ⇒ no landing that step (SIM "padHitBefore").
4. Blue orb/pad flips: SIM skips solid collisions for the flip step; GD instead relies on the 0.1 s flip grace + collision-log reset [M].
5. Gravity portal + SIM hardcodes (`accel −= 6.48` when v > 350 u/s at speed > 1x; sign flip in fall-off branch) are artifacts of SIM's integrator — do **not** port; use OSGD order.
6. Flipped-gravity coyote (10 / 16 ticks) — legacy bug, removed by kA32.
7. Ship can't buffer; UFO/swing consume one press; a press held through ship/wave is still unused when entering UFO/swing ⇒ instant UFO jump / swing flip.
8. Ball: a press during air is remembered and fires on landing; holding through a mode change into ball also fires once.
9. Orbs: one jumping orb per step; previous-step overlap still counts for a fresh press; can't activate while dashing.
10. Mini inner box stays 9×9 (by code) — affects mini wall/ceiling deaths [L].
11. Velocity rounding to 0.001 makes effective gravity the rounded increment; values not on the grid after ×0.5/×0.6/×0.7 multiplies get re-gridded by the next add.
12. Legacy saw circle test misses edge-only contacts.
13. Robot boost cancellation with pads depends on kA34 (§3.7).
14. Speed portal takes effect from the next step's x movement; size portal immediately for collisions in the same step after it (processed in list order) [M].
15. Wave touching any slope dies; wave cannot land on blocks (only bounds stop it).

---

## 15. Discrepancies between sources (summary)
| Topic | OSGD/GDP | SIM | REMAKE | Note |
|---|---|---|---|---|
| wave → other mode velocity | ×0.5 (×0.25 into flying) | cube ÷4, ship ÷4, UFO ÷2, ball ÷1 (of 0.9·speed) | OSGD | flag |
| mini inner box | 0.3·m_width (9) | 9 always | 5.4 | flag |
| red/green orb velocity | formula | formula − 1 gravity tick | formula | press-tick gravity |
| SIM blue pad 0.5x ship/ball | — | equals blue-orb value | — | SIM table error |
| cube gravity 0.5x/2x/3x | 0.940199/0.957199/0.961199·0.225 | rounded effective 0.212/0.215/0.216 | — | consistent after rounding |
| spider/swing bounds | n/a | n/a | 240/300 (MEM) | unverified |
| pushButton | n/a | implies updateJump(0) | updateJump(0) | inferred |
| lily-pi docs | — | — | — | cube g "79 b/s²" (2370 u/s²) is wrong; real 2794 u/s² (93.1 b/s²) at 1x |
| gddocs kA4 u/s column | — | — | — | 0/1 rows swapped |

---

## 16. Worked numbers [CALC, `gdnums.py`, float32 positions, rounding per §1.5, landing when feet ≤ 0 with v ≤ 0]

### 16.1 Cube jump from flat ground (held-button model: no gravity on jump tick)
| speed | apex (units) | apex (blocks) | airtime ticks / s | distance units / blocks |
|---|---|---|---|---|
| 0.5x | 61.05 | 2.035 | 102 / 0.425 | 106.74 / 3.56 |
| 1x | 66.36 | 2.212 | 105 / 0.4375 | 136.32 / 4.54 |
| 2x | 69.53 | 2.318 | 108 / 0.450 | 174.34 / 5.81 |
| 3x | 66.95 | 2.232 | 105 / 0.4375 | 204.75 / 6.83 |
| 4x | 66.95 | 2.232 | 105 / 0.4375 | 252.00 / 8.40 |
| mini 0.5x | 39.26 | 1.309 | 82 / 0.342 | 85.81 / 2.86 |
| mini 1x | 42.68 | 1.423 | 84 / 0.350 | 109.05 / 3.64 |
| mini 2x | 44.71 | 1.490 | 86 / 0.358 | 138.83 / 4.63 |
| mini 3x/4x | 43.05 | 1.435 | 85 / 0.354 | 165.75 / 5.53 (3x), 204.0 / 6.80 (4x) |
Fresh-press model (one gravity step on the jump tick): 1x apex 63.85 (2.128 b), 103 ticks, 4.46 b; 0.5x 58.66/100; 2x 66.96/106; 3x 64.42/103; mini 1x 40.66/82.

### 16.2 Robot (1x; others in script output)
| | normal | mini |
|---|---|---|
| min (tap, released next tick) apex / airtime / distance | 18.75 u / 59 t / 2.55 b | 12.10 u / 48 t / 2.08 b |
| max (held, 67 boost ticks) | 103.02 u / 166 t / 7.18 b | 79.52 u / 151 t / 6.54 b |
0.5x: min 17.29, max 97.34 · 2x: 19.56 / 105.63 · 3x/4x: 18.83 / 103.47.

### 16.3 Ship (all speeds; gravity independent of speed except the 2g threshold)
- Max climb 8 → **432 u/s** (14.4 b/s); max fall 6.4 → **345.6 u/s** (11.52 b/s). Mini 9.412 → 508.25 u/s; 7.529 → 406.57 u/s.
- 0 → +8 holding: 89 ticks (0.37 s); 0 → −6.4 released: 93 ticks; −6.4 → +8 holding: 148 ticks (0.62 s). Mini: 90 / 93 / 149.
- Ratios vs horizontal at 1x: climb slope 432/311.58 = 1.387 (54.2°), fall 1.109 (47.96°).

### 16.4 UFO hop from rest on the floor (speed-independent)
Apex 43.42 u (1.45 b), 128 ticks to land (0.533 s). Mini: 34.65 u, 106 ticks.

### 16.5 Ball switch across 240-unit bounds (floor→ceiling, 210 units of travel)
| speed | v0 | ticks | s | horizontal blocks |
|---|---|---|---|---|
| 0.5x | 3.186 | 99 | 0.4125 | 3.45 |
| 1x | 3.354 | 98 | 0.408 | 4.24 |
| 2x | 3.426 | 98 | 0.408 | 5.27 |
| 3x | 3.369 | 98 | 0.408 | 6.37 |
| 4x | 3.369 | 98 | 0.408 | 7.84 |
| mini 1x | 2.683 | 106 | 0.442 | 4.59 |

### 16.6 Wave
dy per tick = dx: 1.0465 / 1.2983 / 1.6143 / 1.95 / 2.4 (45°); mini ×2 (63.43°). Over one block of x the wave moves 30 units (mini 60).

### 16.7 Swing
Gravity 0.08624/tick (→0.086 effective; 1117 u/s²), mini 0.12936 (→0.129). Caps ±8 (432 u/s). Flip keeps 80 % velocity.

### 16.8 Per-tick constants (GD units)
| | 0.5x | 1x | 2x | 3x/4x |
|---|---|---|---|---|
| cube gravity raw | 0.2115448 | 0.2155948 | 0.2153698 | 0.2162698 |
| robot gravity raw | 0.1903903 | 0.1940353 | 0.1938328 | 0.1946428 |
| ball/spider gravity | 0.1293569 | = | = | = |
| fallingBugged threshold 2g | 1.880398 | 1.916398 | 1.914398 | 1.922398 |
| jump v (normal / mini) | 10.62 / 8.496 | 11.18 / 8.944 | 11.42 / 9.136 | 11.23 / 8.984 |
SIM cross-checks: all cube/ball/ship/UFO accelerations, jump velocities, caps, thresholds and orb/pad values listed in §3, §5, §6 reproduce SIM within rounding, except the items in §15.
