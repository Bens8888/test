"""Python port of the simple scores and the renderer in core/src/audio/synth.cpp (clickTrack,
clickTrackTempoChange, drumLoop, render) used to build extra parity-test signals.

Same formulas, same PCG32 noise stream (al::Rng), same mixing order. It is not required to be
bit-identical to the C++ renderer: the parity tests write the audio to a WAV file and both the
C++ (`alcli analyze`) and the Python analysis read that same file.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import List

import numpy as np

KICK, SNARE, HIHAT, CLAP = 0, 1, 2, 3
_TAU = 6.283185307179586
_M64 = (1 << 64) - 1


class Rng:
    """al::Rng (PCG32 XSH-RR)."""

    def __init__(self, seed: int = 0x853C49E6748FEA9B, stream: int = 0xDA3E39CB94B95BDB):
        self.state = 0
        self.inc = ((stream << 1) | 1) & _M64
        self.next()
        self.state = (self.state + seed) & _M64
        self.next()

    def next(self) -> int:
        old = self.state
        self.state = (old * 6364136223846793005 + self.inc) & _M64
        xs = (((old >> 18) ^ old) >> 27) & 0xFFFFFFFF
        rot = old >> 59
        return ((xs >> rot) | (xs << ((32 - rot) & 31))) & 0xFFFFFFFF

    def uniform(self, lo: float = 0.0, hi: float = 1.0) -> float:
        u = (self.next() >> 8) * (1.0 / 16777216.0)
        return lo + (hi - lo) * u


@dataclass
class Score:
    bpm: float = 128
    duration: float = 0
    drums: List[tuple] = field(default_factory=list)   # (time, kind, velocity)
    notes: List[tuple] = field(default_factory=list)   # (time, duration, midi, velocity, voice)
    beats: List[float] = field(default_factory=list)


def click_track(bpm: float, duration: float, offset: float = 0.5) -> Score:
    s = Score(bpm=bpm, duration=duration)
    p = 60.0 / bpm
    t = offset
    while t < duration - 0.2:
        s.drums.append((t, CLAP, 1.0))
        s.beats.append(t)
        t += p
    return s


def click_track_tempo_change(bpm1: float, bpm2: float, switch_time: float, duration: float) -> Score:
    s = Score(bpm=bpm1, duration=duration)
    t = 0.5
    while t < duration - 0.2:
        s.drums.append((t, CLAP, 1.0))
        s.beats.append(t)
        t += 60.0 / (bpm1 if t < switch_time else bpm2)
    return s


def drum_loop(bpm: float, bars: int, offset: float = 0.25) -> Score:
    s = Score(bpm=bpm)
    p = 60.0 / bpm
    s.duration = offset + bars * 4 * p + 0.5
    roots = [45, 41, 48, 43]
    for b in range(bars * 4):
        t = offset + b * p
        s.beats.append(t)
        if b % 4 == 0:
            s.notes.append((t, 4 * p * 0.95, float(roots[(b // 4) % 4]), 0.9, 1))
        s.drums.append((t, KICK, 1.0))
        if b % 2 == 1:
            s.drums.append((t, SNARE, 0.9))
        s.drums.append((t, HIHAT, 0.6))
        s.drums.append((t + p / 2, HIHAT, 0.5))
    return s


def render(score: Score, sr: int = 22050, seed: int = 1) -> np.ndarray:
    n = int(math.ceil(score.duration * sr)) + sr
    y = np.zeros(n, np.float32)
    rng = Rng(seed)
    for (time, kind, vel) in score.drums:
        s0 = int(max(0.0, time) * sr)
        if kind == KICK:
            ln = min(int(0.35 * sr), n - s0)
            t = np.arange(ln) / sr
            f = 45 + 110 * np.exp(-t * 28)
            phase = np.cumsum(_TAU * f / sr)
            env = np.exp(-t * 9.0) * np.minimum(1.0, t * sr / 8.0)
            y[s0:s0 + ln] += (0.9 * vel * env * np.sin(phase)).astype(np.float32)
        elif kind in (SNARE, CLAP):
            ln = int(0.22 * sr)
            lp = prev = 0.0
            for i in range(ln):
                if s0 + i >= n:
                    break
                t = i / sr
                noise = rng.uniform(-1, 1)
                hp = noise - prev
                prev = noise
                lp += 0.5 * (hp - lp)
                env = math.exp(-t * (30.0 if kind == CLAP else 18.0))
                tone = 0.35 * math.sin(_TAU * 190 * t) * math.exp(-t * 30) if kind == SNARE else 0.0
                y[s0 + i] += np.float32(0.5 * vel * (env * lp * 1.4 + tone))
        else:  # hi-hat
            ln = int(0.06 * sr)
            prev = prev2 = 0.0
            for i in range(ln):
                if s0 + i >= n:
                    break
                t = i / sr
                noise = rng.uniform(-1, 1)
                hp = noise - 2 * prev + prev2
                prev2, prev = prev, noise
                y[s0 + i] += np.float32(0.22 * vel * math.exp(-t * 70.0) * hp)
    for (time, dur, midi, vel, voice) in score.notes:
        s0 = int(max(0.0, time) * sr)
        f = 440.0 * 2.0 ** ((midi - 69.0) / 12.0)
        rel = 0.3 if voice == 2 else 0.06
        ln = min(int((dur + rel) * sr), n - s0)
        t = np.arange(ln) / sr
        att = 0.08 if voice == 2 else 0.004
        env = np.minimum(1.0, t / att)
        after = t > dur
        env = np.where(after, env * np.exp(-(t - dur) / (rel * 0.35)), env)
        if voice == 0:
            env = np.where(after, env, env * (0.75 + 0.25 * np.exp(-t * 6)))
        ph = np.cumsum(np.full(ln, _TAU * f / sr))
        if voice == 0:
            sig = np.sin(ph) + 0.5 * np.sin(2 * ph) + 0.33 * np.sin(3 * ph) + 0.2 * np.sin(4 * ph) + 0.12 * np.sin(5 * ph)
        elif voice == 1:
            sig = np.sin(ph) + 0.3 * np.sin(2 * ph)
        else:
            sig = np.sin(ph) + 0.25 * np.sin(2 * ph + 0.3) + 0.15 * np.sin(3 * ph)
        gain = 0.16 if voice == 0 else (0.28 if voice == 1 else 0.07)
        y[s0:s0 + ln] += (gain * vel * env * sig).astype(np.float32)
    peak = float(np.max(np.abs(y)))
    if peak > 0.98:
        y *= np.float32(0.98 / peak)
    return y[:int(score.duration * sr)].copy()


def wav_bytes(channels: List[np.ndarray], sr: int, bits: int = 16) -> bytes:
    """Interleaved PCM WAV (16 or 24 bit) from float channels in [-1, 1]."""
    x = np.stack([np.clip(np.asarray(c, np.float64), -1.0, 1.0) for c in channels], axis=1)
    if bits == 16:
        q = np.round(x * 32767.0).astype("<i2")
        data = q.tobytes()
    elif bits == 24:
        q = np.round(x * 8388607.0).astype("<i4").reshape(-1)
        b = q.view(np.uint8).reshape(-1, 4)[:, :3]
        data = b.tobytes()
    else:
        raise ValueError("bits must be 16 or 24")
    ch = len(channels)
    ba = ch * bits // 8
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, sr, sr * ba, ba, bits)
    hdr += b"data" + struct.pack("<I", len(data))
    return hdr + data
