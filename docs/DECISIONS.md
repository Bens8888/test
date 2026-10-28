# Design decisions

Each entry: decision, why, and what would change it.

## D1. Portable core + thin Geode mod
All logic (audio, physics, bot, generator, ML inference, level format) lives in `core/`, which has
no Geode or cocos dependency. The mod in `src/` only does UI, hooks and editor I/O.
*Why:* the physics/bot/generator must be unit-tested and fuzzed headlessly (this machine has no
Geometry Dash), training scripts must use the exact same code, and the core then builds identically
on Windows, macOS (arm64 + x86_64) and Linux CI.

## D2. Versions: Geode 5.11.2, GD 2.2081, C++23 for the mod
Geode v5 requires C++23 (clang ≥ 19 / MSVC ≥ 19.44); the core is written in C++20 and compiles in
either mode. 2.2081 is what Geode 5.11.x primarily supports (5.11.2 also runs on 2.209 in
forward-compat mode, but bindings and physics are verified against 2.2081 only).

## D3. Physics re-implemented from MIT-licensed reconstructions
The simulator is written from scratch following StarryDawn72/iliasHDZ `open-source-gd-project`
(MIT, hand-verified 2.2081 reconstructions) and camila314/gdp decompilations; camila314's gd-sim is
not licensed for redistribution, so it is used only as a numeric cross-check. Spec with sources:
`docs/research/physics_spec.md`.

## D4. Audio: own FFT, minimp3 + stb_vorbis decoders
FFT, STFT, HPSS, pitch, tempo, beat and section analysis are our own code. Decoders are the
public-domain/CC0 single-file minimp3 and stb_vorbis (GD songs are .mp3 and .ogg) and miniz for
gzip. No other dependency.

## D5. Beat tracking: DP tracker + metronomic grid fit
Ellis-style dynamic-programming tracker, then (per tempo segment) a robust linear grid fit when the
beats are metronomic (median residual < 12 ms) — nearly all GD music is produced on a fixed grid,
and the fit removes local glitches (snare rolls, breaks). Off-beat lock is corrected using kick/snare
evidence; downbeats come from a Viterbi pass over bar position. Measured on synthetic tests: median
beat error < 4 ms, onset error < 3 ms.

## D6. Drops are found by "power" (loudness + kick rate), not raw intensity
Snare-roll build-ups are dense and loud; what distinguishes a drop is the low end (kick on every
beat) and loudness. Kick detection uses absolute percussive bass energy from HPSS.

## D7. Determinism
PCG32 streams seeded from (seed, purpose, index); no `std::` distributions; `-ffp-contract=off`
and `/fp:precise` so floating-point physics matches across compilers.

## D8. Model inference: a hand-written C++ engine, not ONNX Runtime
The pattern model is one GRU layer (hidden 96) with a token embedding and a linear head, about
66k parameters (260 KB per model file). Running it is ~150 lines of C++ (`core/src/model/model.cpp`):
no 10+ MB runtime to bundle and load on two platforms, no allocator or threading surprises inside
GD, thread-safe by construction (the generator builds variants in parallel), and deterministic. The
file format (`.alm`: header, float32 tensors, CRC-32) is written by `training/autolayout/pattern_model.py`.
Parity is tested on every build: `tests/test_model.cpp` replays PyTorch reference vectors
(`tests/data/model_fixture*.{alm,json}`); measured max |logit difference| 1.2e-6.
Risk: a bigger architecture (attention, several layers) would need the engine extended; ONNX
Runtime would become worth it above a few MB of weights.

## D9. What the model does — and what it never does
ML decides pattern choice, style and "feel": given the music at a click (role, strength, gaps,
melody movement, intensity, intent), the gamemode state, difficulty, style, density and the last
16 decisions, it scores the patterns. It only ever scores candidates the rules already accept for
the situation (constrained generation: the builder masks out anything that would break the path),
and the builder still simulates every placement: survival, timing window, necessity. If the model
picks something that does not work, the next candidate is tried; the bot verifies the level at the
end. Correctness never depends on the model. Temperature depends on style (chaotic 1.5, minimal 0.7,
tight 0.8) and on intensity (calm moments sample closer to the model's favourite).
Unproven tricks (reinforcement learning on raw object placement, diffusion over level images) are
deliberately not used: they cannot guarantee beatable output and would need far more data.

## D10. Training data: self-play with the bot as the reward, real levels through a labeller
- Self-play (`aldata selfplay`): random synthetic songs (tempo 92-176 BPM, key, motifs, drum feel,
  structure), random style/difficulty/density, K variants each, all bot-verified; the best third
  by reward (music alignment, forced clicks, dropped clicks, difficulty following the music,
  variety, fairness) is kept — rejection sampling. Rounds repeat with the newly trained model
  generating (`training/selfplay_loop.py`).
- Real levels (`aldata extract-dir`): .gmd + song; filtered (platformer, 2-player, too short,
  unsolvable by the bot, wrong song); the bot solves the level and every jump's geometry is
  classified back into the closest pattern token. Measured on generated levels where the truth is
  known: 80% exact / 95% terrain agreement on regular jumps, 84% on orb jumps (pit widths are
  ambiguous by one cell). Flying/ball/spider sections are labelled once per section (family).
- Augmentation: tempo shift (click gaps scaled 0.85-1.18), intensity jitter, random crops of the
  decision sequence. Transposition and vertical mirroring are no-ops for this representation (pitch
  enters only as relative movement, every pattern is gravity-relative), so they are not applied.
- Limitation, stated plainly: a model trained only on self-play learns to imitate the rule-based
  generator's best outputs; it does not know human taste. Style learning from human levels needs
  the user's .gmd files (see training/RETRAINING.md for how many and how to export them).
