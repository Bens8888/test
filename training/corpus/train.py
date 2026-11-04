"""Train the structure library: core/src/gen/module_library.inc.

    python train.py

Reads the levels fetched with fetch.py. What it learns:
  * the modules: structures from Ben's own levels, and from other levels only the generic shapes
    that two or more different levels use (no creator's signature geometry is shipped);
  * for every module which way it faces, which gamemodes it appears in and how often each style
    uses it;
  * for every style the spacing between structures, how often a shape repeats, the share of saws.
"""
import collections, json, math, os, statistics

import extract

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OWN = {"149382825", "151559360"}
STYLES = {
    "Rail": ["149382825", "151559360", "89496627"],
    "Slope": ["86407629", "151559360", "87665224"],
    "Compact": ["10565740", "58825144", "54953085"],
    "Spiky": ["149992717", "148598469", "49896559"],
    "Classic": [],   # filled in main(): RobTop's levels, rated Easy to Insane levels, classic easy demons
}
# classic easy demons (rated 10 stars, but plain gameplay of the kind Insane levels have)
CLASSIC_DEMONS = {"5310094", "7116121", "13519", "10109", "5904109", "6939821", "55520", "3543219", "11402965", "2842129", "814678",
                  "23298409", "38445559"}


# mixes and sped-up copies of RobTop's levels
ROBTOP_COPIES = {"411459", "741941", "1694003", "11698380", "856066", "862216", "877915", "509393", "3140440", "4376468"}


def source(level_id):
    """Who made a level: RobTop's levels and copies of them count as one source, every other level as its own."""
    return "robtop" if level_id.startswith("rt") or level_id in ROBTOP_COPIES else level_id
SKIP = {"13850073", "4125776", "11431524", "76962930", "rt3001"}   # copies, and one-of-a-kind mega structures
SPIKES = {8, 39, 103, 392, 144, 145, 177, 178, 179, 205, 216, 217, 218, 458, 459}
MODE_PORTALS = {12: "ground", 47: "ground", 745: "ground", 1331: "ground", 13: "fly", 111: "fly", 1933: "fly", 660: "wave"}
MODE_BIT = {"ground": 1, "fly": 2, "wave": 4}


def quarter(rot):
    q = rot / 90.0
    return abs(q - round(q)) < 0.01


OUTLINE_SET = {1, 467, 468, 469, 470, 471}   # the plain block and the outline pieces the tiler uses


def self_overlap(body):
    """Whether two solid pieces of a structure lie over each other (in the source level a fill
    covered that; drawn as outlines it is a tangle)."""
    boxes = [o.box() for o in body if o.kind == "Solid"]
    for i in range(len(boxes)):
        a = boxes[i]
        for b in boxes[i + 1:]:
            if min(a[2], b[2]) - max(a[0], b[0]) > 4 and min(a[3], b[3]) - max(a[1], b[1]) > 4:
                return True
    return False


def clean(f):
    """The structure as pieces, or None if it is not a usable module."""
    if len(f["body"]) < 2 or any(abs(o.scale - 1) > 0.01 or not quarter(o.rot) for o in f["body"]):
        return None
    if self_overlap(f["body"]):
        return None
    spikes = [s for s in f["spikes"] if s.id in SPIKES and abs(s.scale - 1) < 0.01 and quarter(s.rot)]
    seen, kept = set(), []
    for s in spikes:
        key = (round(s.x), round(s.y), s.id)
        if key not in seen:
            seen.add(key)
            kept.append(s)
    sideways = sum(1 for s in kept if int(round(s.rot / 90)) % 2 == 1)
    if len(kept) > 10 or sideways > 3:   # spike trees take over a stretch that repeats them
        return None
    thin = any(o.kind == "Solid" and min(o.w, o.h) <= 4 for o in f["body"])
    if not kept and not thin and not any(o.kind == "Slope" for o in f["body"]):
        return None                      # a plain block group says nothing next to the path
    x0, y0, x1, y1 = f["box"]
    w, h = x1 - x0, y1 - y0
    if w > 8 * 30 or h > 9 * 30 or (w > 180 and h > 180):
        return None
    ox, oy = math.floor(x0 / 30) * 30, math.floor(y0 / 30) * 30
    pieces = [(o.id, o.x - ox, o.y - oy, int(round(o.rot)) % 360, o.fx, o.fy) for o in f["body"] + kept]
    cells_w = max(1, math.ceil((x1 - ox) / 30))
    cells_h = max(1, math.ceil((y1 - oy) / 30))
    return pieces, cells_w, cells_h, facing(f["body"], kept)


def facing(body, spikes):
    """+1: the dangerous side points up (the module goes under the path); -1: down; 0: both."""
    up = down = 0
    for s in spikes:
        q = int(round(s.rot / 90)) % 4
        if q in (1, 3):
            continue
        d = 1 if q == 0 else -1
        d = -d if s.fy else d
        up += d > 0
        down += d < 0
    if up != down:
        return 1 if up > down else -1
    # rails run away from the path
    rails = [o for o in body if o.kind == "Solid" and min(o.w, o.h) <= 4]
    blocks = [o for o in body if not (o.kind == "Solid" and min(o.w, o.h) <= 4)]
    if rails and blocks:
        r = statistics.mean(o.y for o in rails) - statistics.mean(o.y for o in blocks)
        if abs(r) > 8:
            return -1 if r > 0 else 1
    return 0


def mirrored_y(pieces, cells_h):
    h = cells_h * 30
    return [(i, x, h - y, (-r) % 360, fx, not fy) for i, x, y, r, fx, fy in pieces]


def mode_at(portals, x):
    mode = "ground"
    for px, m in portals:
        if px > x:
            break
        mode = m
    return mode


def lanes(found):
    """For every structure the next one to its right in the same lane (overlapping heights)."""
    order = sorted(found, key=lambda f: f["box"][0])
    pairs = []
    for i, a in enumerate(order):
        for b in order[i + 1:i + 40]:
            if b["box"][0] >= a["box"][2] - 1 and a["box"][1] < b["box"][3] and b["box"][1] < a["box"][3]:
                pairs.append((a, b))
                break
    return pairs


def main():
    kinds = extract.object_table()
    index = json.load(open(os.path.join(extract.LEVELS, "index.json")))
    STYLES["Classic"] = sorted(l for l, e in index.items() if l not in SKIP and
                               (l.startswith("rt") or 2 <= e.get("stars", 0) <= 9 or l in CLASSIC_DEMONS))
    print("classic levels:", len(STYLES["Classic"]))
    level_found, level_portals, level_saws = {}, {}, {}
    for level_id in index:
        if level_id in SKIP:
            continue
        objs = extract.read_level(os.path.join(extract.LEVELS, level_id + ".txt"), kinds)
        level_found[level_id] = extract.structures(objs)
        level_portals[level_id] = sorted((o.x, MODE_PORTALS[o.id]) for o in objs if o.id in MODE_PORTALS)
        level_saws[level_id] = sum(1 for o in objs if o.kind == "Saw" and o.w > 8)

    table = {}
    for level_id, found in level_found.items():
        for f in found:
            c = clean(f)
            if not c:
                continue
            e = table.setdefault(f["shape"], {"c": c, "levels": collections.Counter(), "modes": 0})
            e["levels"][level_id] += 1
            e["modes"] |= MODE_BIT[mode_at(level_portals[level_id], f["box"][0])]

    style_names = list(STYLES)
    modules = []
    for e in table.values():
        own = any(l in OWN for l in e["levels"])
        if not own and len({source(l) for l in e["levels"] if l not in OWN}) < 2:
            continue
        pieces, w, h, side = e["c"]
        weights = [sum(e["levels"][l] for l in STYLES[s]) for s in style_names]
        # the classic look is one block set: a module mixing in others would look patched together
        if any(p[0] not in OUTLINE_SET and p[0] not in SPIKES for p in pieces):
            weights[style_names.index("Classic")] = 0
        if sum(1 for p in pieces if p[0] in SPIKES) < 2:
            weights[style_names.index("Spiky")] = 0
        if not any(weights):
            continue
        if side > 0:                       # stored facing down: every module hangs over the path
            pieces, side = mirrored_y(pieces, h), -1
        modules.append((pieces, w, h, 0 if side == 0 else 1, e["modes"], weights))
    modules.sort(key=lambda m: (-sum(m[5]), m[1], m[2]))

    stats = []
    for s in style_names:
        g, repeats, pairs, saws, rails, pieces, count = [], 0, 0, 0, 0, 0, 0
        for l in STYLES[s]:
            found = level_found[l]
            for a, b in lanes(found):
                pairs += 1
                g.append((b["box"][0] - a["box"][2]) / 30)
                repeats += a["shape"] == b["shape"]
            saws += level_saws[l]
            count += len(found)
            rails += sum(1 for f in found for o in f["body"] if o.kind == "Solid" and min(o.w, o.h) <= 4)
            pieces += sum(len(f["body"]) for f in found)
        g = sorted(v for v in g if v < 20) or [2.0]
        stats.append({"name": s, "pitchLo": g[len(g) // 4], "pitchHi": g[3 * len(g) // 4], "repeat": repeats / max(pairs, 1),
                      "rails": rails / max(pieces, 1), "saws": saws / max(count, 1)})

    path = os.path.join(ROOT, "core/src/gen/module_library.inc")
    with open(path, "w", encoding="utf8", newline="\n") as f:
        f.write("// Generated by training/corpus/train.py. Do not edit.\n")
        f.write("static const ModulePiece kPieces[] = {\n")
        start = []
        n = 0
        for pieces, *_ in modules:
            start.append(n)
            for i, x, y, r, fx, fy in pieces:
                f.write("{%d,%.2ff,%.2ff,%d,%d,%d},\n" % (i, x, y, r, fx, fy))
                n += 1
        f.write("};\nstatic const Module kModules[] = {\n")
        for st, (pieces, w, h, directed, modes, weights) in zip(start, modules):
            f.write("{%d,%d,%d,%d,%d,%d,{%s}},\n" % (st, len(pieces), w, h, directed, modes, ",".join(str(min(v, 60000)) for v in weights)))
        f.write("};\nstatic const StyleStats kStyles[] = {\n")
        for s in stats:
            f.write('{"%s",%.2ff,%.2ff,%.3ff,%.3ff},\n' % (s["name"], s["pitchLo"], s["pitchHi"], s["repeat"], s["rails"]))
        f.write("};\n")
    print(len(modules), "modules,", sum(len(m[0]) for m in modules), "pieces")
    for s in stats:
        print(s)
    print("per style:", [sum(1 for m in modules if m[5][i]) for i in range(len(style_names))])
    print("by mode:", collections.Counter(m[4] for m in modules))
    print("from Ben's levels only:", sum(1 for e in table.values() if set(e["levels"]) <= OWN))


if __name__ == "__main__":
    main()
