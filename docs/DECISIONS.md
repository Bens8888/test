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
