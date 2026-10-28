# Benchmarks

`./build/tools/alcli bench resources/models/model_all.alm docs/benchmarks.json`, Release build,
measured on the development machine: Intel Xeon @ 2.30 GHz, 4 threads, Linux, clang. (A typical
laptop CPU is in the same range or faster per core.)

```
analysis: demo song 143.7 s audio in 2.48 s; 210.0 s song in 3.68 s (1.05 s per minute of audio)
generation (210 s song, Insane): per variant median 0.54 s, max 0.70 s; 5 variants in parallel 0.93 s
physics: 8.2 M steps/s (162 full-level rollouts/s of a 210 s level)
bot: 15469 five-second rollouts per second
model: 66294 parameters, 294 us per decision (17-step GRU window)
generation with the model: 0.67 s for one variant (beaten with the designed clicks)
```

| Target (spec) | Measured |
|---|---|
| Song analysis < ~20 s for a 3-minute song | 3.6 s for a 3.5-minute song (about 1 s per minute of audio) |
| Generation < ~10 s per variant | ~0.5-0.7 s per variant (5 variants in parallel in about 1 s) |
| Thousands of bot rollouts per second | ~15,500 five-second rollouts per second (8 M physics steps/s) |
| Model runs in seconds on a laptop CPU, < 30 MB | ~0.3 ms per decision; 66k parameters, 258 KB per model, 2.1 MB for all 8 shipped models |

The generator fuzz test (`AL_FUZZ_N=400 ./build/tests/al_tests fuzz`: random seeds,
difficulties and styles on the demo song) and self-play runs count levels that the designed
clicks fail to beat. Latest runs: 400 fuzz levels with 0 failures, 0 repairs and the slowest
variant at 1.03 s; 1,800 self-play levels with 0 failures.
