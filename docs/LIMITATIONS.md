# Known limitations

Stated plainly, with what each one means in practice.

## Not verified inside the game yet
- **Tested by the self test in GD, not yet by a person.** The mod builds against Geode 5.10.1 / GD 2.2081
  and loads on Geode 5.11.2. The self test presses the editor button, opens every tab, generates,
  applies, undoes, opens Tap Along, and checks the recorder (every click caught at the right tick).
  It cannot judge how levels feel to play. macOS builds on CI but has not been run.
- **Structures are lighter than top levels.** 96-109 hitbox objects and 20-29 hazards per screen,
  next to Bidal Wave (80 and 20) and xo (84 and 32). Bloodbath has 150, Tidal Wave 194 and Grief 284,
  with 53-118 hazards, and saws are about 1 per screen against Tidal Wave's 31
  (`training/corpus/compare.py`).
- **Simulator vs the real game.** Checked against the real game with the in-game self test (a
  `selftest.txt` in the mod's save folder plays a generated level with its designed clicks and logs
  every physics step) and `simtrace`. Horizontal error is about 0.001 units over whole levels. Fixed
  from those runs: blue pads facing away, pad sizes, spider area height and teleports (a hazard on the
  way kills), robot boost after pads and orbs, dash through portals, portal bounds on a row boundary,
  flying modes leaving a bound, swing portal width, saw hits and the small saw's radius, flipped cubes
  touching the ground, the landing of a step counting before a mode portal, a swing's press on a floor,
  where flying modes start. Generated levels keep 3 units of extra distance from hazards to absorb what is
  left, mostly one-tick pad timing.
  - On the last round, 8 of 8 new levels reached 100% with their designed clicks in GD 2.2081, and the
    round before 6 of 8 (both deaths fixed since). Seven of the last 8 traces stay within 2 units of
    the simulator to the end of the level; the other drifted 2 units on a ship stretch and the run
    still finished. These are bot runs that press on the exact tick. "Beatable" means beaten in the
    simulator, so play a level before trusting it.
  - Still not exact: riding along slopes, rotated portals (the generator avoids both), some spider
    teleports, and a wave leaving into a swing while it rides a bound (one step of gravity, about 2
    units until the next contact).
- **Editor coordinates.** The ghost overlay and recorded positions assume GD's object layer is 90
  units above level-string coordinates. If the ghost path appears shifted, that constant
  (`kEditorYOffset`) is the one to change.

## Generator (path-first builder, the default)
- **Outline pieces unverified in GD.** The outline look (Style → Look) relies on how GD draws its
  outline pieces by default; Settings → Place piece test shows it in a minute (see the user
  guide). The default block look does not depend on it.
- **Not every click is strictly necessary.** Skipping a click often kills, but ship and wave taps
  often only nudge the path, and some jumps have no spot for a hazard that would not also touch a
  fair early or late press. Timing windows are always kept.
- **The library has no player paths.** Modules come from level geometry, so their distance from
  the path is decided by the corridor, not learned.
- **Platforms after long gaps** can still be wide boxes (the player has to run somewhere until the
  next click).
- **Recording mode** keeps your clicks and hold lengths; the gamemodes are chosen from your click
  rhythm, not from the modes you played in.

## Generator (both builders)
- **Vocabulary.** Layouts use gameplay objects only:
  - blocks/outline pieces, 45-degree and 2-wide slopes, the standard spikes and saws;
  - every orb colour including dash orbs (path-first builder), yellow/pink/red/blue pads;
  - mode, speed, size, gravity and dual portals.

  They do not use teleport portals, rotate/move gameplay triggers, keys or collision
  blocks, and there is no platformer support.
- **Clicks that can't be built fairly are left out** and reported. That is typically 2-5% of
  musical clicks, more for very dense songs in cube mode, where orb chains are capped so the
  player never climbs off the screen.
- **Flying modes** (ship, swing) often have "flow" clicks: skipping one only shifts the path
  slightly, so not every click is strictly forced. Timing windows are guaranteed; necessity is not
  always.
- **Layout only, by design.** The output is plain default blocks and spikes with no decoration,
  colours or visual triggers; decorating is left to you.
- **Gameplay vs the song after edits.** The layout is timed to the song from x = 0 with the song
  offset of the level. If you insert gameplay or speed portals before it, the timing shifts.

## Music analysis
- Built for produced music on a steady grid (almost all GD songs). Rubato, live tempo drift,
  or long beatless ambient parts give weaker beats, sections and drop detection.
- The Python mirror reads WAV only; convert MP3/OGG with any tool for training-side analysis. The
  mod and the C++ tools read MP3, OGG and WAV.

## Machine learning
- **The shipped models learned from self-play only**, imitating the best third of the rule-based
  generator's own levels judged by the bot. On held-out songs that measurably improves the reward
  (+5-9% over the rules), but the models do not know human taste yet. Style learning needs real levels: see
  training/RETRAINING.md (60 levels recommended).
- More self-play rounds (the model generating its own data) did not improve it further with the
  current reward; the reward function itself is hand-designed.
- The model chooses patterns. It does not invent new obstacle shapes: everything it places
  comes from the pattern library and is verified by the simulator.

## Recording
- Native *click between steps* is turned off while recording, so every click lands on a whole
  physics step (1/240 s). That is deterministic and well inside human timing, but slightly
  coarser than GD's default input handling.
- Two-player levels are not supported for generation; recordings of the second player are stored
  but not used.
