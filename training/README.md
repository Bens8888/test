# training/

Python side of AutoLayout. Code lives in the `autolayout` package (numpy only).

## Audio analysis mirror (`autolayout/audio.py`)

`autolayout.audio` is a port of `core/src/audio/analysis.cpp` (plus `fft.cpp` and the WAV/resample
parts of `decode.cpp`), so the training pipeline sees exactly the features the mod computes at
runtime.

```python
from autolayout import audio
A = audio.analyze_file("song.wav")          # or audio.analyze(samples, sample_rate)
A.onsetEnv, A.bandOnset[0], A.rmsDb, A.intensity   # fine curves (hop = 128 / 22050 s)
A.percRatio, A.pitchMidi, A.chroma                 # coarse curves (hop = 512 / 22050 s)
A.beats, A.downbeats, A.tempo, A.sections, A.events
```

`MusicAnalysis`, `Section`, `MusicEvent`, `TempoSegment` and `AnalysisOptions` use the same field
names as `analysis.hpp`. Curves are `float32` arrays; times are Python floats (C++ `double`).
`analyze_file` reads WAV only (PCM 8/16/24/32-bit, float 32/64, extensible, any channel count,
any rate; non-22050 Hz input goes through the same Kaiser-sinc resampler as the C++ code).
MP3/OGG raise an error: convert them to WAV first. Another decoder than the mod's minimp3 or
stb_vorbis can change the samples slightly, so for exact parity decode with the same library.

### How parity is achieved

The port mirrors the C++ arithmetic, not just the algorithm:

* `float` maps to `np.float32` and `double` to Python `float`/`float64`, with an explicit cast
  wherever C++ promotes. NumPy 2 keeps `float32 * python_float` in float32, which matches C++
  `float * 0.5f` but not `float * 0.5`.
* Sequential C++ sums (`acc += ...`) use `np.add.accumulate`, which is strictly sequential.
  `np.sum` is not used for these because it sums pairwise and rounds differently.
* The FFT is the same radix-2 transform with the same bit reversal, the same float32 twiddles
  (computed through glibc `cos`/`sin`) and the same real-input split. Complex products are
  written out as separate float32 multiplies and adds. NumPy's complex64 SIMD loops can use FMA,
  and the C++ core is built with `-ffp-contract=off`.
* `log1pf` is a vectorised port of glibc's `s_log1pf.c`. NumPy's float32 `log1p` (SVML on
  AVX-512) differs by 1 ulp on about 4% of inputs, and float64-then-round differs on about 2%.
  The port matched glibc on 2M random inputs. Scalar `log`/`log10`/`exp`/`pow`/`log2` go through
  `math`, which is glibc.
* Median filters and percentiles are pure selections (`np.partition`), so they involve no
  rounding. Ties in Viterbi, argmax and DP steps resolve to the first index, the same rule as
  `std::min_element`/`max_element` and strict `<`/`>` scans.

### Measured parity (`tests/test_audio_parity.py`)

Measured on x86-64 with glibc 2.39, numpy 2.5 and the core built RelWithDebInfo.

| signal | curves | beats / downbeats | events |
|---|---|---|---|
| demo song (143.75 s, `alcli demo-song`) | all bit-exact | 307 / 77, identical | 1908 / 1908 identical (all fields) |
| drum loop 124 BPM (31.7 s) | all bit-exact | 65 / 17, identical | 538 / 538 identical |
| click track 100 -> 140 BPM (45 s) | all bit-exact | 93 / 23, identical | 588 / 588 identical |
| demo excerpt, 44.1 kHz stereo 24-bit (40 s) | all bit-exact | 85 / 22, identical | 519 / 519 identical |

"All curves" means rmsDb, onsetEnv, the four bandOnset curves, intensity, percRatio,
bassPercRatio, pitchMidi, pitchConf, chroma and the waveform preview. BPM, tempo segments, meter,
beatInBar, section boundaries, labels and the energy/power/density/trend statistics are also
identical. Silence, a too-short input, 32 kHz stereo noise (non-integer resample ratio) and the
demo song rendered as a 32-bit float WAV were also checked by hand and are identical.

The table reflects the analysis after the tempo and beat-grid fixes in `analysis.cpp` (octave
folding by factors of 2 only, tempo changes need reliable windows, robust BPM regression, tight
grid fits accepted with fewer inliers, no shifted times from `refineOnset`). Both
implementations were changed together and stayed bit-exact.

The test tolerances are looser than what was measured. Results should stay inside them if a
future numpy or libm changes rounding by an ulp:

* per-frame curves: max |diff| <= 1e-4 (rmsDb <= 1e-3 dB) and Pearson r >= 0.999; pitchMidi:
  >= 99.5% of frames identical
* beats and downbeats: count within +-1, >= 99% matched within 5 ms in both directions
* BPM within 0.1, same tempo segment count (boundaries within 0.5 s), same meter
* sections: same count and labels, boundaries within 1 bar, energy/power within 0.01
* onset/kick/snare/hihat/note/sustain events: >= 97% matched within 10 ms in both directions;
  section, buildup, drop, tempo and silence events: same count, within 1 bar

### Residual differences (not observed, but possible)

* Libm differences on other platforms (macOS, Windows, other glibc versions). The C++ results
  themselves depend on the platform libm (`log1pf`, `log`, `exp`, `pow`), so "parity" means
  parity with the build on the same machine. `log1pf` here is the glibc 2.39 algorithm. A glibc
  with the correctly rounded CORE-MATH `log1pf` (2.41 or later) would differ by 1 ulp on about
  2% of spectrum bins. The flux curves would then differ around 1e-7, which is far inside the
  tolerances.
* Event ordering ties. Before the beat phase check the C++ code sorts events with an unstable
  `std::sort` by time, and Python uses a stable sort. Two events with exactly the same time can
  therefore end up in a different order. This only affects float summation order at the ulp
  level, and the final `stable_sort` by (time, type) only reorders events that are identical in
  time and type.
* Python sorts the section boundary candidates stably, while `std::sort` is not stable. The
  order can only differ when two novelty scores are exactly equal.

### Running the test

```
cmake --build build                         # alcli with the extended `analyze --json` export
python3 training/tests/test_audio_parity.py  # standalone (pytest is optional)
python3 -m pytest training/tests             # if pytest is installed
```

`alcli analyze <wav> --json out.json` exports the scalar fields it always had, plus full-precision
per-frame curves (`curves.*`), `beats`, `beatStrength`, `beatInBar`, `downbeats`, `hop`,
`hopCoarse`, section `power`/`firstBar`/`barCount`, and per-event `time`, `strength`, `band`,
`index` and `duration`.

### Runtime

For the 143.75 s demo song, Python takes about 4-5 s and C++ (`alcli analyze`) about 2.6 s. The
Python time is mostly the vectorised FFTs and the frame-by-frame Viterbi/DP loops, which are
sequential by nature.
