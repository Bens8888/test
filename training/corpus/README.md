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
  shapes used by two or more levels ship (RobTop's levels count as one).
- The Classic style learns from RobTop's own levels, rated Easy to Insane levels (2 to 9 stars, by
  the `stars` fetch.py stores) and a few classic easy demons. Its shapes use only the plain block
  and the outline pieces, and no style keeps a shape whose blocks lie over each other.
  `python fetch.py --rated 3,4,5 8` fetches the 80 most liked rated Hard, Harder and Insane levels
  each. Mixes and sped-up copies of RobTop's levels go in `ROBTOP_COPIES` in train.py.
- RobTop's levels come from the game itself: `Resources/levels/<n>.txt`, base64 of a gzipped level
  string, stored in `levels/` as `rt<n>.txt`.

To check the physics against the game, build `simtrace` and run it on a route and a trace from the
FB Pathfinder folder. Set `SIMTRACE_RESYNC=1` to list every mismatch across a whole level:

    simtrace <level string> routes/bb-g6.txt routes/tr-g6-trace.tsv
