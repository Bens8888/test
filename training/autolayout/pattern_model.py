"""Pattern model: a small GRU over the level's pattern decisions.

Mirrors core/src/model/model.cpp exactly (input layout, gate order, window, file format), so a
model trained here runs unchanged in the mod. See docs/DECISIONS.md (D8, D9).

Step input = [24 context features | style one-hot (9) | density, density-auto flag | embedding of
the previous decision's pattern (row V = "none")].  GRU (PyTorch gate order r, z, n), linear head
to one logit per pattern token.  Each window of up to WINDOW + 1 decisions starts from a zero
state, at training and at inference time.
"""
from __future__ import annotations

import json
import struct
import zlib
from dataclasses import dataclass, field

import numpy as np
import torch
from torch import nn

N_FEATURES = 24
N_STYLES = 9          # gen::Style::Count (Auto + 8 styles)
WINDOW = 16
FORMAT_VERSION = 1


@dataclass
class ModelConfig:
    vocab: int
    hidden: int = 96
    embed: int = 24
    window: int = WINDOW
    style: int = -1          # -1 = all styles, else the style this model was fine-tuned for
    name: str = "all"
    metadata: dict = field(default_factory=dict)

    @property
    def input_size(self) -> int:
        return N_FEATURES + N_STYLES + 2 + self.embed


class PatternModel(nn.Module):
    def __init__(self, cfg: ModelConfig):
        super().__init__()
        self.cfg = cfg
        self.emb = nn.Embedding(cfg.vocab + 1, cfg.embed)
        self.gru = nn.GRU(cfg.input_size, cfg.hidden, batch_first=True)
        self.out = nn.Linear(cfg.hidden, cfg.vocab)

    def step_inputs(self, feats: torch.Tensor, style: torch.Tensor, density: torch.Tensor, prev: torch.Tensor) -> torch.Tensor:
        """feats (B,T,24), style (B,) long, density (B,) float (<0 = auto), prev (B,T) long (-1 = none)."""
        B, T, _ = feats.shape
        style_oh = torch.zeros(B, T, N_STYLES, dtype=feats.dtype, device=feats.device)
        style_oh.scatter_(2, style.view(B, 1, 1).expand(B, T, 1), 1.0)
        auto = (density < 0).to(feats.dtype)
        dens = torch.where(density < 0, torch.full_like(density, 0.5), density.clamp(0, 1))
        dens = torch.stack([dens, auto], dim=-1).view(B, 1, 2).expand(B, T, 2)
        prev_idx = torch.where(prev < 0, torch.full_like(prev, self.cfg.vocab), prev)
        return torch.cat([feats, style_oh, dens, self.emb(prev_idx)], dim=-1)

    def forward(self, feats, style, density, prev):
        x = self.step_inputs(feats, style, density, prev)
        h, _ = self.gru(x)
        return self.out(h)  # (B, T, V)

    # ------------------------------------------------------------------ export

    def export(self, path: str) -> bytes:
        """Writes the .alm file read by core/src/model/model.cpp."""
        cfg = self.cfg
        sd = {k: v.detach().cpu().float().numpy() for k, v in self.state_dict().items()}
        name = cfg.name.encode()
        meta = json.dumps(cfg.metadata, sort_keys=True).encode()
        head = b"ALMD" + struct.pack("<8I", FORMAT_VERSION, N_FEATURES, N_STYLES, cfg.embed, cfg.hidden, cfg.vocab,
                                     cfg.window, cfg.style & 0xFFFFFFFF)
        head += struct.pack("<I", len(name)) + name + struct.pack("<I", len(meta)) + meta
        body = b"".join(np.ascontiguousarray(a, dtype="<f4").tobytes() for a in [
            sd["emb.weight"], sd["gru.weight_ih_l0"], sd["gru.weight_hh_l0"], sd["gru.bias_ih_l0"], sd["gru.bias_hh_l0"],
            sd["out.weight"], sd["out.bias"]])
        data = head + body
        data += struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)
        with open(path, "wb") as f:
            f.write(data)
        return data

    @staticmethod
    def load(path: str) -> "PatternModel":
        data = open(path, "rb").read()
        assert data[:4] == b"ALMD", "not an AutoLayout model"
        assert zlib.crc32(data[:-4]) & 0xFFFFFFFF == struct.unpack("<I", data[-4:])[0], "checksum"
        ver, F, S, E, H, V, W, style = struct.unpack("<8I", data[4:36])
        assert ver == FORMAT_VERSION and F == N_FEATURES and S == N_STYLES
        pos = 36
        (n,) = struct.unpack("<I", data[pos:pos + 4]); pos += 4
        name = data[pos:pos + n].decode(); pos += n
        (n,) = struct.unpack("<I", data[pos:pos + 4]); pos += 4
        meta = json.loads(data[pos:pos + n].decode() or "{}"); pos += n
        style = style - (1 << 32) if style >= 1 << 31 else style
        cfg = ModelConfig(vocab=V, hidden=H, embed=E, window=W, style=style, name=name, metadata=meta)
        m = PatternModel(cfg)
        I = cfg.input_size
        shapes = [("emb.weight", (V + 1, E)), ("gru.weight_ih_l0", (3 * H, I)), ("gru.weight_hh_l0", (3 * H, H)),
                  ("gru.bias_ih_l0", (3 * H,)), ("gru.bias_hh_l0", (3 * H,)), ("out.weight", (V, H)), ("out.bias", (V,))]
        sd = {}
        for key, shp in shapes:
            cnt = int(np.prod(shp))
            sd[key] = torch.from_numpy(np.frombuffer(data, dtype="<f4", count=cnt, offset=pos).reshape(shp).copy())
            pos += cnt * 4
        m.load_state_dict(sd)
        return m


def parity_vectors(model: PatternModel, rng: np.random.Generator, cases: int = 8) -> list:
    """Random step sequences + the logits PyTorch computes for the last step (checked by the C++ test)."""
    out = []
    model.eval()
    with torch.no_grad():
        for _ in range(cases):
            T = int(rng.integers(1, model.cfg.window + 2))
            feats = torch.from_numpy(rng.random((1, T, N_FEATURES), dtype=np.float32))
            style = torch.tensor([int(rng.integers(0, N_STYLES))])
            density = torch.tensor([float(rng.choice([-1.0, rng.random()]))], dtype=torch.float32)
            prev = torch.from_numpy(rng.integers(-1, model.cfg.vocab, size=(1, T)))
            x = model.step_inputs(feats, style, density, prev)
            logits = model(feats, style, density, prev)[0, -1]
            out.append({"inputs": x[0].tolist(), "logits": logits.tolist(),
                        "features": feats[0].tolist(), "style": int(style[0]), "density": float(density[0]),
                        "prev": prev[0].tolist()})
    return out
