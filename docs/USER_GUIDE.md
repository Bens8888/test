# AutoLayout — User guide

AutoLayout designs a complete, playable layout for a song inside the GD editor. It picks the
structure, gamemodes, speeds, difficulty curve, obstacles, orbs, colours and triggers. A physics
bot plays every layout before you see it. Every option defaults to **Auto**: the generator
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
5. Pick a variant (best first), then press **Apply to level**. The layout goes onto a **new editor
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
| **Generate** | Song (the level's song or any file), click source, faithfulness, difficulty (Easy to Extreme Demon, or Auto), seed (same seed = same layout), number of variants, Generate / Cancel. Results view: variants, timeline and preview, **Re-roll section** / **Re-roll all**, Apply, Undo, Ghost. |
| **Style** | Style preset (Auto, Flow, Tight, Chaotic, Minimal, Retro, Modern, Memory, Decor-heavy), density, block theme, spike types, decoration level, triggers (camera, beat pulse/move, colours per section, shake on drops), trained model on/off. |
| **Modes** | Weight per gamemode (*never* … 100%, *All auto*), speed changes (off / subtle / aggressive), start speed, portal frequency, gravity flips, mini/big, dual, hold vs tap. |
| **Sections** | Per song section (intro, verse, chorus, build-up, drop, break, outro): difficulty −3…+3, and an optional forced gamemode. |
| **Playtest** | **Bot-verify the editor level** runs the bot on the level as it is now, including your own edits. Ghost on/off and **Play ghost** show the bot's path in the editor. Also shows the playability report: risky spots (tightest timings), the longest gap without input, notes, and the difficulty curve against the music's intensity. |
| **Training** | Shows the loaded models and lets you reload them. Collects your own levels into the training dataset (see below). |
| **Settings** | Backup before applying, retries per section, presets (save / load), reset all options, open the save folder. |

## How generation works (short)

The song is analysed into beats, sections, drums, notes and energy. Clicks come from the strongest
musical events, or from your recording. Each section gets a gamemode, a speed and a difficulty
that follows the music. Then every obstacle is placed around the simulated path of the player,
and it is kept only if all of the following hold:

- the player survives;
- the click has a fair timing window for the difficulty;
- skipping the click kills.

At the end the bot plays the whole level with exactly the clicks it was designed for. The report
says honestly when a click had to be left out.

## Teaching it your style (training with real levels)

AutoLayout ships with models trained by self-play. To make it learn from real, human-made levels
you need level files plus their songs:

- **How many:** 60 levels. 30 is the minimum and 100 is better. Spread them roughly like this:
  10 Easy/Normal, 15 Hard/Harder, 15 Insane and 20 demons (Easy to Extreme). Pick levels whose
  gameplay you like, and prefer classic levels over platformer or 2-player ones (those are skipped
  automatically).
- **Exporting a level:**
  - *In the mod:* open the level in the editor (for online levels: copy/save it to your local
    levels first). In **Training**, choose its difficulty and, optionally, a style, then press
    **Add this level**. The level `.gmd`, a copy of its song and a small label file are saved to
    `<save folder>/dataset/levels/<difficulty>/`.
  - *With GDShare:* export the level as `.gmd` and put it in a folder named after its difficulty
    (`easy`, `normal`, `hard`, `harder`, `insane`, `easy_demon` … `extreme_demon`). Next to it,
    put its song file with the **same name** (`MyLevel.gmd` + `MyLevel.mp3`). Newgrounds songs are
    in GD's song folder as `<song id>.mp3`.
- **Training** happens on a computer with Python 3 and PyTorch; see `training/RETRAINING.md`. Then
  copy the new `.alm` files into `<save folder>/models` and press **Reload models**.

## Files

Everything below lives in the save folder (**Settings → Save folder**):

- `backups/`: the level string before each Apply;
- `recordings/`: your recordings;
- `presets/`: saved option presets;
- `dataset/levels/`: levels collected for training;
- `models/`: your retrained models (these override the shipped ones).
