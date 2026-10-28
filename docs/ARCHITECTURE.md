# AutoLayout — Architecture

AutoLayout is a portable C++20 core with no dependency on Geometry Dash or Geode, plus a thin
Geode mod that drives it inside the game. Everything that must be correct (physics, the bot, the
generator's checks) lives in the core, where it is compiled and unit-tested on every push,
including headless Linux CI. Training code is Python/PyTorch in `training/`.

```
┌──────────────────────────────── Geode mod (src/) ───────────────────────────────────────────┐
│ hooks/   EditorUI button + keybind → AutoLayoutPopup · recorder hooks (handleButton,         │
│          ringJump, checkpoints, respawns, deaths, completion, editor playtests, pause menu) │
│ ui/      AutoLayoutPopup (7 tabs) · Timeline (waveform/sections/clicks + level preview) ·    │
│          Ghost (bot path + animated icon in the editor) · Widgets                            │
│ session/ options (saved) + presets · song resolution · analysis cache · background jobs    │
│          (std::thread, progress + cancel, polled by the popup) · models                     │
│ bridge/  editor I/O: objects as one undo step on a new editor layer, backups, start         │
│          settings (restored by undo), dataset export                                         │
│ recorder/ LiveRecorder: practice stitching, .alrec + .json files                            │
└───────────────┬──────────────────────────────────────────────────────────────────────────────┘
                │ plain C++ structs (no cocos types cross into the core)
┌───────────────▼────────────────── core/ (libautolayout_core) ────────────────────────────────┐
│ audio/   decode (wav/mp3/ogg) → resample → own FFT → STFT → band flux, HPSS, kick/snare/hat, │
│          tempo (ACF + DP beat tracker + metronomic grid fit), downbeats (Viterbi), pitch    │
│          (harmonic summation + Viterbi), energy, sections (novelty + power) → typed events  │
│          synth: deterministic test signals, the demo song, random songs for self-play       │
│ rec/     recordings (.alrec binary v1 + JSON), GDR / frame-list import, practice stitcher, │
│          demo recording                                                                      │
│ physics/ deterministic 240 Hz GD 2.2 player physics (8 modes, speeds, mini, gravity, dual,  │
│          orbs, pads, portals, slopes, hitboxes)                                              │
│ bot/     run · isBeatable · firstFailure · autoSolve (beam search) · timing windows          │
│ gen/     plan (parts, gamemodes, speeds, clicks) · fusion (recordings × music) · pattern    │
│          library + style models · Builder (verified placement) · ground/flying/surface      │
│          builders · generator (variants, report, feel score, re-roll)                       │
│ model/   GRU pattern model inference (.alm files), model sets per style                     │
│ data/    training sequences: self-play (reward) and real levels (bot + labeller), JSONL     │
│ level/   object database · level string / .gmd read & write                                 │
│ report/  PNG renderer for previews and the gallery                                           │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
   tools/alcli   analyze · generate · demo-song · demo-recording · rec-info · bench
   tools/aldata  vocab · selfplay · extract · extract-dir
┌───────────────────────────────── training/ (Python) ─────────────────────────────────────────┐
│ autolayout/audio.py         numpy mirror of the C++ analysis (bit-exact parity test)        │
│ autolayout/pattern_model.py the GRU model, .alm export/import, parity vectors              │
│ train_patterns.py           training + per-style fine-tunes + report                         │
│ selfplay_loop.py            self-play rounds with held-out evaluation                        │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

## Generation pipeline

1. **Analyse** the song (background thread, cached per file): beats, bars, downbeats, tempo
   segments, onsets per band, kicks/snares/hi-hats, sustained notes, melody pitch, energy and
   labelled sections (intro, verse, chorus, build-up, drop, break, outro).
2. **Clicks**: from the music (salience of every musical event, density from difficulty, intensity,
   intent and style; no dead stretches) or from a recording (fusion: roles, latency, figures,
   motifs; the faithfulness slider decides how strictly they are kept).
3. **Plan**: sections become gameplay parts with an intent (intro, groove, build, climax, breather,
   outro), a gamemode (affinity per intent × user weights × how well the mode fits the clicks), speed,
   size, gravity and dual, and a difficulty that follows the energy.
4. **Build** part by part with an incremental physics world and an input track:
   - ground modes (cube, robot): for every click a pattern is chosen (style model) and placed around
     the simulated jump; it is kept only if the player survives, the timing window is at least the
     difficulty's minimum, and skipping the click kills (necessity);
   - flying modes (ship, ufo, wave, swing): the path is simulated from the clicks (plus band-keeping
     toggles), then a tube is carved around the path **and every timing variant of it** (each input
     shifted early/late by half the required window), so windows hold by construction; saws mark
     the no-click path of clicks that would otherwise be skippable;
   - ball and spider: a corridor sized for the click rhythm, hazards wherever no timing variant passes.
   Every part is verified from its start (and a little past its end); failing parts are retried with
   new random streams and finally rebuilt as a plain, safe stretch (its clicks are reported).
5. **Transitions**: portals where the player will be, moved further if passing them would kill.
6. **Verify**: the bot plays the finished layout with the designed inputs. If that ever fails, the
   part where the run dies is rebuilt as a plain, safe stretch (and the parts after it on top),
   then a bot search; an unbeatable result is reported with the exact time, position and cause.
   The output is a layout only: plain blocks and spikes, no decoration or visual triggers.
7. **Report**: clicks (forced / left out), timing windows, risky spots, longest input gap,
   difficulty curve vs music intensity, music alignment; variants ranked by a feel score.

## Where ML sits (see DECISIONS D8-D10)
The style model decides *which valid pattern* fits the moment; it never decides anything that must
be correct. The shipped GRU models were trained by self-play rejection sampling with the bot as the
judge and can be retrained with real levels.
