# AutoLayout — User guide

AutoLayout designs a complete, playable layout for a song inside the GD editor. It picks the
structure, gamemodes, speeds, difficulty curve, obstacles, orbs, pads and portals. A physics
bot plays every layout before you see it.

It makes **layouts only**: no decoration, no colour, camera or pulse triggers, no background
objects. The only trigger it adds is the end trigger that marks where the level ends. Your level's
colours, background and existing objects are left as they are.

## Quick start

1. Create a level and choose its song in the level settings. For a Newgrounds or library song,
   **download it** there first.
2. Open the editor and press the **green music-note button** in the top-right menu, or press
   `Ctrl+Alt+L` (`Cmd+Alt+L` on macOS).
3. Choose a difficulty (or leave *Auto*) and press **Generate**. The first run analyses the song
   (a few seconds), then builds the layout. **Cancel** stops it.
4. Press **Place in level**. The layout goes onto a **new editor layer** as **one undo step**, and
   the level is backed up first. **Remove** takes it out again, **New layout** replaces it with a
   new one.

## The menu

It has one page:

| Row | What it does |
|---|---|
| **Song** | The level's song, or any audio file with **Change**. **Level song** goes back to the level's own. |
| **Clicks** | Where the clicks come from: *Follow the music*, *My tap-along* (with **Tap along**, below) or *A recording / macro* (with **Load**, below). |
| **Difficulty** | Auto (Harder to Insane, from the song's energy and tempo), or Easy to Extreme Demon. Sets how often you click, the timing windows, the wave corridors and how often the gamemode changes. |
| **Speed** | Auto, Slow, Normal or Fast: the speed the portals keep to. |
| **Structures** | Auto, Rails, Slopes, Compact or Spiky: the style of the structures, learned from real levels. |

Everything else (gamemodes, portals, orbs and pads, how often things change) is decided from the
song. Every **Generate** makes a new layout.

## Tap along (the easiest way to give your own clicks)

Works like GD's Guideline Creator:

1. Set **Clicks** to *My tap-along* and press **Tap along**. Set the **Start Offset** (seconds into the level) if you
   only want to tap from somewhere in the middle.
2. Press **Record**. The song plays. Tap along with a click, a touch, **Space** or **Up**. Hold
   for a hold.
3. Press **Stop**, or let the song end. Each tap shows up as a line in the editor (GD
   guidelines: orange for a tap, yellow for the end of a hold).
4. Record more takes to add parts or fix them. Takes are merged into one track, and re-tapping a
   spot replaces the old tap there. **Undo take** removes the last take, and **Clear** removes
   everything.
5. Press **Generate**. The level ends shortly after your
   last tap.

The taps are timed from the song itself, with no gameplay in between, so they don't drift. If the
level already has guidelines (for example from GD's own Guideline Creator), they become your
first take. The track is saved per level in `<save folder>/taps`.

Placing a layout sets the level's song offset to the one the layout was built for (the song's
start). Any other offset would put every click off by that much. **Remove** restores the old
offset.

## Click-recording mode (play along)

1. Arm the recorder in either of two ways:
   - in a level, open the pause menu and press **Rec**;
   - anywhere, press the hotkey `Ctrl+Alt+R` (`Cmd+Alt+R`).

   *REC* appears on screen; it is grey while waiting for the next attempt and red while
   recording.
2. Play along with the song and click where the layout should make you click. This works in an
   **editor playtest** of an empty level (the easiest way) or while playing any level. Practice
   mode works too: deaths and respawns are stitched out, and only the successful path is kept.
3. When the run ends (level complete, playtest stopped, or the level left), the recording is
   saved to `<save folder>/recordings` as `.alrec` plus `.json`.
4. Set **Clicks** to *A recording / macro* and press **Generate**. You can also load an older
   recording, a GDR macro (`.gdr`, JSON or MessagePack) or a plain frame list with **Load**.
   Every click is kept where you made it.

The level ends about two seconds after your last click. A recording of part of the song makes a
level of that part. If the first click comes late in the song (a take of the middle), the level
starts about 2.5 seconds before it, and its song offset is set to match. You don't get a long
empty run first. If a playtest started from a start position or the middle of the level, the
song's real position is measured, so the clicks still match the song.

AutoLayout also measures your latency (how late you click on average) and removes it. It finds
your taps, holds, doubles, streams and repeating motifs, and gives the same motif the same
pattern each time it comes back.

## How generation works (short)

The song is analysed into beats, sections, drums, notes and energy. Clicks come from the strongest
musical events, or from your recording. Then, like a layout maker working from a macro:

1. **The gameplay path first.** The clicks are cut into gamemode stretches (switching at musical
   boundaries, more often in drops). At every click AutoLayout decides what it does: a jump, an
   orb (yellow, pink, red, blue, green, black, dash), a tap or a hold, a ball or spider flip.
   Between clicks it decides what keeps you moving: a platform to land on, a pad, a gravity
   portal, the portals of the next stretch. A search tries many combinations with the physics
   simulator and keeps the best (fair, varied, following the music and the learned style).
2. **Structures around the path.** Every click is simulated pressed early and late within the
   difficulty's timing window; that space stays free. Where the run *without* the click goes, a
   hazard is placed, so every click matters. Then the path is framed: hanging pillars with spike
   tips over the jumps, rising pillars below, saws in the gaps, wave and ship corridors with
   45-degree and 2-wide slopes.
3. **Verified.** The bot plays the finished level with exactly the clicks it was designed for, and
   the timing window of every click is measured on the finished level. The report says honestly
   when a click had to be left out or is not strictly necessary.

## Files

Everything below lives in the mod's save folder:

- `backups/`: the level string before each layout is placed;
- `recordings/`: your recordings;
- `taps/`: your tap-along tracks (one per level).
