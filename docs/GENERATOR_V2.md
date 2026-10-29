# Generator v2: path first, structures around it

Version 1 chose a block pattern per click and then checked that the player survives. Real
layouts are made the other way round, and that is what v2 does:

1. **Clicks.** From the music (automatic) or from a recording, as before (`DesignPlan::clicks`).
2. **Gameplay path.** Walk through the clicks with the physics simulator. At every click, choose
   what the click does; between clicks, choose what keeps the player moving. Small search (beam)
   over candidate actions, scored for fairness, variety, music and style.
3. **Structures around the path.** Pillars, platforms, spikes, saws and slopes placed in the free
   space around the path's timing envelope. Every click is made necessary: the path the player
   takes without the click (or with it too early or too late) runs into a hazard.
4. **Look.** Structures are kept as solid cells and slopes, then tiled with default blocks (the
   default, as in most layout videos) or with the outline set (white silhouettes). The path's
   platforms use the same look.
5. **Verify.** The bot plays the designed inputs, timing windows are measured, failures are
   repaired (as in v1).

## Actions

| Mode | At a click | Between clicks (no click) |
|---|---|---|
| cube | jump from a platform; orb (yellow, pink, red, blue, green, black, green/pink dash) | land on a platform, pad (yellow, pink, red, blue), gravity portal, mode/speed/size portal in mid-air |
| robot | jump (hold length = the click's hold), orb | as cube |
| ship, ufo | tap or hold | gravity portal, mode portal; ufo: orbs |
| wave | every click turns the wave: one click presses, the next releases it (music clicks), or press and hold for half the gap (even zig-zag) | gravity portal, mode portal |
| swing | tap (direction change) | gravity portal, mode portal |
| ball | flip from a surface; blue/green orb | surface to land on, blue pad, gravity portal |
| spider | teleport flip; spider orb | surface, spider pad |

## Segments and switches
Gamemode, speed and size change in mid-air at musical boundaries. The switch rate comes from the
style (modern layouts change every 1-4 clicks, classic ones every section) and the music (drops
switch on the drop and speed up). The mode is chosen for the click rhythm: fast streams suit
wave/ship/swing, spaced clicks cube/robot/ball/ufo, long notes holds.

## Structures (modelled on reference layouts)
- walls that follow the path, the backbone of most layouts: pillars hanging from the top and
  rising from the floor, broken into columns 1-3 wide with gaps. Each column ends right at the
  reserved space, so the walls step along with the path, and has a spike tip pointing at the
  player.
- small block groups floating next to the path, above or below it: a hanging block with a spike
  under it, slabs of 2-4 with a spike at one or both ends, 2x2 blocks, T shapes, steps, short
  columns with a spike tip. Now and then there is a spike on the front, pointing at the player.
  Groups keep a free cell between them, so they stay separate shapes instead of one blob. In
  flying stretches they also stand on, or hang from, the floor and ceiling, and they are placed
  more densely.
- hanging pillars (from the top) and rising pillars (from the floor) with spike tips, over jump
  apexes and under the path
- saw clusters in the pockets next to the path (a saw may cut into a wall, but it is never buried
  in it)
- wave parts, built from presets the way wave layouts are:
  - a 45° triangle rises from the floor under every peak of the zig-zag and hangs from the ceiling
    over every dip, as big as the reserved space allows. Neighbouring triangles merge into a W.
  - where the zig-zag is too small for a triangle, a block slab sits under or over it with a few
    spikes on its face;
  - an occasional saw in the open space beyond a turn.

  Nothing else is placed in wave parts: no corridor filling, no extra spikes or blocks. Wave runs
  at 2x speed or faster, through the middle of its bounds, for strokes long enough for the
  triangles.
- ship corridors: top and bottom walls following the path with 45° and 2-wide slopes
- spikes in groups of 1-3 with gaps, never long identical rows. Spikes only go on faces that
  point at the path (never on the far side of a wall), and only one side spike per wall, at the
  height the player passes. A spike never stands on another spike.

## What is learned from real levels (gen/profile, done)
- per gamemode: how often each click action is used (jump, each orb colour, tap, hold), pads and
  gravity portals between clicks;
- how many clicks a gamemode stretch lasts, which mode follows which, time per gamemode;
- structure density (hazards, saws, solids per second).
Counted on a run the bot finds through each level, so only the layout is read: decoration, colours
and triggers are ignored. The planner, the segments and the structures use these as preferences.

## Planned next (roadmap)
- structure pieces cut out of real levels around the player's path, as a library the framing
  step draws from (so the shapes themselves come from the levels you like);
- a sequence model over click actions once enough levels are collected.

## Status and numbers (development machine, demo and synthetic songs)

- Fuzz (80 levels, random seeds, difficulties and styles): 0 failed, 99.4% of the clicks used,
  slowest variant 4.4 s (target: under 10 s).
- Every gamemode on its own: all beatable with the designed clicks, 95%+ of the clicks used.
- Clicks made necessary (skipping kills): 83-88% on mixed levels; the rest are mostly ship and
  wave taps that only nudge the path, and ground jumps where no hazard fits without touching a
  fair early/late press.
- Timing windows are measured on the finished level; orbs the player only grazes are avoided
  below the difficulty's target window.
- Re-rolling one segment keeps the level before it identical (tested).

## Not verified yet
- How GD draws each outline piece at rotation 0 (Settings → Place piece test checks it in a
  minute). The physics of the pieces is the same as solid blocks in the simulator either way.
- The simulator against the real game (as for v1: `alcli validate` on recorded runs).
