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
4. **Look.** Structures are kept as solid cells and slopes, then tiled with the outline set (white
   silhouettes, the usual layout look) or with default blocks.
5. **Verify.** The bot plays the designed inputs, timing windows are measured, failures are
   repaired (as in v1).

## Actions

| Mode | At a click | Between clicks (no click) |
|---|---|---|
| cube | jump from a platform; orb (yellow, pink, red, blue, green, black, green/pink dash) | land on a platform, pad (yellow, pink, red, blue), gravity portal, mode/speed/size portal in mid-air |
| robot | jump (hold length = the click's hold), orb | as cube |
| ship, ufo | tap or hold | gravity portal, mode portal; ufo: orbs |
| wave, swing | tap or hold (direction change) | gravity portal, mode portal |
| ball | flip from a surface; blue/green orb | surface to land on, blue pad, gravity portal |
| spider | teleport flip; spider orb | surface, spider pad |

## Segments and switches
Gamemode, speed and size change in mid-air at musical boundaries. The switch rate comes from the
style (modern layouts change every 1-4 clicks, classic ones every section) and the music (drops
switch on the drop and speed up). The mode is chosen for the click rhythm: fast streams suit
wave/ship/swing, spaced clicks cube/robot/ball/ufo, long notes holds.

## Structures (starter pool, before learning from real levels)
- hanging pillar (from the top), 1-3 cells wide, spike tip(s), optional 45° slope corner
- rising pillar (from the floor), spikes on top, optional step and slope corner
- platform the player runs on, extended down to the floor
- spike rows and single spikes on structures, floating saws in pockets next to the path
- wave/ship corridors: top and bottom boundaries following the path with 45° slopes
- ball/spider: surfaces with slope corners, diamond blocks between the lanes

## What is learned from real levels
- per click: which action is used for which mode, rhythm and music (a style profile and, with
  enough data, the sequence model);
- segment lengths and mode transitions;
- structure statistics (pillar sizes, spike/saw use, density) and structure pieces cut out
  around the player's path.
Only the layout is read: decoration, colours and triggers are ignored.
