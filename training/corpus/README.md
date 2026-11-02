# Structure corpus

Teaches the generator what good structures look like.

    python fetch.py 149382825 151559360 "Tidal Wave" Acheron ...
    python extract.py --pil <dir with Pillow> <out dir>
    python train.py

- `fetch.py` downloads levels by ID or name into `levels/` (not committed). Check the creator it
  prints: a name search can find a copy.
- `extract.py` reads only objects with a hitbox, groups them into structures and writes a review
  sheet and each level's style numbers.
- `train.py` writes `core/src/gen/module_library.inc`. Edit its `STYLES` to change which levels define a
  style, and `OWN` for levels whose structures may ship as they are. From every other level only
  shapes used by two or more levels ship.

To check the physics against the game, build `simtrace` and run it on a route and a trace from the
FB Pathfinder folder. Set `SIMTRACE_RESYNC=1` to list every mismatch across a whole level:

    simtrace <level string> routes/bb-g6.txt routes/tr-g6-trace.tsv
