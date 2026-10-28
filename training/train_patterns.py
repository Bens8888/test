#!/usr/bin/env python3
"""Train the AutoLayout pattern models.

    python3 training/train_patterns.py --data training/data/selfplay_r0.jsonl [more.jsonl ...] \
        --out training/models [--epochs 12] [--hidden 96] [--per-style]

Data: JSON Lines written by `aldata selfplay` / `aldata extract-dir` (one sequence of pattern
decisions per line). Writes model_all.alm (+ model_<style>.alm fine-tunes with --per-style),
parity_<name>.json (test vectors for the C++ engine) and train_report.json (data, accuracy,
hardware and wall time).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import platform
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from autolayout.pattern_model import N_FEATURES, N_STYLES, WINDOW, ModelConfig, PatternModel, parity_vectors  # noqa: E402

STYLE_NAMES = ["auto", "flow", "tight", "chaotic", "minimal", "retro", "modern", "memory"]


# ---------------------------------------------------------------------------- data

class Seq:
    __slots__ = ("feats", "tokens", "style", "density", "reward", "source")

    def __init__(self, j: dict):
        steps = [s for s in j["steps"] if s.get("t", -1) >= 0]
        self.feats = np.array([s["f"] for s in steps], dtype=np.float32).reshape(-1, N_FEATURES)
        self.tokens = np.array([s["t"] for s in steps], dtype=np.int64)
        self.style = int(j.get("style", 0))
        self.density = float(j.get("density", -1.0))
        self.reward = float(j.get("reward", 0.0))
        self.source = j.get("source", "")


def load(paths: list[str]) -> list[Seq]:
    seqs = []
    for p in paths:
        with open(p) as f:
            for line in f:
                line = line.strip()
                if line:
                    s = Seq(json.loads(line))
                    if len(s.tokens) >= 4:
                        seqs.append(s)
    return seqs


def augment(feats: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    """Feature-space augmentation. Tempo shift: the click gaps (f17, f18 = min(gap, 2)/2) scale by r.
    Intensity jitter. (Transposition and vertical mirroring leave the features and tokens unchanged:
    pitch enters only as a relative movement and every pattern is gravity-relative.)"""
    f = feats.copy()
    r = rng.uniform(0.85, 1.18)
    for k in (17, 18):
        gap = f[:, k] * 2.0
        f[:, k] = np.where(f[:, k] < 0.999, np.minimum(2.0, gap * r) / 2.0, f[:, k])
    f[:, 8] = np.clip(f[:, 8] + rng.normal(0, 0.04, size=len(f)), 0, 1)
    return f


def make_batch(seqs: list[Seq], idx: np.ndarray, rng: np.random.Generator, train: bool, vocab: int):
    """Random crops of up to WINDOW + 1 decisions (each window starts from a zero GRU state, like
    the C++ inference which looks at the last WINDOW decisions)."""
    L = WINDOW + 1
    B = len(idx)
    feats = np.zeros((B, L, N_FEATURES), np.float32)
    prev = np.full((B, L), -1, np.int64)
    target = np.full((B, L), -100, np.int64)  # -100 = ignored (padding)
    style = np.zeros(B, np.int64)
    dens = np.zeros(B, np.float32)
    for b, i in enumerate(idx):
        s = seqs[i]
        n = len(s.tokens)
        start = int(rng.integers(0, max(1, n - 1))) if train else 0
        end = min(n, start + L)
        if train and rng.random() < 0.3:  # also learn short histories (start of a level)
            end = min(end, start + int(rng.integers(1, L + 1)))
        f = s.feats[start:end]
        if train:
            f = augment(f, rng)
        T = end - start
        feats[b, :T] = f
        target[b, :T] = s.tokens[start:end]
        prev[b, 0] = s.tokens[start - 1] if start > 0 else -1
        prev[b, 1:T] = s.tokens[start:end - 1]
        style[b] = s.style if 0 <= s.style < N_STYLES else 0
        dens[b] = s.density
    return (torch.from_numpy(feats), torch.from_numpy(style), torch.from_numpy(dens), torch.from_numpy(prev), torch.from_numpy(target))


def evaluate(model: PatternModel, seqs: list[Seq], vocab: int) -> dict:
    """Next-decision accuracy over every position of every validation sequence, with each position
    seeing at most WINDOW previous decisions (exactly what the mod sees)."""
    model.eval()
    correct = top5 = total = 0
    nll = 0.0
    with torch.no_grad():
        for s in seqs:
            n = len(s.tokens)
            # sliding windows: positions [k*W, (k+1)*W) predicted with up to W decisions of context
            for t0 in range(0, n, WINDOW):
                lo = max(0, t0 - WINDOW)
                hi = min(n, t0 + WINDOW)
                f = torch.from_numpy(s.feats[lo:hi])[None]
                prev = np.full(hi - lo, -1, np.int64)
                prev[0] = s.tokens[lo - 1] if lo > 0 else -1
                prev[1:] = s.tokens[lo:hi - 1]
                logits = model(f, torch.tensor([s.style if 0 <= s.style < N_STYLES else 0]), torch.tensor([s.density], dtype=torch.float32),
                               torch.from_numpy(prev)[None])[0]
                tgt = torch.from_numpy(s.tokens[lo:hi])
                sel = slice(t0 - lo, hi - lo)
                lg, tg = logits[sel], tgt[sel]
                nll += F.cross_entropy(lg, tg, reduction="sum").item()
                correct += (lg.argmax(-1) == tg).sum().item()
                top5 += (lg.topk(min(5, lg.shape[-1]), -1).indices == tg[:, None]).any(-1).sum().item()
                total += len(tg)
    return {"accuracy": correct / max(1, total), "top5": top5 / max(1, total), "nll": nll / max(1, total), "decisions": total}


def baseline(train: list[Seq], val: list[Seq]) -> float:
    """Accuracy of always predicting the most frequent token of the decision's gamemode (features 0..7)."""
    counts: dict[tuple[int, int], int] = {}
    for s in train:
        modes = s.feats[:, :8].argmax(1)
        for m, t in zip(modes, s.tokens):
            counts[(int(m), int(t))] = counts.get((int(m), int(t)), 0) + 1
    best = {}
    for (m, t), c in counts.items():
        if c > best.get(m, (0, -1))[0]:
            best[m] = (c, t)
    ok = tot = 0
    for s in val:
        for m, t in zip(s.feats[:, :8].argmax(1), s.tokens):
            ok += int(best.get(int(m), (0, -1))[1] == int(t))
            tot += 1
    return ok / max(1, tot)


def train(model: PatternModel, seqs: list[Seq], val: list[Seq], epochs: int, lr: float, batch: int, rng: np.random.Generator, vocab: int, log) -> dict:
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    # an "epoch" = as many windows as decisions / WINDOW over the training set
    steps_per_epoch = max(10, sum(len(s.tokens) for s in seqs) // (WINDOW * batch) * 2)
    total_steps = steps_per_epoch * epochs
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=total_steps, pct_start=0.15)
    best = None
    history = []
    for ep in range(epochs):
        model.train()
        t0 = time.time()
        loss_sum = 0.0
        for _ in range(steps_per_epoch):
            idx = rng.integers(0, len(seqs), size=batch)
            feats, style, dens, prev, tgt = make_batch(seqs, idx, rng, True, vocab)
            logits = model(feats, style, dens, prev)
            loss = F.cross_entropy(logits.reshape(-1, vocab), tgt.reshape(-1), ignore_index=-100)
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            loss_sum += loss.item()
        ev = evaluate(model, val, vocab) if val else {}
        history.append({"epoch": ep + 1, "train_loss": loss_sum / steps_per_epoch, **ev, "seconds": time.time() - t0})
        log(f"  epoch {ep + 1:2d}/{epochs}: loss {loss_sum / steps_per_epoch:.3f}" +
            (f"  val acc {ev['accuracy']:.3f} top5 {ev['top5']:.3f} nll {ev['nll']:.3f}" if ev else "") + f"  ({time.time() - t0:.1f} s)")
        if ev and (best is None or ev["nll"] < best[0]):
            best = (ev["nll"], {k: v.clone() for k, v in model.state_dict().items()}, ev)
    if best:
        model.load_state_dict(best[1])
        return {"history": history, "best": best[2]}
    return {"history": history, "best": {}}


def hardware() -> dict:
    cpu = platform.processor() or platform.machine()
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    return {"cpu": cpu, "threads": torch.get_num_threads(), "torch": torch.__version__, "python": platform.python_version(),
            "os": platform.platform(), "gpu": "none (CPU only)"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", nargs="+", required=True)
    ap.add_argument("--vocab", default=None, help="vocab.json from `aldata vocab` (default: infer the size from the data)")
    ap.add_argument("--vocab-size", type=int, default=None)
    ap.add_argument("--out", default="training/models")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--hidden", type=int, default=96)
    ap.add_argument("--embed", type=int, default=24)
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--val", type=float, default=0.1)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--per-style", action="store_true", help="also fine-tune one model per style")
    ap.add_argument("--init", default=None, help="start from this .alm (fine-tuning / self-play rounds)")
    ap.add_argument("--name", default="all")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)
    t_start = time.time()
    log_lines = []

    def log(msg):
        print(msg, flush=True)
        log_lines.append(msg)

    if args.vocab:
        vocab = len(json.load(open(args.vocab))["vocab"])
    elif args.vocab_size:
        vocab = args.vocab_size
    else:
        raise SystemExit("give --vocab vocab.json (from `aldata vocab`) or --vocab-size")
    seqs = load(args.data)
    if not seqs:
        raise SystemExit("no training sequences")
    # split by source (song / level) so validation songs are unseen
    sources = sorted({s.source.split(":")[0] + ":" + s.source.split(":")[-1] for s in seqs})
    rng.shuffle(sources)
    nval = max(1, int(len(sources) * args.val))
    val_src = set(sources[:nval])
    key = lambda s: s.source.split(":")[0] + ":" + s.source.split(":")[-1]
    tr = [s for s in seqs if key(s) not in val_src]
    va = [s for s in seqs if key(s) in val_src]
    n_dec = sum(len(s.tokens) for s in seqs)
    log(f"data: {len(seqs)} sequences, {n_dec} decisions, vocab {vocab}; train {len(tr)} / val {len(va)} sequences")
    base = baseline(tr, va)
    log(f"baseline (most frequent pattern per gamemode): val accuracy {base:.3f}")

    cfg = ModelConfig(vocab=vocab, hidden=args.hidden, embed=args.embed, name=args.name)
    model = PatternModel.load(args.init) if args.init else PatternModel(cfg)
    if args.init:
        model.cfg.name = args.name
    params = sum(p.numel() for p in model.parameters())
    log(f"model: GRU hidden {model.cfg.hidden}, embed {model.cfg.embed}, window {WINDOW}, {params} parameters ({params * 4 / 1024:.0f} KB)")
    result = train(model, tr, va, args.epochs, args.lr, args.batch, rng, vocab, log)
    hw = hardware()
    report = {"data": {"files": args.data, "sequences": len(seqs), "decisions": n_dec, "train_sequences": len(tr), "val_sequences": len(va)},
              "baseline_accuracy": base, "models": {}, "hardware": hw}
    meta = {"trained_on": n_dec, "val": result["best"], "baseline": base, "hardware": hw["cpu"], "torch": hw["torch"]}
    model.cfg.metadata = meta
    path = os.path.join(args.out, f"model_{args.name}.alm")
    data = model.export(path)
    json.dump(parity_vectors(model, np.random.default_rng(1)), open(os.path.join(args.out, f"parity_{args.name}.json"), "w"))
    report["models"][args.name] = {"file": path, "bytes": len(data), **result}
    log(f"wrote {path} ({len(data) / 1024:.0f} KB)")

    if args.per_style:
        general = {k: v.clone() for k, v in model.state_dict().items()}
        for st in range(1, N_STYLES):
            st_tr = [s for s in tr if s.style == st]
            st_va = [s for s in va if s.style == st]
            if len(st_tr) < 20:
                log(f"style {STYLE_NAMES[st]}: only {len(st_tr)} sequences, using the general model")
                continue
            log(f"fine-tuning for style {STYLE_NAMES[st]} ({len(st_tr)} sequences)")
            m = PatternModel(ModelConfig(vocab=vocab, hidden=model.cfg.hidden, embed=model.cfg.embed, style=st, name=STYLE_NAMES[st]))
            m.load_state_dict(general)
            res = train(m, st_tr, st_va or va, max(3, args.epochs // 3), args.lr * 0.3, args.batch, rng, vocab, log)
            m.cfg.metadata = {**meta, "style": STYLE_NAMES[st], "val": res["best"]}
            p = os.path.join(args.out, f"model_{STYLE_NAMES[st]}.alm")
            d = m.export(p)
            report["models"][STYLE_NAMES[st]] = {"file": p, "bytes": len(d), **res}
            log(f"wrote {p}")

    report["seconds"] = time.time() - t_start
    report["log"] = log_lines
    json.dump(report, open(os.path.join(args.out, f"train_report_{args.name}.json"), "w"), indent=1)
    log(f"training took {report['seconds']:.1f} s on {hw['cpu']} ({hw['threads']} threads, {hw['gpu']})")


if __name__ == "__main__":
    main()
