#!/usr/bin/env python3
"""Writes the small random model + PyTorch reference logits used by the C++ parity test
(tests/test_model.cpp). Re-run after changing the model or the vocabulary:
    python3 training/make_test_fixture.py --vocab-size $(./build/tools/aldata vocab /dev/stdout | ...)"""
import argparse
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from autolayout.pattern_model import ModelConfig, PatternModel, parity_vectors  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--vocab", required=True, help="vocab.json from `aldata vocab`")
ap.add_argument("--out", default="tests/data")
a = ap.parse_args()
torch.manual_seed(3)
V = len(json.load(open(a.vocab))["vocab"])
m = PatternModel(ModelConfig(vocab=V, hidden=20, embed=6, name="fixture"))
with torch.no_grad():
    for p in m.parameters():
        p.mul_(3.0)  # larger weights: non-trivial gate activations
os.makedirs(a.out, exist_ok=True)
m.export(os.path.join(a.out, "model_fixture.alm"))
json.dump(parity_vectors(m, np.random.default_rng(5), cases=12), open(os.path.join(a.out, "model_fixture_parity.json"), "w"))
print("wrote", a.out)
