"""Cut structures out of the levels in training/corpus/levels and measure each level's style.

    python extract.py [--skip id,id] [--pil dir] out_dir

Only objects with a hitbox are read, so decoration does not matter. A structure is a connected
group of blocks and slopes plus the small spikes touching it. Writes pool.json (every distinct
structure with the levels it comes from), styles.tsv and sheet.png (review sheet).
"""
import collections, json, math, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LEVELS = os.path.join(HERE, "levels")
CELL = 60
MAX_W, MAX_H = 600, 420      # bigger groups are whole sections, not structures
MAX_SPIKE = 14               # half-size hitboxes above this are big hazards, not attachments


def object_table():
    src = open(os.path.join(ROOT, "core/src/level/objects.cpp"), encoding="utf8").read()
    kinds = {}
    for m in re.finditer(r"range\(\{(.*?)\}\s*,\s*Kind::(\w+),\s*([\d.]+)f?,\s*([\d.]+)f?\)", src, re.S):
        for a, b in re.findall(r"\{(\d+),\s*(\d+)\}", m[1]):
            for i in range(int(a), int(b) + 1):
                kinds[i] = (m[2], float(m[3]), float(m[4]))
    for m in re.finditer(r"\bset\((\d+),\s*Kind::(\w+),\s*([\d.]+)f?,\s*([\d.]+)f?", src):
        kinds[int(m[1])] = (m[2], float(m[3]), float(m[4]))
    for m in re.finditer(r"saw\(\{(.*?)\},\s*([\d.]+)f?\)", src):
        for i in m[1].split(","):
            kinds[int(i)] = ("Saw", float(m[2]), float(m[2]))
    for pat, w in ((r"for \(int i : \{(289.*?)\}\)", 30), (r"for \(int i : \{(291.*?)\}\)", 60)):
        for i in re.search(pat, src, re.S)[1].split(","):
            kinds[int(i)] = ("Slope", w, 30)
    kinds[363] = kinds[1717] = ("HazardSlope", 30, 30)
    return kinds


class Obj:
    __slots__ = "kind id x y w h rot fx fy grouped scale".split()

    def box(self):
        th = math.radians(self.rot)
        c, s = abs(math.cos(th)), abs(math.sin(th))
        hw, hh = (self.w * c + self.h * s) / 2, (self.w * s + self.h * c) / 2
        return self.x - hw, self.y - hh, self.x + hw, self.y + hh


def read_level(path, kinds):
    out = []
    for text in open(path, encoding="utf8").read().split(";")[1:]:
        p = text.split(",")
        if len(p) < 4:
            continue
        m = dict(zip(p[0::2], p[1::2]))
        try:
            oid = int(m["1"])
            k = kinds.get(oid)
            if not k or m.get("121") == "1":
                continue
            scale = float(m.get("32", 1))
            o = Obj()
            o.kind, o.id, o.x, o.y = k[0], oid, float(m["2"]), float(m["3"])
            o.w, o.h = k[1] * scale * float(m.get("128", 1)), k[2] * scale * float(m.get("129", 1))
            o.rot, o.fx, o.fy = float(m.get("6", 0)), m.get("4") == "1", m.get("5") == "1"
            o.grouped = "57" in m
            o.scale = scale * float(m.get("128", 1)) * float(m.get("129", 1))
            out.append(o)
        except ValueError:
            continue
    return out


def overlap(a, b, gap):
    return a[0] <= b[2] + gap and b[0] <= a[2] + gap and a[1] <= b[3] + gap and b[1] <= a[3] + gap


def structures(objs):
    body = [o for o in objs if o.kind in ("Solid", "Slope")]
    boxes = [o.box() for o in body]
    parent = list(range(len(body)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    grid = collections.defaultdict(list)
    for n, b in enumerate(boxes):
        for cx in range(int(b[0] // CELL), int(b[2] // CELL) + 1):
            for cy in range(int(b[1] // CELL), int(b[3] // CELL) + 1):
                grid[cx, cy].append(n)
    for cell in grid.values():
        for i, a in enumerate(cell):
            for b in cell[i + 1:]:
                if overlap(boxes[a], boxes[b], 1):
                    parent[find(a)] = find(b)
    groups = collections.defaultdict(list)
    for n in range(len(body)):
        groups[find(n)].append(n)

    spikes = collections.defaultdict(list)
    for o in objs:
        if o.kind == "Hazard" and max(o.w, o.h) <= MAX_SPIKE:
            spikes[int(o.x // CELL), int(o.y // CELL)].append(o)

    found = []
    for members in groups.values():
        x0 = min(boxes[n][0] for n in members)
        y0 = min(boxes[n][1] for n in members)
        x1 = max(boxes[n][2] for n in members)
        y1 = max(boxes[n][3] for n in members)
        if x1 - x0 > MAX_W or y1 - y0 > MAX_H or (y1 - y0 < 8 and x1 - x0 < 8):
            continue
        attached = []
        for cx in range(int((x0 - 30) // CELL), int((x1 + 30) // CELL) + 1):
            for cy in range(int((y0 - 30) // CELL), int((y1 + 30) // CELL) + 1):
                for s in spikes.get((cx, cy), ()):
                    b = s.box()
                    inside = any(boxes[n][0] < s.x < boxes[n][2] and boxes[n][1] < s.y < boxes[n][3] for n in members)
                    if not inside and any(overlap(b, boxes[n], 16) for n in members):
                        attached.append(s)
        shape = tuple(sorted((body[n].kind, round((boxes[n][0] - x0) / 7.5), round((boxes[n][1] - y0) / 7.5),
                              round(boxes[n][2] - boxes[n][0]), round(boxes[n][3] - boxes[n][1]),
                              int(body[n].rot) % 360, body[n].fx, body[n].fy) for n in members))
        found.append({"shape": shape, "body": [body[n] for n in members], "spikes": attached,
                      "box": (x0, y0, x1, y1), "grouped": any(body[n].grouped for n in members)})
    return found


def style_row(name, objs, found):
    count = collections.Counter(o.kind for o in objs)
    reuse = collections.Counter(f["shape"] for f in found)
    pieces = sum(len(f["body"]) for f in found) or 1
    rails = sum(1 for f in found for o in f["body"] if o.kind == "Solid" and min(o.w, o.h) <= 4)
    slopes = sum(1 for f in found for o in f["body"] if o.kind == "Slope")
    length = max((o.x for o in objs), default=1)
    return {
        "level": name,
        "hitbox_objects": len(objs),
        "per_screen": round(len(objs) / max(length / 570, 1)),
        "structures": len(found),
        "distinct": len(reuse),
        "reuse": round(len(found) / max(len(reuse), 1), 2),
        "avg_pieces": round(pieces / max(len(found), 1), 1),
        "slope_share": round(slopes / pieces, 2),
        "rail_share": round(rails / pieces, 2),
        "spikes_per_structure": round(sum(len(f["spikes"]) for f in found) / max(len(found), 1), 2),
        "saws": count["Saw"],
        "orbs": count["Orb"],
        "grouped_share": round(sum(1 for f in found if f["grouped"]) / max(len(found), 1), 2),
    }


def draw_structure(d, f, cx, cy, size):
    x0, y0, x1, y1 = f["box"]
    sc = min(1.3, (size - 26) / max(x1 - x0 + 30, y1 - y0 + 30))
    mx, my = (x0 + x1) / 2, (y0 + y1) / 2

    def P(x, y):
        return cx + (x - mx) * sc, cy - (y - my) * sc

    for o in f["body"]:
        if o.kind == "Slope":
            tri = [(-o.w / 2, -o.h / 2), (o.w / 2, -o.h / 2), (o.w / 2, o.h / 2)]
            tri = [(-a if o.fx else a, -b if o.fy else b) for a, b in tri]
            th = math.radians(o.rot)
            d.polygon([P(o.x + a * math.cos(th) + b * math.sin(th), o.y - a * math.sin(th) + b * math.cos(th)) for a, b in tri],
                      fill=(70, 90, 170), outline=(255, 255, 255))
        else:
            b = o.box()
            d.rectangle([P(b[0], b[3]), P(b[2], b[1])], fill=(20, 26, 70), outline=(210, 215, 235))
    for s in f["spikes"]:
        th = math.radians(s.rot)
        ux, uy = math.sin(th), math.cos(th)     # where the tip points
        if s.fy:
            ux, uy = ux, -uy
        wx, wy = uy, -ux
        r = max(s.w, s.h) * 1.1
        d.polygon([P(s.x + ux * r, s.y + uy * r), P(s.x - ux * r + wx * r, s.y - uy * r + wy * r),
                   P(s.x - ux * r - wx * r, s.y - uy * r - wy * r)], fill=(245, 245, 245))


def sheet(rows, path, pil_dir, per_row=10, size=128):
    if pil_dir:
        sys.path.insert(0, pil_dir)
    from PIL import Image, ImageDraw
    label = 170
    im = Image.new("RGB", (label + per_row * size, len(rows) * size), (40, 70, 180))
    d = ImageDraw.Draw(im)
    for r, (name, tops) in enumerate(rows):
        d.text((6, r * size + 8), name[:26], fill=(255, 255, 255))
        for c, (f, n) in enumerate(tops[:per_row]):
            draw_structure(d, f, label + c * size + size // 2, r * size + size // 2, size)
            d.text((label + c * size + 3, r * size + 2), "x%d" % n, fill=(255, 240, 120))
    im.save(path)


def main():
    args = sys.argv[1:]
    skip, pil_dir = set(), None
    while args and args[0].startswith("--"):
        if args[0] == "--skip":
            skip = set(args[1].split(","))
        elif args[0] == "--pil":
            pil_dir = args[1]
        args = args[2:]
    out = args[0]
    os.makedirs(out, exist_ok=True)
    kinds = object_table()
    index = json.load(open(os.path.join(LEVELS, "index.json")))
    pool = {}
    styles, rows = [], []
    for level_id, info in index.items():
        if level_id in skip:
            continue
        objs = read_level(os.path.join(LEVELS, level_id + ".txt"), kinds)
        found = structures(objs)
        name = "%s (%s)" % (info["name"], level_id)
        styles.append(style_row(name, objs, found))
        reuse = collections.Counter(f["shape"] for f in found)
        first = {}
        for f in found:
            first.setdefault(f["shape"], f)
            e = pool.setdefault(repr(f["shape"]), {"pieces": len(f["body"]), "uses": collections.Counter()})
            e["uses"][level_id] += 1
        tops = [(first[s], n) for s, n in reuse.most_common(60) if len(s) >= 2]
        rows.append((name, tops))
        print(name, "structures", len(found), "distinct", len(reuse), flush=True)
    keys = list(styles[0])
    with open(os.path.join(out, "styles.tsv"), "w", encoding="utf8") as f:
        f.write("\t".join(keys) + "\n")
        for s in styles:
            f.write("\t".join(str(s[k]) for k in keys) + "\n")
    with open(os.path.join(out, "pool.json"), "w", encoding="utf8") as f:
        json.dump([{"shape": k, "pieces": v["pieces"], "uses": dict(v["uses"])} for k, v in pool.items()], f)
    sheet(rows, os.path.join(out, "sheet.png"), pil_dir)
    shared = sum(1 for v in pool.values() if len(v["uses"]) >= 2)
    print("pool:", len(pool), "distinct structures,", shared, "appear in 2+ levels")


if __name__ == "__main__":
    main()
