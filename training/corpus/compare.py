"""How generated levels compare with real ones: python compare.py level.txt ... (level strings)."""
import collections, json, os, sys

import extract

KEEP = {"149382825": "bidal wave", "151559360": "bidal wave 2", "86407629": "Tidal Wave", "149992717": "GRIEF", "10565740": "Bloodbath",
        "58825144": "xo", "148598469": "Artemis 13", "73667628": "Acheron"}


def row(name, path, kinds):
    objs = extract.read_level(path, kinds)
    found = extract.structures(objs)
    length = max((o.x for o in objs), default=1)
    screens = max(length / 570, 1)
    count = collections.Counter(o.kind for o in objs)
    pieces = [len(f["body"]) for f in found] or [0]
    # how much of the height around the middle of the action is filled, per column
    cols = collections.defaultdict(float)
    for o in objs:
        if o.kind in ("Solid", "Slope", "Hazard", "Saw"):
            b = o.box()
            for c in range(int(b[0] // 30), int(b[2] // 30) + 1):
                cols[c] += 1
    cover = len(cols) / max(length / 30, 1)
    print("%-22s %7.0f/screen  solids %5.0f  hazards %5.0f  saws %4.0f  structs %5.1f  avg size %5.1f  columns used %3.0f%%" % (
        name[:22], len(objs) / screens, (count["Solid"] + count["Slope"]) / screens, count["Hazard"] / screens, count["Saw"] / screens,
        len(found) / screens, sum(pieces) / len(pieces), cover * 100))


def main():
    kinds = extract.object_table()
    for i, n in KEEP.items():
        row(n, os.path.join(extract.LEVELS, i + ".txt"), kinds)
    for p in sys.argv[1:]:
        row("MINE " + os.path.basename(p), p, kinds)


if __name__ == "__main__":
    main()
