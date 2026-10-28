# AutoLayout — User guide

AutoLayout designs a complete, playable layout for a song inside the GD editor. It picks the
structure, gamemodes, speeds, difficulty curve, obstacles, orbs, pads and portals. A physics
bot plays every layout before you see it.

It makes **layouts only**: plain default blocks and spikes, no decoration, no colour, camera or
pulse triggers, no background objects. The only trigger it adds is the end trigger that marks
where the level ends. Your level's colours, background and existing objects are left as they are. Every option defaults to **Auto**: the generator
decides, and your settings only steer it.

## Quick start (automatic mode)

1. Create a level and choose its song in the level settings. For a Newgrounds or library song,
   **download it** there first.
2. Open the editor and press the **green music-note button** in the top-right menu, or press
   `Ctrl+Alt+L` (`Cmd+Alt+L` on macOS).
3. In **Generate**, choose a difficulty or leave *Auto* and press **Generate**. The first run
   analyses the song (a few seconds) and then builds the variants (default 1; up to 8). The
   progress bar shows each stage, and **Cancel** stops the run.
4. The results view shows:
   - a timeline of the whole song: waveform, coloured sections, and every click coloured by its
     musical role (red downbeat, orange kick, yellow snare, cyan hi-hat, green melody, blue
     sustain, purple syncopation, pink = your recording, grey = a click that couldn't be built
     fairly);
   - a detail view of the level with the bot's path. Drag **scroll** to move through the song.
5. Optional: press **Watch it build**. The menu closes and the camera follows a player icon along
   the designed path while the layout appears around it (the orbs, pads and portals just ahead,
   then the structures). It is only a preview; nothing is in the level yet.
6. Pick a variant (best first), then press **Apply to level**. The layout goes onto a **new editor
   layer** as **one undo step**, and the level is backed up first. **Undo** removes the last
   AutoLayout layout again and restores the start settings.

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
4. In **Generate**, set **Clicks come from** to *My recording* and press **Generate**. You can
   also load an older recording, a GDR macro (`.gdr`, JSON or MessagePack) or a plain frame list
   with **Load…**.
5. **Faithful to my clicks** controls how strictly your clicks are followed:
   - **strict** keeps every click exactly where you made it;
   - lower values snap clicks to the music, thin out clicks that are too close for the gamemode,
     and fill long silent stretches with clicks from the music.

AutoLayout also measures your latency (how late you click on average) and removes it. It finds
your taps, holds, doubles, streams and repeating motifs, and gives the same motif the same
pattern each time it comes back.

## Tabs

| Tab | What it does |
|---|---|
| **Generate** | Song (the level's song or any file), click source, faithfulness, difficulty (Easy to Extreme Demon, or Auto), seed (same seed = same layout), number of variants, Generate / Cancel. Results view: variants, timeline and preview, **Re-roll section** (only that gamemode stretch and what follows changes) / **Re-roll all**, Apply, Undo, Ghost, Watch it build. |
| **Style** | Style preset (Auto, Flow, Tight, Chaotic, Minimal, Retro, Modern, Memory), density (how many structures frame the path), **Builder** (path first = v2, the default; classic = v1), **Gamemode switches** (AI decides / few / normal / many), **Look** (outline silhouettes like most layouts, or default blocks), trained model on/off (classic builder only). |
| **Modes** | Weight per gamemode (*never* … 100%, *All auto*), speed changes (off / subtle / aggressive), start speed, portal frequency, gravity flips, mini/big, dual, hold vs tap. |
| **Sections** | Per song section (intro, verse, chorus, build-up, drop, break, outro): difficulty −3…+3, and an optional forced gamemode. |
| **Playtest** | **Bot-verify the editor level** runs the bot on the level as it is now, including your own edits. Ghost on/off and **Play ghost** show the bot's path in the editor. Also shows the playability report: risky spots (tightest timings), the longest gap without input, notes, and the difficulty curve against the music's intensity. |
| **Training** | **Learn this level** / **Learn from all levels**: the bot plays your levels and AutoLayout learns your style from them (below). Use what was learned on/off, Forget. Also the classic builder's models and the dataset folder. |
| **Settings** | Backup before applying, retries per section (classic builder), presets (save / load), reset all options, open the save folder, **Place piece test** (see below). |

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

## Checking the outline pieces (please do this once)

The outline look builds structures from GD's outline pieces (edges, corners, pillar caps, slopes).
Which way GD draws each piece by default could not be verified outside the game. In the editor,
**Settings → Place piece test** places every piece at every rotation, labelled, plus a sample
structure, after your level's last object. If the sample structure is not one clean silhouette,
send a screenshot: the fix is one table in the code. **Undo** removes the test again.

## Learning your style

**Training → Learn this level** (with the level open in the editor) saves it to the dataset and
learns from it right away; **Learn from all levels** relearns from the whole dataset folder. The
bot plays each level (a few seconds to a minute each, in the background), and AutoLayout counts:
which orbs, pads, jumps, taps and holds each gamemode uses, how long gamemode stretches last,
which modes follow which, and how dense the structures are. Generation then follows those numbers.
Only the layout is read; decoration, colours and triggers are ignored, so decorated levels are
fine. Levels the bot cannot beat, platformer and 2-player levels are skipped with the reason.

## Which levels to collect

For **Learn my style** (above) and for retraining the classic builder's model:

- **How many:** 60 levels. 30 is the minimum and 100 is better. Spread them roughly like this:
  10 Easy/Normal, 15 Hard/Harder, 15 Insane and 20 demons (Easy to Extreme). Pick levels whose
  gameplay you like, and prefer classic levels over platformer or 2-player ones (those are skipped
  automatically).
- **Only the layout is learned.** Decorated levels are fine: decoration, colours, background
  objects and triggers are ignored. Training reads the gameplay objects the player can touch
  (solid blocks, spikes, saws, orbs, pads, portals) and the path the bot takes through them.
- **Exporting a level:**
  - *In the mod:* open the level in the editor (for online levels: copy/save it to your local
    levels first). In **Training**, choose its difficulty and, optionally, a style, then press
    **Add this level**. The level `.gmd`, a copy of its song and a small label file are saved to
    `<save folder>/dataset/levels/<difficulty>/`.
  - *With GDShare:* export the level as `.gmd` and put it in a folder named after its difficulty
    (`easy`, `normal`, `hard`, `harder`, `insane`, `easy_demon` … `extreme_demon`). Next to it,
    put its song file with the **same name** (`MyLevel.gmd` + `MyLevel.mp3`). Newgrounds songs are
    in GD's song folder as `<song id>.mp3`.
- **Learning for the default builder** happens in the mod (Learn this level / Learn from all
  levels); no song file is needed for it.
- **Retraining the classic builder's model** happens on a computer with Python 3 and PyTorch; see
  `training/RETRAINING.md`. Then copy the new `.alm` files into `<save folder>/models` and press
  **Reload models**.

## Files

Everything below lives in the save folder (**Settings → Save folder**):

- `backups/`: the level string before each Apply;
- `recordings/`: your recordings;
- `presets/`: saved option presets;
- `dataset/levels/`: levels collected for training;
- `style_profile.json`: what was learned from your levels (Forget deletes it);
- `models/`: your retrained models (these override the shipped ones).
