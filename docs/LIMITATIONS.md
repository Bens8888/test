# Known limitations

Stated plainly, with what each one means in practice.

## Not verified inside the game yet
- **Built and unit-tested, not play-tested.** The mod compiles on CI for Windows and macOS against
  Geode 5.11.2 / GD 2.2081, and the core passes ~50 test groups on every push. Nobody has played
  it inside GD during development yet: UI layout, menu feel and in-game edge cases need a first
  round of real use.
- **Simulator vs the real game.** The physics is a re-implementation from published reverse
  engineering (see DECISIONS D3). It reproduces the documented numbers (jump heights, airtimes,
  caps, orb and pad strengths). It has not been compared against runs recorded in the actual game.
  `alcli validate <level.gmd> <recording.alrec>` does exactly that comparison (a recording made with
  the mod stores the player's position at every click). **Please run it on a few levels and
  report the error.** The target tolerance is under 1 unit of position per input event over a
  whole level; anything above 2 units points at a physics difference. Known approximations:
  - slopes;
  - the spider's teleport distance in unusual geometry;
  - collisions at extreme speed with very small blocks.
- **Editor coordinates.** The ghost overlay and recorded positions assume GD's object layer is 90
  units above level-string coordinates. If the ghost path appears shifted, that constant
  (`kEditorYOffset`) is the one to change.

## Generator
- **Vocabulary.** Layouts use classic gameplay objects:
  - blocks, slabs, the standard spikes and saws;
  - yellow and pink orbs, yellow pads;
  - mode, speed, size, gravity and dual portals.

  They do not use dash orbs, teleport portals, rotate/move gameplay triggers, keys or collision
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
