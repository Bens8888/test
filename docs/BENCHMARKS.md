# Benchmarks

`./build/tools/alcli bench resources/models/model_all.alm docs/benchmarks.json`, Release build,
measured on the development machine: Intel Xeon @ 2.30 GHz, 4 threads, Linux, clang. (A typical
laptop CPU is in the same range or faster per core.)

```
analysis: demo song 143.7 s audio in 2.43 s; 210.0 s song in 3.66 s (1.05 s per minute of audio)
generation (210 s song, Insane): per variant median 0.54 s, max 0.72 s; 5 variants in parallel 0.97 s
physics: 6.3 M steps/s (124 full-level rollouts/s of a 210 s level)
bot: 17046 five-second rollouts per second
model: 66294 parameters, 275 us per decision (17-step GRU window)
generation with the model: 0.58 s for one variant (beaten with the designed clicks)
```

| Target (spec) | Measured |
|---|---|
| Song analysis < ~20 s for a 3-minute song | 3.6 s for a 3.5-minute song (about 1 s per minute of audio) |
| Generation < ~10 s per variant | ~0.5-0.7 s per variant (5 variants in parallel in about 1 s) |
| Thousands of bot rollouts per second | ~16,700 five-second rollouts per second (6 M physics steps/s) |
| Model runs in seconds on a laptop CPU, < 30 MB | ~0.3 ms per decision; 66k parameters, 259 KB per model, 2.3 MB for all 9 shipped models |

The generator fuzz test (`AL_FUZZ_N=200 ./build/tests/al_tests fuzz`) and self-play runs (600
levels in the last run, 1,800 in round 0) count levels that the designed clicks fail to beat:
0 failures in the latest runs.
