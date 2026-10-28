#!/usr/bin/env python3
"""Self-play training loop: the physics bot is the judge.

    python3 training/selfplay_loop.py --rounds 2 --songs 120 [--start-data training/data/selfplay_r0.jsonl]

Round 0 data comes from the rule-based generator (`aldata selfplay`). Each round: train (or
fine-tune) the model on all kept data so far, let the new model generate on fresh random songs,
keep the best variants by bot-verified reward (rejection sampling), repeat. After every round the
rules and the current model are scored on the same held-out songs, so the effect of training is
measured, not assumed. Results go to training/models/selfplay_report.json.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALDATA = os.environ.get("ALDATA", os.path.join(ROOT, "build", "tools", "aldata"))


def run(cmd: list[str]) -> str:
    print("$", " ".join(cmd), flush=True)
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout[-3000:], p.stderr[-3000:])
        raise SystemExit(f"command failed: {cmd[0]}")
    return p.stdout


def summary(out: str) -> dict:
    for line in out.splitlines():
        if line.startswith("SUMMARY "):
            return json.loads(line[len("SUMMARY "):])
    return {}


def evaluate(model: str | None, songs: int, seed: int, tmp: str) -> dict:
    path = os.path.join(tmp, "eval.jsonl")
    if os.path.exists(path):
        os.remove(path)
    cmd = [ALDATA, "selfplay", "--out", path, "--songs", str(songs), "--variants", "3", "--keep", "0.01", "--seed", str(seed)]
    if model:
        cmd += ["--model", model]
    return summary(run(cmd))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--songs", type=int, default=120, help="new self-play songs per round")
    ap.add_argument("--variants", type=int, default=6)
    ap.add_argument("--eval-songs", type=int, default=24)
    ap.add_argument("--start-data", nargs="*", default=[os.path.join(ROOT, "training", "data", "selfplay_r0.jsonl")])
    ap.add_argument("--extra-data", nargs="*", default=[], help="real-level data (aldata extract-dir) mixed into every round")
    ap.add_argument("--out", default=os.path.join(ROOT, "training", "models"))
    ap.add_argument("--epochs", type=int, default=12)
    args = ap.parse_args()
    if not os.path.exists(ALDATA):
        raise SystemExit("build the tools first: cmake -B build -DAL_CORE_ONLY=ON && cmake --build build")
    os.makedirs(args.out, exist_ok=True)
    tmp = os.path.join(args.out, "tmp")
    os.makedirs(tmp, exist_ok=True)
    vocab = os.path.join(args.out, "vocab.json")
    run([ALDATA, "vocab", vocab])
    t0 = time.time()
    report = {"rounds": [], "eval_seed": 777}
    report["rules_eval"] = evaluate(None, args.eval_songs, 777, tmp)
    print("rules on the held-out songs:", report["rules_eval"], flush=True)
    data = list(args.start_data) + list(args.extra_data)
    prev_model = None
    for r in range(args.rounds + 1):
        rd = os.path.join(args.out, f"round{r}")
        os.makedirs(rd, exist_ok=True)
        cmd = [sys.executable, os.path.join(ROOT, "training", "train_patterns.py"), "--data", *data, "--vocab", vocab, "--out", rd,
               "--epochs", str(args.epochs if r == 0 else max(4, args.epochs // 2))]
        if prev_model:
            cmd += ["--init", prev_model, "--lr", "1e-3"]
        tr_out = run(cmd)
        model = os.path.join(rd, "model_all.alm")
        ev = evaluate(model, args.eval_songs, 777, tmp)
        print(f"round {r}: model on the held-out songs: {ev}", flush=True)
        entry = {"round": r, "data": list(data), "eval": ev, "train_log": tr_out.splitlines()[-25:]}
        report["rounds"].append(entry)
        prev_model = model
        if r == args.rounds:
            break
        new = os.path.join(ROOT, "training", "data", f"selfplay_r{r + 1}.jsonl")
        if os.path.exists(new):
            os.remove(new)
        sp = run([ALDATA, "selfplay", "--out", new, "--songs", str(args.songs), "--variants", str(args.variants), "--keep", "0.34",
                  "--seed", str(5000 + 1000 * r), "--model", model])
        entry["selfplay"] = summary(sp)
        data.append(new)
        json.dump(report, open(os.path.join(args.out, "selfplay_report.json"), "w"), indent=1)
    report["seconds"] = time.time() - t0
    json.dump(report, open(os.path.join(args.out, "selfplay_report.json"), "w"), indent=1)
    print(f"self-play loop finished in {report['seconds'] / 60:.1f} min", flush=True)


if __name__ == "__main__":
    main()
