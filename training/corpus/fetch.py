"""Download reference levels from the GD servers into training/corpus/levels (not committed).

    python fetch.py 149382825 "Tidal Wave" Acheron ...

A number is a level ID; anything else is searched by name and the most downloaded match is taken.
"""
import base64, json, os, sys, time, urllib.parse, urllib.request, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "levels")
BASE = "https://www.boomlings.com/database/"


def post(endpoint, **fields):
    fields["secret"] = "Wmfd2893gb7"
    req = urllib.request.Request(BASE + endpoint, urllib.parse.urlencode(fields).encode(), {"User-Agent": ""})
    with urllib.request.urlopen(req, timeout=40) as r:
        return r.read().decode("latin1")


def parse(entry):
    p = entry.split(":")
    return dict(zip(p[0::2], p[1::2]))


def search(name):
    body = post("getGJLevels21.php", type=0, str=name)
    if body.startswith("-1"):
        return None
    head = body.split("#")
    creators = dict(c.split(":")[:2] for c in head[1].split("|") if c.count(":") >= 2)
    levels = [parse(e) for e in head[0].split("|")]
    exact = [l for l in levels if l.get("2", "").lower() == name.lower()] or levels
    best = max(exact, key=lambda l: int(l.get("10", 0)))
    best["creator"] = creators.get(best.get("6"), "?")
    return best


def rated(diff, pages):
    """IDs of the most liked rated levels of one difficulty (3 Hard, 4 Harder, 5 Insane), 10 per page."""
    ids = []
    for page in range(pages):
        body = post("getGJLevels21.php", type=2, diff=diff, star=1, page=page)
        time.sleep(2)
        if body.startswith("-1"):
            break
        ids += [parse(e)["1"] for e in body.split("#")[0].split("|")]
    return ids


def main():
    os.makedirs(OUT, exist_ok=True)
    index_path = os.path.join(OUT, "index.json")
    index = json.load(open(index_path)) if os.path.exists(index_path) else {}
    args = sys.argv[1:]
    if args[:1] == ["--rated"]:
        # fetch.py --rated 3,4,5 6: the most liked rated levels of those difficulties, 6 pages each
        args = [i for d in args[1].split(",") for i in rated(d, int(args[2]))]
    for arg in args:
        creator = "?"
        level_id = arg
        if not arg.isdigit():
            found = search(arg)
            time.sleep(2)
            if not found:
                print("not found:", arg)
                continue
            level_id, creator = found["1"], found["creator"]
        path = os.path.join(OUT, level_id + ".txt")
        if level_id in index and os.path.exists(path):
            continue
        fields = parse(post("downloadGJLevel22.php", levelID=level_id).split("#")[0])
        time.sleep(2)
        if "4" not in fields:
            print("no level data:", arg)
            continue
        text = zlib.decompress(base64.urlsafe_b64decode(fields["4"] + "=="), 47).decode("utf8", "replace")
        with open(path, "w", encoding="utf8") as f:
            f.write(text)
        index[level_id] = {"name": fields.get("2"), "creator": creator, "objects": text.count(";") - 1, "song": fields.get("35"),
                           "stars": int(fields.get("18", 0) or 0)}
        print(level_id, fields.get("2"), "by", creator, text.count(";") - 1, "objects,", fields.get("18", "?"), "stars", flush=True)
        json.dump(index, open(index_path, "w"), indent=1)


if __name__ == "__main__":
    main()
