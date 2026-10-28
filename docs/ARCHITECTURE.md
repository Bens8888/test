# AutoLayout — Architecture

AutoLayout is split into a portable core that has no dependency on Geometry Dash or Geode, and a
thin Geode mod that drives it from inside the game. Everything that must be correct (physics, the
bot, the generator's constraint checks) lives in the core, where it is compiled and unit-tested on
every platform, including headless Linux CI.

```
                ┌──────────────────────────── Geode mod (src/) ─────────────────────────────┐
                │ EditorUI button → AutoLayoutPopup (7 tabs) · PauseLayer record button      │
                │ Recorder hooks (GJBaseGameLayer::handleButton / update) · Ghost playback    │
                │ EditorBridge: read level/song → core · write objects as one undo group      │
                │ JobRunner: background std::thread, progress + cancel, results via           │
                │            geode::queueInMainThread                                         │
                └───────────────┬────────────────────────────────────────────────────────────┘
                                │ plain C++ structs (no cocos types)
┌───────────────────────────────▼──────────── core/ (libautolayout) ─────────────────────────┐
│ audio/   decode (mp3/ogg/wav) → resample → own radix-2 FFT → STFT → features:             │
│          spectral flux per band, HPSS (median filter), kick/snare/hat, tempo (autocorr +    │
│          DP beat tracker, tempo-change segmentation), downbeats, YIN pitch on the harmonic  │
│          part, RMS/loudness energy, novelty-based sections → MusicTimeline (typed events)   │
│ record/  ClickRecording (v1 binary .alrec + JSON), practice stitching, GDR / frame import   │
│ fusion/  click roles, pattern + motif detection, trajectory extraction, DesignPlan          │
│ physics/ deterministic 240 Hz re-implementation of GD 2.2 player physics (8 gamemodes,      │
│          speeds, mini, gravity, dual, orbs, pads, portals, slopes, hitboxes)               │
│ bot/     isBeatable · firstFailure · autoSolve (state-snapshot DFS/beam search) ·          │
│          timing-window analysis (fairness)                                                  │
│ gen/     planner (sections → gamemode/speed/size/gravity plan) · click-track synthesis ·    │
│          path-first builder per gamemode with a tagged pattern library · motif memory ·    │
│          clean-block pass (edges/corners, theme) · triggers · verify → repair → regenerate │
│ ml/      tiny inference engine (GRU + MLP, int8/fp32 weights), constrained sampler (masks) │
│ level/   object database (IDs, hitboxes, kinds) · level string read/write · .gmd export    │
│ report/  playability report (risky clicks, longest no-input gap, difficulty curve)         │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
          ▲                                   ▲
          │ C API (libautolayout_c, ctypes)   │ alcli (CLI: analyze / generate / verify / bench / render)
┌─────────┴───────────── training/ (Python, PyTorch) ────────────────────────────────────────┐
│ dataset: .gmd → objects → bot auto-solve → click track + trajectory → time-slice tokens    │
│          aligned with audio features (Python mirror of the C++ analysis, parity-tested)     │
│ models:  per-style sequence model (GRU + attention head) predicting slice tokens            │
│ selfplay: sample layouts with the C++ generator+model, score with the bot, keep the best    │
│           (rejection-sampling fine-tune / expert iteration)                                 │
│ export:  weights → .alm files shipped in resources/models, C++/PyTorch parity tests         │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

## Generation pipeline

1. **Analyze** the song → `MusicTimeline`: beats/bars/downbeats with tempo segments, per-band
   onsets (bass/mid/high, kick/snare/hat), sustained notes (holds), pitch contour of the melody,
   energy curve, and labelled sections (intro, verse, build-up, drop, break, outro).
2. **Clicks**: either synthesised from the timeline (auto mode) or taken from a recording.
   Every click gets a musical role (kick, snare, melody note, syncopation, hold).
3. **Plan**: the design brain splits the song into gameplay sections and chooses gamemode,
   speed, size and gravity for each, with transitions on downbeats and breathing room
   before/after; difficulty and density follow the energy curve and the user's per-section
   overrides.
4. **Build (path-first)**: for each section, the builder simulates the player through the click
   track and places geometry *around that path*: floors/platforms where the player lands, hazards
   that make each click necessary, corridors around flying paths, orbs/pads where the music calls
   for them. Patterns are chosen by the style model (or by rule weights when no model is loaded)
   from a tagged pattern library; motif memory repeats patterns on repeated music.
5. **Verify**: the bot replays the intended inputs, then measures each click's timing window by
   perturbation. Any section that dies or is tighter than the difficulty allows is repaired
   (adjust obstacle) or regenerated with a new sub-seed. A section that still fails after the
   retry budget is replaced by the simplest safe pattern and reported.
6. **Decorate**: clean block pass (edge and corner pieces, consistent theme, grid alignment,
   z-order), colours per section, beat-synced pulses, camera triggers, drop shake.
7. **Apply**: the editor bridge backs up the level string, writes the objects on a fresh editor
   layer as a single undo group, and can export a `.gmd`.

Determinism: every random decision draws from a PCG32 stream seeded from
`hash(seed, section index, retry)`, so the same seed + inputs give the same layout, and
re-rolling one section does not disturb the others.

## Milestones

| # | Milestone | Done when |
|---|-----------|-----------|
| 1 | Mod skeleton, menu, test objects | CI builds the .geode for Windows and macOS; editor button opens the popup; test pattern placed and undone |
| 2 | Recorder | round-trip tests for .alrec/JSON/GDR; in-game hooks record presses with state |
| 3 | Physics + bot | unit tests against reference numbers for every mode; benchmark ≥ thousands of rollouts/s |
| 4 | Audio analysis | synthetic-audio tests within a few ms; < 20 s for a 3-minute song |
| 5 | Fusion | click roles/patterns/motifs on demo recording; design plan output |
| 6 | Rule-based generator | fuzz: zero unbeatable outputs over thousands of seeds |
| 7 | Dataset extraction | .gmd → tokens with bot-solved clicks; augmentation; filters |
| 8 | Training | measured time/hardware; reward curves |
| 9 | Models in mod | C++/PyTorch parity; constrained sampling |
| 10 | Full menu + polish | all options wired; preview timeline; ghost playback |
| 11 | Tests + docs | integration test, fuzz, benchmark, gallery, guides |
