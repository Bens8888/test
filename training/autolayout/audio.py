"""numpy mirror of the C++ music analysis (core/src/audio/{fft,analysis,decode}.cpp).

The training pipeline must compute exactly the features the mod computes at runtime, so this
module is a line-by-line port of ``al::audio::analyze`` with the same constants, the same
algorithm order and the same floating-point conventions:

* ``float`` in C++ is ``np.float32`` here and ``double`` is Python ``float`` / ``np.float64``.
  NumPy 2 treats Python scalars as "weak", so ``np.float32(x) * 0.5`` stays float32; every
  place where C++ promotes to double converts explicitly with ``float(...)``.
* Sequential C++ accumulations (``acc += ...``) use ``np.add.accumulate`` (strictly
  sequential) instead of ``np.sum`` (pairwise), so sums are bit-identical.
* The FFT is the same radix-2 Cooley-Tukey + real-input split as ``RealFft`` with the same
  float32 twiddles and the same (non-fused) complex arithmetic, vectorised across frames.
* ``log1pf`` is a vectorised port of glibc's ``s_log1pf.c`` (numpy's own float32 log1p is
  SIMD/SVML based and differs by 1 ulp on ~4% of inputs); scalar double transcendental
  functions go through ``math`` (glibc) rather than numpy's SIMD loops.

See training/README.md for the measured parity and the remaining (tiny) differences.

Public API: :func:`analyze`, :func:`analyze_file`, :func:`decode_wav`, :func:`resample` and the
dataclasses mirroring ``analysis.hpp``.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import List, Optional, Tuple

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

F32 = np.float32
F64 = np.float64

# ---------------------------------------------------------------------------------------------
# Types (analysis.hpp)


class Band(IntEnum):
    Bass = 0
    LowMid = 1
    Mid = 2
    High = 3


class EventType(IntEnum):
    Beat = 0
    Downbeat = 1
    Onset = 2
    Kick = 3
    Snare = 4
    HiHat = 5
    Note = 6
    Sustain = 7
    SectionStart = 8
    BuildUp = 9
    Drop = 10
    TempoChange = 11
    Silence = 12


EVENT_TYPE_NAMES = ["beat", "downbeat", "onset", "kick", "snare", "hihat", "note", "sustain",
                    "section", "buildup", "drop", "tempo", "silence"]


class SectionType(IntEnum):
    Intro = 0
    Verse = 1
    Chorus = 2
    BuildUp = 3
    Drop = 4
    Break = 5
    Outro = 6


SECTION_TYPE_NAMES = ["intro", "verse", "chorus", "buildup", "drop", "break", "outro"]


def event_type_name(t: EventType) -> str:
    return EVENT_TYPE_NAMES[int(t)]


def section_type_name(t: SectionType) -> str:
    return SECTION_TYPE_NAMES[int(t)]


@dataclass
class MusicEvent:
    time: float = 0.0            # seconds
    type: EventType = EventType.Onset
    strength: float = 0.0        # 0..1 (a float32 value)
    band: Band = Band.Mid
    pitch: float = 0.0           # MIDI (Note/Sustain)
    duration: float = 0.0        # seconds (Note/Sustain/BuildUp/Silence/SectionStart)
    index: int = -1              # beat index / section index


@dataclass
class TempoSegment:
    start: float = 0.0
    end: float = 0.0
    bpm: float = 120.0


@dataclass
class Section:
    start: float = 0.0
    end: float = 0.0
    type: SectionType = SectionType.Verse
    energy: float = 0.0
    power: float = 0.0
    density: float = 0.0
    trend: float = 0.0
    firstBar: int = 0
    barCount: int = 0

    def duration(self) -> float:
        return self.end - self.start


@dataclass
class AnalysisOptions:
    analysisRate: int = 22050
    minBpm: float = 60.0
    maxBpm: float = 220.0
    preferredBpm: float = 130.0
    sustainMin: float = 0.30
    beatsPerBar: int = 0
    fineOnsets: bool = True


def _empty_f32() -> np.ndarray:
    return np.zeros(0, F32)


@dataclass
class MusicAnalysis:
    duration: float = 0.0
    hop: float = 0.0
    hopCoarse: float = 0.0
    # fine frame curves (hop)
    rmsDb: np.ndarray = field(default_factory=_empty_f32)
    onsetEnv: np.ndarray = field(default_factory=_empty_f32)
    bandOnset: List[np.ndarray] = field(default_factory=lambda: [_empty_f32() for _ in range(4)])
    intensity: np.ndarray = field(default_factory=_empty_f32)
    # coarse frame curves (hopCoarse)
    percRatio: np.ndarray = field(default_factory=_empty_f32)
    bassPercRatio: np.ndarray = field(default_factory=_empty_f32)
    pitchMidi: np.ndarray = field(default_factory=_empty_f32)
    pitchConf: np.ndarray = field(default_factory=_empty_f32)
    chroma: np.ndarray = field(default_factory=lambda: np.zeros((0, 12), F32))
    beats: List[float] = field(default_factory=list)
    beatStrength: List[float] = field(default_factory=list)
    beatInBar: List[int] = field(default_factory=list)
    downbeats: List[float] = field(default_factory=list)
    beatsPerBar: int = 4
    tempo: List[TempoSegment] = field(default_factory=list)
    sections: List[Section] = field(default_factory=list)
    events: List[MusicEvent] = field(default_factory=list)
    waveformMin: np.ndarray = field(default_factory=_empty_f32)
    waveformMax: np.ndarray = field(default_factory=_empty_f32)

    # -- accessors (MusicAnalysis methods in analysis.cpp)
    def mainBpm(self) -> float:
        if not self.tempo:
            return 120.0
        best, bpm = 0.0, self.tempo[0].bpm
        for s in self.tempo:
            if s.end - s.start > best:
                best, bpm = s.end - s.start, s.bpm
        return bpm

    def intensityAt(self, t: float) -> float:
        return _sample_curve(self.intensity, t, self.hop)

    def onsetAt(self, t: float) -> float:
        return _sample_curve(self.onsetEnv, t, self.hop)

    def pitchAt(self, t: float) -> Tuple[float, float]:
        """Returns (midi, confidence)."""
        if len(self.pitchMidi) == 0:
            return 0.0, 0.0
        i = int(min(max(t / self.hopCoarse, 0.0), float(len(self.pitchMidi) - 1)))
        return float(self.pitchMidi[i]), float(self.pitchConf[i])

    def sectionAt(self, t: float) -> int:
        for i, s in enumerate(self.sections):
            if t < s.end:
                return i
        return len(self.sections) - 1 if self.sections else -1

    def beatPeriodAt(self, t: float) -> float:
        b = self.beats
        if len(b) >= 2:
            i = int(np.searchsorted(b, t, side="right"))
            i = max(i, 1)
            i = min(i, len(b) - 1)
            return b[i] - b[i - 1]
        for s in self.tempo:
            if t < s.end:
                return 60.0 / s.bpm
        return 0.5

    def snapToGrid(self, t: float, div: int) -> float:
        b = self.beats
        if len(b) < 2 or div <= 0:
            return t
        i = int(np.searchsorted(b, t, side="right"))
        if i == 0:
            b1 = b[0]
            b0 = b1 - (b[1] - b[0])
        elif i >= len(b):
            b0 = b[-1]
            b1 = b0 + (b[-1] - b[-2])
        else:
            b0, b1 = b[i - 1], b[i]
        step = (b1 - b0) / div
        k = _cround((t - b0) / step)
        return b0 + k * step

    def eventsIn(self, t0: float, t1: float, type: EventType) -> List[MusicEvent]:
        return [e for e in self.events if t0 <= e.time < t1 and e.type == type]


def _sample_curve(c: np.ndarray, t: float, hop: float) -> float:
    if len(c) == 0 or hop <= 0:
        return 0.0
    f = t / hop
    if f <= 0:
        return float(c[0])
    i = int(f)
    if i + 1 >= len(c):
        return float(c[-1])
    fr = f - i
    return float(F32(float(c[i]) * (1 - fr) + float(c[i + 1]) * fr))


# ---------------------------------------------------------------------------------------------
# Small helpers mirroring C/C++ library semantics


def _cround(x: float) -> int:
    """std::round / std::lround: half away from zero (exact for all doubles)."""
    if x >= 0:
        r = math.floor(x)
        return int(r + 1) if x - r >= 0.5 else int(r)
    return -_cround(-x)


def _seqsum(a: np.ndarray, dtype=F64):
    """Strictly sequential sum (C++ `acc += a[i]` from 0) in the given accumulator type."""
    a = np.asarray(a)
    if a.size == 0:
        return dtype(0)
    return np.add.accumulate(a.astype(dtype, copy=False))[-1]


def _seqsum_rows(a: np.ndarray, dtype=F64) -> np.ndarray:
    """Sequential sum along the last axis of a 2-D array."""
    if a.shape[-1] == 0:
        return np.zeros(a.shape[:-1], dtype)
    return np.add.accumulate(a.astype(dtype, copy=False), axis=-1)[..., -1]


def _percentile(v, p: float):
    """percentile() in analysis.cpp: the k-th smallest with k = size_t(p * (n - 1))."""
    v = np.asarray(v, dtype=F32)
    if v.size == 0:
        return F32(0)
    k = int(min(max(p, 0.0), 1.0) * float(v.size - 1))
    return F32(np.partition(v, k)[k])


def _moving_average(x: np.ndarray, half: int) -> np.ndarray:
    n = len(x)
    if n == 0:
        return np.zeros(0, F32)
    pre = np.zeros(n + 1, F64)
    pre[1:] = np.add.accumulate(x.astype(F64))
    i = np.arange(n)
    a = np.maximum(i - half, 0)
    b = np.minimum(n, i + half + 1)
    return ((pre[b] - pre[a]) / (b - a).astype(F64)).astype(F32)


def _log_list(vals) -> List[float]:
    lg = math.log
    return [lg(v) for v in vals]


_LP = [F32(v) for v in (6.6666668653e-01, 4.0000000596e-01, 2.8571429849e-01, 2.2222198546e-01,
                        1.8183572590e-01, 1.5313838422e-01, 1.4798198640e-01)]


def log1pf(x: np.ndarray) -> np.ndarray:
    """Vectorised port of glibc's float log1pf (sysdeps/ieee754/flt-32/s_log1pf.c) for finite
    x >= 0. Bit-identical to the libm used by the C++ build (verified on 2M random inputs)."""
    x = np.ascontiguousarray(x, dtype=F32)
    shape = x.shape
    x = x.reshape(-1)
    hx = x.view(np.int32)
    out = np.empty_like(x)
    ln2_hi = F32(6.9313812256e-01)
    ln2_lo = F32(9.0580006145e-06)
    tiny = hx < 0x31000000
    t1 = hx < 0x24800000
    out[tiny & t1] = x[tiny & t1]
    m = tiny & ~t1
    xm = x[m]
    out[m] = xm - xm * xm * F32(0.5)
    big = hx >= 0x3ed413d7
    small = (~tiny) & (~big)
    with np.errstate(all="ignore"):
        xb = x[big]
        hb = hx[big]
        u = F32(1.0) + xb
        hu = u.view(np.int32).copy()
        k = (hu >> 23) - 127
        c = np.where(k > 0, F32(1.0) - (u - xb), xb - (u - F32(1.0))).astype(F32)
        c = c / u
        huge = hb >= 0x5a000000
        hu = np.where(huge, hb, hu)
        k = np.where(huge, (hb >> 23) - 127, k)
        c = np.where(huge, F32(0), c).astype(F32)
        hu = hu & 0x007fffff
        lowm = hu < 0x3504f7
        uu = np.where(lowm, hu | 0x3f800000, hu | 0x3f000000).astype(np.int32).view(F32)
        k = np.where(lowm, k, k + 1)
        hu = np.where(lowm, hu, (0x00800000 - hu) >> 2)
        fb = uu - F32(1.0)
        n = x.size
        Fv = np.zeros(n, F32)
        K = np.zeros(n, np.int32)
        C = np.zeros(n, F32)
        HU = np.ones(n, np.int32)
        Fv[small] = x[small]
        Fv[big] = fb
        K[big] = k
        C[big] = c
        HU[big] = hu
        sel = ~tiny
        f = Fv[sel]
        k = K[sel]
        c = C[sel]
        hu = HU[sel]
        kf = k.astype(F32)
        hfsq = F32(0.5) * f * f
        s = f / (F32(2.0) + f)
        z = s * s
        R = z * (_LP[0] + z * (_LP[1] + z * (_LP[2] + z * (_LP[3] + z * (_LP[4] + z * (_LP[5] + z * _LP[6]))))))
        r_k0 = f - (hfsq - s * (hfsq + R))
        r_k = kf * ln2_hi - ((hfsq - (s * (hfsq + R) + (kf * ln2_lo + c))) - f)
        res = np.where(k == 0, r_k0, r_k).astype(F32)
        h0 = hu == 0
        if h0.any():
            f0, k0, c0, hf0 = f[h0], kf[h0], c[h0], hfsq[h0]
            R0 = hf0 * (F32(1.0) - F32(0.66666666666666666) * f0)
            r = np.where(k[h0] == 0, f0 - R0, k0 * ln2_hi - ((R0 - (k0 * ln2_lo + c0)) - f0))
            r = np.where(f0 == 0, np.where(k[h0] == 0, F32(0), k0 * ln2_hi + (c0 + k0 * ln2_lo)), r)
            res[h0] = r
        out[sel] = res
    return out.reshape(shape)


# ---------------------------------------------------------------------------------------------
# FFT (fft.cpp)


def hann_window(n: int) -> np.ndarray:
    return np.array([0.5 - 0.5 * math.cos(2.0 * math.pi * i / n) for i in range(n)], dtype=F32)


class RealFft:
    """Mirror of al::audio::RealFft: packs the real input into a half-size complex FFT
    (iterative radix-2, float32 twiddles) and splits the spectrum. Vectorised over frames."""

    def __init__(self, n: int):
        if n < 4 or n & (n - 1):
            raise ValueError("RealFft size must be a power of two >= 4")
        self.n = n
        h = n // 2
        self.h = h
        # Fft(h) twiddles: a = -2 pi k / h, k < h/2
        tw = [(-2.0 * math.pi * k) / h for k in range(h // 2)]
        self.twr = np.array([math.cos(a) for a in tw], dtype=F32)
        self.twi = np.array([math.sin(a) for a in tw], dtype=F32)
        bits = 0
        while (1 << bits) < h:
            bits += 1
        rev = np.zeros(h, np.int64)
        for i in range(h):
            r = 0
            for b in range(bits):
                if i & (1 << b):
                    r |= 1 << (bits - 1 - b)
            rev[i] = r
        self.rev = rev
        # post-processing twiddles: a = -2 pi k / n, k < h; W[h] = -1
        w = [(-2.0 * math.pi * k) / n for k in range(h)]
        self.wr = np.array([math.cos(a) for a in w] + [-1.0], dtype=F32)
        self.wi = np.array([math.sin(a) for a in w] + [0.0], dtype=F32)
        kk = np.arange(h + 1)
        self.idx_k = kk % h
        self.idx_c = (h - kk) % h

    def forward(self, frames: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """frames: (F, n) float32 -> (re, im), each (F, n/2+1) float32."""
        h = self.h
        nf = frames.shape[0]
        zr = np.ascontiguousarray(frames[:, 0::2][:, self.rev])
        zi = np.ascontiguousarray(frames[:, 1::2][:, self.rev])
        ln = 2
        while ln <= h:
            half = ln >> 1
            step = h // ln
            wr = self.twr[0:half * step:step]
            wi = self.twi[0:half * step:step]
            zr4 = zr.reshape(nf, h // ln, 2, half)
            zi4 = zi.reshape(nf, h // ln, 2, half)
            ur, ui = zr4[:, :, 0, :], zi4[:, :, 0, :]
            ar, ai = zr4[:, :, 1, :], zi4[:, :, 1, :]
            # v = a * w (complex<float>: re = ar*wr - ai*wi, im = ar*wi + ai*wr, no FMA)
            vr = ar * wr
            vr -= ai * wi
            vi = ar * wi
            vi += ai * wr
            # butterfly: a[i+j] = u + v, a[i+j+half] = u - v (in place, a already consumed)
            np.subtract(ur, vr, out=ar)
            np.subtract(ui, vi, out=ai)
            np.add(ur, vr, out=ur)
            np.add(ui, vi, out=ui)
            ln <<= 1
        zkr, zki = zr[:, self.idx_k], zi[:, self.idx_k]
        zcr, zci = zr[:, self.idx_c], -zi[:, self.idx_c]
        er = (zkr + zcr) * F32(0.5)
        ei = (zki + zci) * F32(0.5)
        dr = zkr - zcr
        di = zki - zci
        # (dr + i di) * (0 - 0.5i) = (0.5 di, -0.5 dr), exact scalings
        orr = di * F32(0.5)
        oi = -(dr * F32(0.5))
        outr = er + (self.wr * orr - self.wi * oi)
        outi = ei + (self.wr * oi + self.wi * orr)
        return outr, outi

    def magnitude(self, frames: np.ndarray) -> np.ndarray:
        re, im = self.forward(frames)
        return np.hypot(re, im)


# ---------------------------------------------------------------------------------------------
# Decoding / resampling (decode.cpp)


def decode_wav(data: bytes) -> Tuple[np.ndarray, int]:
    """Mirror of decodeWav(): PCM 8/16/24/32-bit and float 32/64 WAV (incl. WAVE_FORMAT_EXTENSIBLE)
    to mono float32 by averaging channels. Returns (samples, sampleRate)."""
    n = len(data)
    if n < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    off = 12
    channels = rate = bits = fmt = 0
    pcm: Optional[bytes] = None
    while off + 8 <= n:
        ln = struct.unpack_from("<I", data, off + 4)[0]
        body = off + 8
        avail = min(ln, n - off - 8)
        cid = data[off:off + 4]
        if cid == b"fmt " and avail >= 16:
            fmt, channels, rate = struct.unpack_from("<HHI", data, body)
            bits = struct.unpack_from("<H", data, body + 14)[0]
            if fmt == 0xFFFE and avail >= 26:
                fmt = struct.unpack_from("<H", data, body + 24)[0]
        elif cid == b"data":
            pcm = data[body:body + avail]
        off += 8 + ln + (ln & 1)
    if pcm is None or channels <= 0 or rate <= 0:
        raise ValueError("corrupt WAV")
    bpb = bits // 8
    if bpb <= 0:
        raise ValueError("corrupt WAV")
    frames = len(pcm) // (bpb * channels)
    raw = np.frombuffer(pcm, dtype=np.uint8, count=frames * bpb * channels)
    if fmt == 3 and bits == 32:
        v = raw.view("<f4").astype(F32)
    elif fmt == 3 and bits == 64:
        v = raw.view("<f8").astype(F32)
    elif bits == 8:
        v = (raw.astype(np.int32) - 128).astype(F32) / F32(128.0)
    elif bits == 16:
        v = raw.view("<i2").astype(F32) / F32(32768.0)
    elif bits == 24:
        b3 = raw.reshape(-1, 3).astype(np.int32)
        xi = (b3[:, 0] << 8) | (b3[:, 1] << 16) | (b3[:, 2] << 24)
        v = (xi >> 8).astype(F32) / F32(8388608.0)
    elif bits == 32:
        v = (raw.view("<i4").astype(F64) / 2147483648.0).astype(F32)
    else:
        raise ValueError(f"unsupported WAV sample format (fmt {fmt}, {bits} bits)")
    v = v.reshape(frames, channels)
    acc = np.zeros(frames, F32)
    for c in range(channels):
        acc = acc + v[:, c]
    return (acc / F32(channels)).astype(F32), int(rate)


def _bessel_i0(x: float) -> float:
    s = 1.0
    term = 1.0
    for k in range(1, 30):
        term *= (x / (2 * k)) * (x / (2 * k))
        s += term
        if term < 1e-12 * s:
            break
    return s


def resample(samples: np.ndarray, in_rate: int, target_rate: int) -> np.ndarray:
    """Mirror of resample(): Kaiser-windowed sinc with a 64-phase linearly interpolated table."""
    x = np.asarray(samples, dtype=F32)
    if in_rate <= 0 or x.size == 0:
        return np.zeros(0, F32)
    if in_rate == target_rate:
        return x.copy()
    ratio = float(target_rate) / in_rate
    cutoff = min(1.0, ratio) * 0.95
    half = 16
    beta = 8.0
    i0b = _bessel_i0(beta)
    scale = min(1.0, ratio)
    taps = int(math.ceil(half / scale))
    phases = 64
    tsize = (2 * taps + 1) * phases + 1
    table = np.zeros(tsize, F32)
    for i in range(tsize):
        xx = float(i) / phases - taps
        t = xx * cutoff
        sinc = 1.0 if abs(t) < 1e-9 else math.sin(3.14159265358979 * t) / (3.14159265358979 * t)
        r = xx / taps
        w = 0.0 if abs(r) >= 1.0 else _bessel_i0(beta * math.sqrt(1 - r * r)) / i0b
        table[i] = sinc * w * cutoff
    out_n = int(math.floor(float(x.size) * ratio))
    out = np.zeros(out_n, F32)
    in_n = x.size
    chunk = 1 << 18
    for o0 in range(0, out_n, chunk):
        o = np.arange(o0, min(out_n, o0 + chunk), dtype=F64)
        center = o / ratio
        c = np.floor(center)
        frac = center - c
        ci = c.astype(np.int64)
        acc = np.zeros(o.size, F64)
        for k in range(-taps, taps + 1):
            idx = ci + k
            xx = (float(k) - frac) + taps
            pos = xx * phases
            pi = np.trunc(pos).astype(np.int64)
            ok = (idx >= 0) & (idx < in_n) & (pi >= 0) & (pi + 1 < tsize)
            pic = np.clip(pi, 0, tsize - 2)
            a = table[pic]
            b = table[pic + 1]
            wgt = a + (b - a) * (pos - pi).astype(F32)
            s = x[np.clip(idx, 0, in_n - 1)]
            acc = acc + np.where(ok, (s * wgt).astype(F64), 0.0)
        out[o0:o0 + o.size] = acc.astype(F32)
    return out


def encode_wav16(samples: np.ndarray, sample_rate: int) -> bytes:
    """Mirror of encodeWav16()."""
    s = np.clip(np.asarray(samples, dtype=F32), F32(-1), F32(1)) * F32(32767.0)
    q = np.where(s >= 0, np.floor(s.astype(F64) + 0.5), -np.floor(-s.astype(F64) + 0.5)).astype("<i2")
    data = q.tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, sample_rate * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    return hdr + data


# ---------------------------------------------------------------------------------------------
# Analysis (analysis.cpp)

K_FINE_N = 1024
K_FINE_HOP = 128
K_COARSE_N = 2048
K_COARSE_HOP = 512


def _sliding_median(a: np.ndarray, half: int) -> np.ndarray:
    """slidingMedian() along the last axis of a 2-D float32 array: element size/2 (upper median)
    of the window [i-half, i+half] clipped to the array."""
    rows, n = a.shape
    out = np.empty_like(a)
    w = 2 * half + 1
    if n >= w:
        win = sliding_window_view(a, w, axis=1)  # (rows, n-w+1, w)
        step = max(1, (1 << 22) // max(1, (n - w + 1) * w))
        for r0 in range(0, rows, step):
            part = np.partition(win[r0:r0 + step], half, axis=-1)
            out[r0:r0 + step, half:n - half] = part[..., half]
    edge = [i for i in range(n) if i < half or i + half >= n]
    for i in edge:
        lo = max(0, i - half)
        hi = min(n, i + half + 1)
        m = (hi - lo) // 2
        out[:, i] = np.partition(a[:, lo:hi], m, axis=1)[:, m]
    return out


class _Peak:
    __slots__ = ("frame", "value", "refined")

    def __init__(self, frame: int, value, refined: float):
        self.frame = frame
        self.value = value
        self.refined = refined


def _pick_peaks(env: np.ndarray, preMax: int, postMax: int, preAvg: int, postAvg: int,
                delta: float, minSpacing: int, absMin: float) -> List[_Peak]:
    n = len(env)
    peaks: List[_Peak] = []
    if n < 3:
        return peaks
    pre = np.zeros(n + 1, F64)
    pre[1:] = np.add.accumulate(env.astype(F64))
    i = np.arange(1, n - 1)
    v = env[1:n - 1]
    ok = v >= F32(absMin)
    for d in range(1, preMax + 1):
        j = i - d
        valid = j >= 0
        ok &= ~valid | (env[np.maximum(j, 0)] < v)
    for d in range(1, postMax + 1):
        j = i + d
        valid = j < n
        ok &= ~valid | (env[np.minimum(j, n - 1)] <= v)
    aa = np.maximum(i - preAvg, 0)
    bb = np.minimum(n, i + postAvg + 1)
    mean = (pre[bb] - pre[aa]) / (bb - aa).astype(F64)
    ok &= ~(v.astype(F64) < mean + float(F32(delta)))
    def refined_pos(ii):
        # parabolic interpolation of the peak position
        y0, y1, y2 = float(env[ii - 1]), float(env[ii]), float(env[ii + 1])
        den = y0 - 2 * y1 + y2
        off = 0.5 * (y0 - y2) / den if abs(den) > 1e-12 else 0.0
        return float(ii) + min(max(off, -0.5), 0.5)

    last = -1000000
    for ii in i[ok].tolist():
        val = env[ii]
        if ii - last < minSpacing:
            if peaks and val > peaks[-1].value:
                peaks[-1] = _Peak(ii, val, refined_pos(ii))
                last = ii
            continue
        peaks.append(_Peak(ii, val, refined_pos(ii)))
        last = ii
    return peaks


class _Refiner:
    """refineOnset(): steepest rise of the log energy of a gently high-passed signal."""

    def __init__(self, x: np.ndarray, sr: int):
        self.n = len(x)
        self.sr = sr
        xd = x.astype(F64)
        hp = np.zeros_like(xd)
        hp[1:] = xd[1:] - xd[:-1]
        self.term = hp * hp + 0.25 * xd * xd   # term[0] is never used (a >= 1)
        self.ar = np.arange(64)

    def __call__(self, t: float, before: float = 0.010, after: float = 0.030) -> float:
        return self.find(t, before, after)[0]

    def find(self, t: float, before: float = 0.010, after: float = 0.030) -> Tuple[float, bool]:
        """Returns (time, found); time is t unchanged when no clear attack was found."""
        win, step = 64, 16
        sr = self.sr
        c = int(t * sr)
        a = c - int(before * sr) - win
        b = c + int(after * sr) + win
        if a < 1 or b >= self.n:
            return t, False
        starts = np.arange(a, b - win, step)
        if len(starts) < 3:
            return t, False
        e = _seqsum_rows(self.term[starts[:, None] + self.ar])
        env = _log_list((e + 1e-10).tolist())
        best = 0.0
        bi = 0
        for i in range(1, len(env)):
            d = env[i] - env[i - 1]
            if d > best:
                best = d
                bi = i
        if best < 0.8:
            return t, False
        onset_sample = float(int(starts[bi]) + win) - step * 0.5
        return onset_sample / sr, True


def _bpm_prior(bpm: float, pref: float) -> float:
    l = math.log2(bpm / pref)
    return float(F32(math.exp(-0.5 * l * l / (0.9 * 0.9))))


def _min1(v) -> F32:
    return F32(min(F32(1.0), F32(v)))


def analyze(samples: np.ndarray, sample_rate: int, options: Optional[AnalysisOptions] = None) -> MusicAnalysis:
    """Analyse mono audio (float, [-1, 1]); mirror of al::audio::analyze()."""
    opt = options or AnalysisOptions()
    A = MusicAnalysis()
    x = np.asarray(samples, dtype=F32)
    if sample_rate != opt.analysisRate:
        x = resample(x, sample_rate, opt.analysisRate)
    else:
        x = x.copy()
    sr = opt.analysisRate
    N = len(x)
    A.duration = float(N) / sr if sr > 0 else 0.0
    A.hop = float(K_FINE_HOP) / sr
    A.hopCoarse = float(K_COARSE_HOP) / sr
    if N < K_COARSE_N * 2:
        A.sections.append(Section(0, A.duration, SectionType.Verse, 0.5, 0, 0, 0, 0, 0))
        A.tempo.append(TempoSegment(0, A.duration, 120))
        return A
    peak = F32(np.max(np.abs(x)))
    if peak > F32(1e-6):
        x = x / peak

    # ------------------------------------------------------------ waveform preview
    bins = 4096
    b_idx = np.arange(bins + 1, dtype=np.int64) * N // bins
    wmin = np.zeros(bins, F32)
    wmax = np.zeros(bins, F32)
    nonempty = b_idx[1:] > b_idx[:-1]
    starts = b_idx[:-1][nonempty]
    wmin[nonempty] = np.minimum(np.minimum.reduceat(x, starts), F32(0))
    wmax[nonempty] = np.maximum(np.maximum.reduceat(x, starts), F32(0))
    A.waveformMin, A.waveformMax = wmin, wmax

    # ------------------------------------------------------------ fine pass (flux per band, RMS)
    nFine = N // K_FINE_HOP + 1
    binHzFine = float(sr) / K_FINE_N
    bandEdges = [int(30 / binHzFine), int(150 / binHzFine), int(500 / binHzFine), int(3000 / binHzFine),
                 min(K_FINE_N // 2, int(11000 / binHzFine))]
    fft = RealFft(K_FINE_N)
    win = hann_window(K_FINE_N)
    padded = np.zeros(K_FINE_N // 2 + N + K_FINE_N, F32)
    padded[K_FINE_N // 2:K_FINE_N // 2 + N] = x
    view = sliding_window_view(padded, K_FINE_N)[::K_FINE_HOP][:nFine]
    nb = K_FINE_N // 2 + 1
    logMag = np.zeros((nFine, nb), F32)
    sumSq = np.zeros(nFine, F64)
    r0, r1 = K_FINE_N // 2 - K_FINE_HOP // 2, K_FINE_N // 2 + K_FINE_HOP // 2
    CH = 128  # frames per FFT batch (small batches stay in cache)
    for f0 in range(0, nFine, CH):
        fr = np.ascontiguousarray(view[f0:f0 + CH])
        s64 = fr[:, r0:r1].astype(F64)
        sumSq[f0:f0 + CH] = _seqsum_rows(s64 * s64)
        mag = fft.magnitude(fr * win)
        logMag[f0:f0 + CH, 1:] = log1pf(F32(100.0) * mag[:, 1:])
    A.rmsDb = np.array([10.0 * math.log10(v / K_FINE_HOP + 1e-12) for v in sumSq.tolist()], dtype=F32)
    d = np.empty_like(logMag)
    d[0] = logMag[0]
    d[1:] = logMag[1:] - logMag[:-1]
    dpos = np.where(d > 0, d, F32(0)).astype(F32)
    onset = np.add.accumulate(dpos[:, 1:], axis=1)[:, -1]
    bandOnset = []
    for b in range(4):
        lo = max(1, bandEdges[b])
        hi = bandEdges[b + 1]
        if hi > lo:
            bandOnset.append(np.add.accumulate(dpos[:, lo:hi], axis=1)[:, -1].astype(F32))
        else:
            bandOnset.append(np.zeros(nFine, F32))
    del d, dpos, logMag

    def norm(v):
        p = _percentile(v, 0.99)
        if p <= F32(1e-9):
            return v.astype(F32)
        return np.minimum(F32(4.0), v / p).astype(F32)

    A.onsetEnv = norm(onset.astype(F32))
    A.bandOnset = [norm(b) for b in bandOnset]

    # ------------------------------------------------------------ coarse pass: HPSS, chroma, pitch
    nCoarse = N // K_COARSE_HOP + 1
    nBins = K_COARSE_N // 2 + 1
    binHz = float(sr) / K_COARSE_N
    fftc = RealFft(K_COARSE_N)
    winc = hann_window(K_COARSE_N)
    paddedc = np.zeros(K_COARSE_N // 2 + N + K_COARSE_N, F32)
    paddedc[K_COARSE_N // 2:K_COARSE_N // 2 + N] = x
    viewc = sliding_window_view(paddedc, K_COARSE_N)[::K_COARSE_HOP][:nCoarse]
    spec = np.zeros((nCoarse, nBins), F32)
    for f0 in range(0, nCoarse, 64):
        spec[f0:f0 + 64] = fftc.magnitude(np.ascontiguousarray(viewc[f0:f0 + 64]) * winc)

    maxBin = min(nBins, int(8000 / binHz))
    harm = np.zeros((nCoarse, nBins), F32)
    perc = spec.copy()  # bins >= maxBin: harm 0, perc = spec
    harm[:, :maxBin] = _sliding_median(np.ascontiguousarray(spec[:, :maxBin].T), 8).T
    perc[:, :maxBin] = _sliding_median(np.ascontiguousarray(spec[:, :maxBin]), 8)
    h2 = harm * harm
    p2 = perc * perc
    den = h2 + p2
    with np.errstate(all="ignore"):
        mh = np.where(den > F32(1e-20), h2 / den, F32(0.5)).astype(F32)
    harm = spec * mh
    perc = spec * (F32(1) - mh)
    del h2, p2, den, mh
    hh64 = harm.astype(F64)
    hh64 *= hh64
    pp64 = perc.astype(F64)
    pp64 *= pp64
    eh = _seqsum_rows(hh64)
    ep = _seqsum_rows(pp64)
    bassLo, bassHi = int(30 / binHz), int(150 / binHz) + 1
    midLo, midHi = int(150 / binHz) + 1, int(3000 / binHz) + 1
    bh = _seqsum_rows(hh64[:, bassLo:bassHi])
    bp = _seqsum_rows(pp64[:, bassLo:bassHi])
    del hh64, pp64
    A.percRatio = (ep / (eh + ep + 1e-12)).astype(F32)
    A.bassPercRatio = (bp / (bh + bp + 1e-12)).astype(F32)
    pe32 = perc * perc
    percBandE = [_seqsum_rows(pe32[:, bassLo:bassHi], F32),
                 _seqsum_rows(pe32[:, midLo:midHi], F32),
                 _seqsum_rows(pe32[:, max(midHi, 0):], F32)]
    del pe32
    for i in range(3):
        band = np.sqrt(percBandE[i]).astype(F32)
        p = _percentile(band, 0.97)
        if p > F32(1e-12):
            band = (band / p).astype(F32)
        percBandE[i] = band
    del spec

    # Chroma from the harmonic part.
    pc = np.full(nBins, -1, np.int64)
    for k in range(1, nBins):
        hz = k * binHz
        if hz < 60 or hz > 5000:
            continue
        midi = 69 + 12 * math.log2(hz / 440.0)
        pc[k] = _cround(midi) % 12
    chroma = np.zeros((nCoarse, 12), F32)
    for c in range(12):
        ks = np.nonzero(pc == c)[0]
        if len(ks):
            chroma[:, c] = _seqsum_rows(harm[:, ks], F32)
    m = chroma.max(axis=1)
    ok = m > F32(1e-9)
    chroma[ok] = chroma[ok] / m[ok, None]
    A.chroma = chroma

    # Melody pitch: harmonic summation salience + Viterbi smoothing.
    midiLo, midiHi, stepSemi = F32(40), F32(88), F32(0.2)
    nCand = int((midiHi - midiLo) / stepSemi) + 1
    mag = np.sqrt(harm).astype(F64)
    frameEnergy = eh.astype(F32)
    s = np.zeros((nCoarse, nCand), F64)
    f0s = [440.0 * math.pow(2.0, float((midiLo + F32(c) * stepSemi) - F32(69)) / 12.0) for c in range(nCand)]
    w = 1.0
    for h in range(1, 9):
        B = np.zeros(nCand, np.int64)
        FR = np.zeros(nCand, F64)
        valid = np.zeros(nCand, bool)
        for c, f0 in enumerate(f0s):
            bin_ = h * f0 / binHz
            # the C++ loop breaks at the first invalid harmonic; both conditions are monotone in h
            if bin_ >= float(nBins - 1) or h * f0 > 5000:
                continue
            ok_prev = True
            for hh in range(1, h):
                if hh * f0 / binHz >= float(nBins - 1) or hh * f0 > 5000:
                    ok_prev = False
            if not ok_prev:
                continue
            B[c] = int(bin_)
            FR[c] = bin_ - float(B[c])
            valid[c] = True
        if valid.any():
            vc = np.nonzero(valid)[0]
            mb = mag[:, B[vc]]
            mb1 = mag[:, B[vc] + 1]
            fr_ = FR[vc]
            v = np.maximum(np.maximum(mb * (1 - fr_) + mb1 * fr_, mb), mb1)
            s[:, vc] = s[:, vc] + w * v
        w *= 0.8
    sal = s.astype(F32)
    del s, mag
    W = int(F32(5) / stepSemi)
    mx = np.maximum(F32(1e-9), sal.max(axis=1))
    salNorm = (sal / mx[:, None]).astype(F32)
    cost = (-salNorm[0]).astype(F32)
    back = np.zeros((nCoarse, nCand), np.int64)
    padc = np.full(nCand + 2 * W, np.inf, F32)
    gidx = np.arange(nCand)[:, None] + np.arange(2 * W + 1)[None, :]
    pen = (F32(0.02) * np.abs(np.arange(-W, W + 1)).astype(F32)).astype(F32)
    rowsC = np.arange(nCand)
    jumpCost = F32(0.6)
    for f in range(1, nCoarse):
        bestPrev = int(np.argmin(cost))
        bc0 = cost[bestPrev] + jumpCost
        padc[W:W + nCand] = cost
        vals = padc[gidx] + pen
        j = np.argmin(vals, axis=1)
        mv = vals[rowsC, j]
        take = mv < bc0
        bc = np.where(take, mv, bc0)
        back[f] = np.where(take, j + rowsC - W, bestPrev)
        nxt = (bc - salNorm[f]).astype(F32)
        cost = (nxt - nxt.min()).astype(F32)
    path = np.zeros(nCoarse, np.int64)
    path[-1] = int(np.argmin(cost))
    for f in range(nCoarse - 1, 0, -1):
        path[f - 1] = back[f, path[f]]
    eMax = frameEnergy.max()
    mean = _seqsum_rows(sal) / nCand
    ratio = (sal[np.arange(nCoarse), path].astype(F64) / (mean + 1e-9)).astype(F32)
    rLo, rHi = _percentile(ratio, 0.1), _percentile(ratio, 0.95)
    conf = np.clip((ratio - rLo) / max(F32(1e-6), F32(rHi - rLo)), F32(0), F32(1)).astype(F32)
    loud = frameEnergy > eMax * F32(1e-4)
    A.pitchConf = np.where(loud, conf, F32(0)).astype(F32)
    A.pitchMidi = np.where(loud & (conf > F32(0.35)), midiLo + path.astype(F32) * stepSemi, F32(0)).astype(F32)
    del harm, perc, sal, salNorm, back

    # ------------------------------------------------------------ tempo (local autocorrelation)
    hop = A.hop
    oenv = A.onsetEnv.copy()
    avg = _moving_average(oenv, int(0.4 / hop))
    oenv = np.maximum(F32(0), oenv - avg).astype(F32)
    o64 = oenv.astype(F64)
    lagMin = int(math.floor(60.0 / float(F32(opt.maxBpm)) / hop))
    lagMax = int(math.ceil(60.0 / float(F32(opt.minBpm)) / hop))
    winFrames, winStep = int(8.0 / hop), int(2.0 / hop)
    localAcf: List[np.ndarray] = []
    localCenter: List[float] = []
    s0 = 0
    while s0 + 4 < nFine:
        e0 = min(nFine, s0 + winFrames)
        if e0 - s0 < lagMax * 2:
            if localAcf:
                break
            e0 = min(nFine, s0 + lagMax * 2 + 2)
        acf = np.zeros(lagMax + 1, F32)
        seg = o64[s0:e0]
        L = e0 - s0
        for lag in range(lagMin // 2, lagMax + 1):
            if L - lag > 0:
                acc = np.add.accumulate(seg[:L - lag] * seg[lag:])[-1]
            else:
                acc = 0.0
            acf[lag] = acc / float(max(1, e0 - s0 - lag))
        localAcf.append(acf)
        localCenter.append((s0 + e0) * 0.5 * hop)
        if e0 >= nFine:
            break
        s0 += winStep
    pref = float(F32(opt.preferredBpm))

    def best_lag(acf):
        best = -1.0
        bl = lagMin
        a = acf.tolist()
        for lag in range(lagMin, lagMax + 1):
            bpm = 60.0 / (lag * hop)
            sc = a[lag]
            if 2 * lag <= lagMax:
                sc += 0.5 * a[2 * lag]
            if lag // 2 >= lagMin // 2:
                sc += 0.25 * a[lag // 2]
            sc *= _bpm_prior(bpm, pref)
            if sc > best:
                best = sc
                bl = lag
        y0, y1, y2 = a[max(1, bl - 1)], a[bl], a[min(lagMax, bl + 1)]
        den = y0 - 2 * y1 + y2
        off = 0.5 * (y0 - y2) / den if abs(den) > 1e-12 else 0.0
        return bl + min(max(off, -0.5), 0.5), best

    localBpm: List[float] = []
    reliableW: List[bool] = []
    for a in localAcf:
        lag, _ = best_lag(a)
        al = a.tolist()
        mean_ = 0.0
        cnt = 0
        for l in range(lagMin, lagMax + 1):
            mean_ += al[l]
            cnt += 1
        mean_ /= max(1, cnt)
        pk = al[min(max(_cround(lag), lagMin), lagMax)]
        reliable = mean_ > 1e-9 and pk / mean_ > 1.6
        localBpm.append(60.0 / (lag * hop) if reliable else math.nan)
        reliableW.append(reliable)
    last = math.nan
    for i, b in enumerate(localBpm):
        if math.isnan(b):
            localBpm[i] = last
        else:
            last = b
    last = math.nan
    for i in range(len(localBpm) - 1, -1, -1):
        if math.isnan(localBpm[i]):
            localBpm[i] = last
        else:
            last = localBpm[i]
    localBpm = [pref if math.isnan(b) else b for b in localBpm]
    tmp = sorted(localBpm)
    globalBpm = tmp[len(tmp) // 2]
    for i, b in enumerate(localBpm):
        best, bestErr = b, 1e9
        for mm in (1.0, 2.0, 0.5):  # octave errors only (3:2 would erase real tempo changes)
            c = b * mm
            err = abs(math.log2(c / globalBpm)) + (0.0 if mm == 1.0 else 0.05)
            if err < bestErr:
                bestErr, best = err, c
        if abs(math.log2(best / globalBpm)) < 0.25:
            localBpm[i] = best
    sm = []
    for i in range(len(localBpm)):
        wv = sorted(localBpm[j] for j in range(i - 2, i + 3) if 0 <= j < len(localBpm))
        sm.append(wv[len(wv) // 2])
    A.tempo = []
    cur = TempoSegment(0.0, 0.0, sm[0] if sm else globalBpm)
    runDiff = 0
    runStart = 0
    for i in range(len(sm)):
        if not reliableW[i]:  # beatless windows neither extend nor reset a tempo-change run
            continue
        differs = abs(sm[i] - cur.bpm) / cur.bpm > 0.04
        if differs:
            if runDiff == 0:
                runStart = i
            runDiff += 1
        else:
            runDiff = 0
        if runDiff >= 3:
            changeT = max(0.0, localCenter[runStart] - 2.0)
            if changeT - cur.start > 6.0:
                cur.end = changeT
                A.tempo.append(cur)
                cur = TempoSegment(changeT, 0.0, sm[i])
            else:
                cur.bpm = sm[i]
            runDiff = 0
    cur.end = A.duration
    A.tempo.append(cur)
    for seg in A.tempo:
        ssum = np.zeros(lagMax + 1, F32)
        for wi in range(len(localAcf)):
            if seg.start <= localCenter[wi] <= seg.end:
                ssum = ssum + localAcf[wi]
        target = 60.0 / seg.bpm / hop
        lo = max(lagMin, int(target * 0.95))
        hi = min(lagMax - 1, int(target * 1.05) + 1)
        bl = lo
        for l in range(lo, hi + 1):
            if ssum[l] > ssum[bl]:
                bl = l
        if bl > 0 and ssum[bl] > 0:
            y0, y1, y2 = float(ssum[bl - 1]), float(ssum[bl]), float(ssum[bl + 1])
            den = y0 - 2 * y1 + y2
            off = min(max(0.5 * (y0 - y2) / den, -0.5), 0.5) if abs(den) > 1e-12 else 0.0
            seg.bpm = 60.0 / ((bl + off) * hop)
    globalBpm = A.mainBpm()

    # ------------------------------------------------------------ beat tracking (dynamic programming)
    period = 60.0 / globalBpm / hop
    half = max(1, int(period / 32))
    g = [F32(math.exp(-0.5 * (i * i) / (half * half / 4.0 + 1e-9))) for i in range(-half, half + 1)]
    acc = np.zeros(nFine, F64)
    for k in range(-half, half + 1):
        sh = np.zeros(nFine, F32)
        if k >= 0:
            sh[:nFine - k] = oenv[k:]
        else:
            sh[-k:] = oenv[:nFine + k]
        acc = acc + (g[k + half] * sh).astype(F64)
    local = acc.astype(F32)
    meanL = np.add.accumulate(local)[-1] / F32(nFine)
    dv = local - meanL
    sd = np.add.accumulate(dv * dv)[-1]
    sd = np.sqrt(sd / F32(nFine)) + F32(1e-9)
    local = (local / sd).astype(F32)

    seg_ends = np.array([s_.end for s_ in A.tempo])
    seg_P = [60.0 / s_.bpm / hop for s_ in A.tempo]

    def period_at(frame: int) -> float:
        t = frame * hop
        for s_ in A.tempo:
            if t < s_.end:
                return 60.0 / s_.bpm / hop
        return 60.0 / A.tempo[-1].bpm / hop

    tf = np.arange(nFine) * hop
    seg_of = np.minimum(np.searchsorted(seg_ends, tf, side="right"), len(A.tempo) - 1)
    pens = []
    for P in seg_P:
        dmax = _cround(2 * P)
        dmin = _cround(P / 2)
        pen_ = np.full(dmax + 1, np.inf, F64)
        for dd in range(max(1, dmin), dmax + 1):
            r = float(dd) / P
            lr = math.log(r)
            pen_[dd] = 100.0 * lr * lr
        pens.append((dmin, dmax, pen_))
    score = np.zeros(nFine, F64)  # tightness = 100 (in pens)
    backp = np.full(nFine, -1, np.int64)
    local64 = local.astype(F64)
    seg_of_l = seg_of.tolist()
    for i in range(nFine):
        dmin, dmax, pen_ = pens[seg_of_l[i]]
        lo = i - dmax
        hi = i - dmin
        jlo = max(0, lo)
        if hi >= jlo:
            vals = score[jlo:hi + 1] - pen_[i - hi:i - jlo + 1][::-1]
            bi = int(np.argmax(vals))
            score[i] = local64[i] + vals[bi]
            backp[i] = jlo + bi
        else:
            score[i] = local64[i]
    P = period_at(nFine - 1)
    st = max(0, nFine - _cround(P) - 1)
    endIdx = st + int(np.argmax(score[st:]))
    beatFrames = []
    i = endIdx
    while i >= 0:
        beatFrames.append(i)
        i = int(backp[i])
    beatFrames.reverse()
    while beatFrames:
        p = period_at(beatFrames[0])
        prev = beatFrames[0] - _cround(p)
        if prev < int(0.05 / hop):
            break
        beatFrames.insert(0, prev)
    refine = _Refiner(x, sr)
    rad = max(1, int(0.025 / hop))
    beats: List[float] = []
    beatStr: List = []
    for bf in beatFrames:
        bestI = bf
        bestV = oenv[bf]
        for j in range(max(1, bf - rad), min(nFine - 2, bf + rad) + 1):
            if oenv[j] > bestV * F32(1.2) and oenv[j] > F32(0.15):
                bestV = oenv[j]
                bestI = j
        pos = float(bestI)
        if bestI > 0 and bestI + 1 < nFine:
            y0, y1, y2 = float(oenv[bestI - 1]), float(oenv[bestI]), float(oenv[bestI + 1])
            den = y0 - 2 * y1 + y2
            if abs(den) > 1e-9 and y1 > 0.1:
                pos += min(max(0.5 * (y0 - y2) / den, -0.5), 0.5)
        t = pos * hop
        if opt.fineOnsets and bestV > F32(0.15):
            t = refine(t)
        if beats and t <= beats[-1] + 0.1 * period_at(bf) * hop:
            continue
        beats.append(t)
        beatStr.append(_min1(bestV))

    # Refine each tempo segment's BPM by linear regression of the tracked beat times.
    for seg in A.tempo:
        bt = [b for b in beats if seg.start + 0.5 <= b < seg.end - 0.5]
        if len(bt) < 8:
            continue
        expected = 60.0 / seg.bpm
        idx = [0.0] * len(bt)
        for k in range(1, len(bt)):
            idx[k] = idx[k - 1] + _cround((bt[k] - bt[k - 1]) / expected)
        use = [True] * len(bt)
        slope = 0.0
        fitted = False
        for _ in range(3):  # robust: fit, drop residuals > 3x median, refit
            sx = sy = sxx = sxy = 0.0
            n = 0
            for k in range(len(bt)):
                if use[k]:
                    sx += idx[k]
                    sy += bt[k]
                    sxx += idx[k] * idx[k]
                    sxy += idx[k] * bt[k]
                    n += 1
            den = n * sxx - sx * sx
            if n < 8 or den <= 1e-9:
                break
            slope = (n * sxy - sx * sy) / den
            t0_ = (sy - slope * sx) / n
            fitted = True
            res = [abs(bt[k] - (t0_ + slope * idx[k])) for k in range(len(bt))]
            thr = max(3.0 * sorted(res)[len(res) // 2], 0.002)
            use = [r <= thr for r in res]
        if fitted and slope > 0 and abs(slope - expected) / expected < 0.03:
            seg.bpm = 60.0 / slope

    # ------------------------------------------------------------ onsets, drums
    ev: List[MusicEvent] = []

    def to_events(peaks, env, etype, band):
        for p in peaks:
            t = p.refined * hop
            if opt.fineOnsets:
                t = refine(t)
            ev.append(MusicEvent(time=max(0.0, t), type=etype, strength=float(_min1(env[p.frame])), band=band))

    ms30 = max(1, int(0.030 / hop))
    bandO = A.bandOnset
    pk = _pick_peaks(A.onsetEnv, 3, 3, int(0.1 / hop), int(0.07 / hop), 0.06, ms30 * 2, 0.08)
    for p in pk:
        bb = 0
        bv = F32(-1)
        for b in range(4):
            v = bandO[b][p.frame]
            if v > bv:
                bv = v
                bb = b
        to_events([p], A.onsetEnv, EventType.Onset, Band(bb))

    def band_max(b, f, r):
        lo = max(0, f - r)
        hi = min(nFine, f + r + 1)
        return max(F32(0), bandO[b][lo:hi].max()) if hi > lo else F32(0)

    def band_perc_at(band, fine_frame):
        cf = min(nCoarse - 1, int(float(fine_frame) * hop / A.hopCoarse))
        seg_ = percBandE[band][cf:min(nCoarse, cf + 3)]
        return max(F32(0), seg_.max()) if len(seg_) else F32(0)

    kicks = _pick_peaks(bandO[0], 3, 3, int(0.1 / hop), int(0.05 / hop), 0.12, int(0.09 / hop), 0.2)
    kk = [p for p in kicks if band_perc_at(0, p.frame) > F32(0.35)]
    to_events(kk, bandO[0], EventType.Kick, Band.Bass)
    mids = _pick_peaks(bandO[2], 3, 3, int(0.1 / hop), int(0.05 / hop), 0.1, int(0.09 / hop), 0.2)
    sn = []
    for p in mids:
        hi_ = band_max(3, p.frame, 2)
        lm = band_max(1, p.frame, 2)
        mid = bandO[2][p.frame]
        pm = band_perc_at(1, p.frame)
        ph = band_perc_at(2, p.frame)
        if hi_ > F32(0.25) and mid >= F32(0.55) * hi_ and (lm > F32(0.15) or mid > F32(0.4)) and pm > F32(0.3) and ph > F32(0.15):
            sn.append(p)
    to_events(sn, bandO[2], EventType.Snare, Band.Mid)
    highs = _pick_peaks(bandO[3], 2, 2, int(0.06 / hop), int(0.03 / hop), 0.08, int(0.05 / hop), 0.15)
    hh = []
    for p in highs:
        mid = band_max(2, p.frame, 1)
        hi_ = bandO[3][p.frame]
        pm = band_perc_at(1, p.frame)
        ph = band_perc_at(2, p.frame)
        if mid < hi_ * F32(0.85) and ph > F32(0.15) and pm < F32(0.6) * max(ph, F32(0.3)):
            hh.append(p)
    to_events(hh, bandO[3], EventType.HiHat, Band.High)

    # ------------------------------------------------------------ beat phase check
    ev.sort(key=lambda e: e.time)
    ev_t = np.array([e.time for e in ev])

    def evidence(t):
        s_ = F32(0)
        i0 = int(np.searchsorted(ev_t, t - 0.035 - 1e-9, side="left"))
        for k in range(max(0, i0 - 1), len(ev)):
            e = ev[k]
            if e.time > t + 0.035:
                break
            if (e.type == EventType.Kick or e.type == EventType.Snare) and abs(e.time - t) < 0.035:
                s_ = s_ + F32(e.strength)
        return s_

    nbt = len(beats)
    shift = [0] * nbt
    WB = 8
    winDecision = []
    i0 = 0
    while i0 + 1 < nbt:
        on = F32(0)
        off = F32(0)
        for i in range(i0, min(nbt - 1, i0 + WB)):
            on = on + evidence(beats[i])
            off = off + evidence(0.5 * (beats[i] + beats[i + 1]))
        winDecision.append(1 if off > F32(1.4) * on + F32(0.5) else 0)
        i0 += WB
    for wd in range(len(winDecision)):
        votes = total = 0
        for dd in (-1, 0, 1):
            j = wd + dd
            if 0 <= j < len(winDecision):
                votes += winDecision[j]
                total += 1
        dec = 1 if votes * 2 > total else 0
        for i in range(wd * WB, min(nbt, (wd + 1) * WB)):
            shift[i] = dec
    if any(shift):
        nbeats: List[float] = []
        nstr: List = []
        for i in range(nbt):
            t = beats[i]
            if shift[i]:
                per = beats[i + 1] - beats[i] if i + 1 < nbt else (beats[i] - beats[i - 1] if i > 0 else 0.5)
                t += 0.5 * per
                if opt.fineOnsets:
                    r_, found = refine.find(t - 0.008)
                    if found:
                        t = r_  # no attack: keep the half-beat position, not the search centre
            if nbeats:
                p_ = beats[i] - beats[i - 1] if i > 0 else 0.5
                if t - nbeats[-1] < 0.6 * p_:
                    continue
            nbeats.append(t)
            f = min(nFine - 1, int(t / hop))
            nstr.append(_min1(A.onsetEnv[f]))
        beats, beatStr = nbeats, nstr

    # ------------------------------------------------------------ metronomic grid fit
    out: List[float] = []
    for seg in A.tempo:
        bt = [b for b in beats if seg.start <= b < seg.end]
        if len(bt) < 16:
            out.extend(bt)
            continue
        P = 60.0 / seg.bpm
        k = [0.0] * len(bt)
        for i in range(1, len(bt)):
            k[i] = k[i - 1] + max(1.0, float(_cround((bt[i] - bt[i - 1]) / P)))
        use = [True] * len(bt)
        t0, slope = bt[0], P
        for _ in range(4):
            sx = sy = sxx = sxy = 0.0
            n = 0
            for i in range(len(bt)):
                if use[i]:
                    sx += k[i]
                    sy += bt[i]
                    sxx += k[i] * k[i]
                    sxy += k[i] * bt[i]
                    n += 1
            den = n * sxx - sx * sx
            if n < 8 or den <= 1e-12:
                break
            slope = (n * sxy - sx * sy) / den
            t0 = (sy - slope * sx) / n
            use = [abs(bt[i] - (t0 + slope * k[i])) < 0.03 for i in range(len(bt))]
        res = []
        inl = 0
        for i in range(len(bt)):
            r = abs(bt[i] - (t0 + slope * k[i]))
            res.append(r)
            if r < 0.03:
                inl += 1
        res.sort()
        med_res = res[len(res) // 2]
        metronomic = abs(slope - P) / P < 0.02 and ((med_res < 0.012 and inl >= int(len(bt) * 0.8)) or
                                                     (med_res < 0.003 and inl >= int(len(bt) * 0.6)))
        if not metronomic:
            out.extend(bt)
            continue
        seg.bpm = 60.0 / slope
        kStart = math.ceil((seg.start - t0) / slope - 0.25)
        kk_ = float(kStart)
        while True:
            t = t0 + kk_ * slope
            kk_ += 1
            if t >= seg.end or t >= A.duration - 0.05:
                break
            if t < 0:
                continue
            r = t
            if opt.fineOnsets:
                cand, found = refine.find(t - 0.004, 0.008, 0.012)
                if found and abs(cand - t) < 0.012:
                    r = cand
            out.append(r)
    out.sort()
    A.beats = []
    A.beatStrength = []
    for b in out:
        if A.beats and b - A.beats[-1] < 0.1:
            continue
        A.beats.append(b)
        f = min(nFine - 1, int(b / hop))
        lo, hi = max(0, f - 3), min(nFine, f + 4)
        m_ = max(F32(0), A.onsetEnv[lo:hi].max())
        A.beatStrength.append(float(_min1(m_)))

    # ------------------------------------------------------------ melody notes & sustains
    hc = A.hopCoarse
    onset_times = np.array([e.time for e in ev if e.type == EventType.Onset])
    pm_ = A.pitchMidi
    pcf = A.pitchConf
    pm_list = pm_.tolist()
    f = 0
    sustainMin = F32(opt.sustainMin)
    while f < nCoarse:
        if pm_list[f] <= 0:
            f += 1
            continue
        s_ = f
        ref = pm_[f]
        confSum = 0.0
        gap = 0
        lastVoiced = f
        ps = []
        while f < nCoarse:
            p = pm_[f]
            if p <= 0:
                gap += 1
                if gap > 2:
                    break
                f += 1
                continue
            if abs(p - ref) > F32(0.75):
                break
            gap = 0
            ps.append(p)
            confSum += float(pcf[f])
            lastVoiced = f
            if len(ps) >= 3:
                ref = sorted(ps)[len(ps) // 2]
            f += 1
        if f == s_:
            f += 1
        ln = lastVoiced - s_ + 1
        if ln >= 2:
            e = MusicEvent(time=s_ * hc, type=EventType.Note)
            r5 = ref * F32(5)
            e.pitch = float(F32(_cround(float(r5))) / F32(5))
            e.duration = float(F32(ln * hc))
            e.strength = float(F32(confSum / float(len(ps))))
            if len(onset_times):
                dd = np.abs(onset_times - e.time)
                ai = int(np.argmin(dd))
                if dd[ai] < 0.04:
                    e.time = float(onset_times[ai])
            ev.append(e)
            if F32(e.duration) >= sustainMin:
                ev.append(MusicEvent(time=e.time, type=EventType.Sustain, strength=e.strength, band=e.band,
                                     pitch=e.pitch, duration=e.duration, index=e.index))

    # ------------------------------------------------------------ intensity
    lo_ = _percentile(A.rmsDb, 0.05)
    hiDb = _percentile(A.rmsDb, 0.97)
    loud = np.clip((A.rmsDb - lo_) / max(F32(1.0), F32(hiDb - lo_)), F32(0), F32(1)).astype(F32)
    dens = np.zeros(nFine, F32)
    for e in ev:
        if e.type not in (EventType.Onset, EventType.Kick, EventType.Snare):
            continue
        fi = int(e.time / hop)
        if fi < nFine:
            dens[fi] = dens[fi] + (F32(0.5) + F32(0.5) * F32(e.strength))
    dens = _moving_average(dens, int(1.0 / hop))
    dp = _percentile(dens, 0.97)
    if dp > 0:
        dens = np.minimum(F32(1.0), dens / dp).astype(F32)
    cfi = np.minimum(nCoarse - 1, (np.arange(nFine, dtype=F64) * hop / A.hopCoarse).astype(np.int64))
    raw = (F32(0.55) * loud + F32(0.30) * dens + F32(0.15) * A.percRatio[cfi]).astype(F32)
    raw = _moving_average(raw, int(0.75 / hop))
    ra, rb = _percentile(raw, 0.02), _percentile(raw, 0.98)
    A.intensity = np.clip((raw - ra) / max(F32(1e-6), F32(rb - ra)), F32(0), F32(1)).astype(F32)

    # ------------------------------------------------------------ meter & downbeats
    nb_ = len(A.beats)
    ev_types = np.array([int(e.type) for e in ev])
    ev_times = np.array([e.time for e in ev])
    ev_str = np.array([e.strength for e in ev], dtype=F32)

    def nearest_strength(etype, t, tol):
        msk = (ev_types == int(etype)) & (np.abs(ev_times - t) < tol)
        return max(F32(0), ev_str[msk].max()) if msk.any() else F32(0)

    chromaChg = np.zeros(nb_, F32)
    for i in range(nb_):
        dt = (A.beats[i + 1] - A.beats[i] if i + 1 < nb_ else 0.5) * 0.5
        c0 = int(max(0.0, A.beats[i] - dt) / A.hopCoarse)
        c1 = min(nCoarse - 1, int((A.beats[i] + dt) / A.hopCoarse))
        chg = F32(0)
        if c0 < nCoarse and c1 < nCoarse:
            dif = np.abs(A.chroma[c1] - A.chroma[c0])
            for kq in range(12):
                chg = chg + dif[kq]
        chromaChg[i] = chg
    chgNorm = max(F32(1e-6), _percentile(chromaChg, 0.9))
    accent = np.zeros(nb_, F32)
    for i in range(nb_):
        t = A.beats[i]
        kick = nearest_strength(EventType.Kick, t, 0.03)
        snare = nearest_strength(EventType.Snare, t, 0.03)
        note = nearest_strength(EventType.Note, t, 0.04)
        accent[i] = kick - F32(0.8) * snare + F32(0.9) * min(F32(1.0), chromaChg[i] / chgNorm) + F32(0.3) * note
    acc_l = [float(a) for a in accent]

    def phase_score(m):
        best = -1e9
        bestPhase = 0
        for ph in range(m):
            on = off = 0.0
            non = noff = 0
            for i in range(nb_):
                if (i + m - ph) % m == 0:
                    on += acc_l[i]
                    non += 1
                else:
                    off += acc_l[i]
                    noff += 1
            sc = (on / non if non else 0.0) - (off / noff if noff else 0.0)
            if sc > best:
                best = sc
                bestPhase = ph
        return best, bestPhase

    s4, _ = phase_score(4)
    s3, _ = phase_score(3)
    mtr = opt.beatsPerBar if opt.beatsPerBar > 0 else (3 if (s3 > s4 * 1.25 and s3 > 0.05) else 4)
    A.beatsPerBar = mtr
    A.beatInBar = [0] * nb_
    if nb_ > 0:
        jumpPenalty = 2.5

        def emit(i, st_):
            a = acc_l[i]
            return a if st_ == 0 else (-0.25 * a if st_ == mtr // 2 else -0.1 * a)

        sc = [emit(0, st_) for st_ in range(mtr)]
        bp_ = [[0] * mtr for _ in range(nb_)]
        for i in range(1, nb_):
            bestS = max(range(mtr), key=lambda q: (sc[q], -q))
            nsc = [0.0] * mtr
            for st_ in range(mtr):
                prevNatural = (st_ + mtr - 1) % mtr
                natural = sc[prevNatural]
                jump = sc[bestS] - jumpPenalty
                if natural >= jump:
                    nsc[st_] = natural + emit(i, st_)
                    bp_[i][st_] = prevNatural
                else:
                    nsc[st_] = jump + emit(i, st_)
                    bp_[i][st_] = bestS
            mx_ = max(nsc)
            sc = [v - mx_ for v in nsc]
        st_ = max(range(mtr), key=lambda q: (sc[q], -q))
        for i in range(nb_ - 1, -1, -1):
            A.beatInBar[i] = st_
            if i > 0:
                st_ = bp_[i][st_]
    A.downbeats = [A.beats[i] for i in range(nb_) if A.beatInBar[i] == 0]

    # ------------------------------------------------------------ sections
    _sections(A, ev, nFine, nCoarse, opt)

    # ------------------------------------------------------------ beats, tempo, silence events
    for i, b in enumerate(A.beats):
        ev.append(MusicEvent(time=b, type=EventType.Downbeat if A.beatInBar[i] == 0 else EventType.Beat,
                             strength=A.beatStrength[i], index=i))
    for i in range(1, len(A.tempo)):
        # strength = size of the change (1 = factor 2), index = tempo segment, duration = new beat period
        ev.append(MusicEvent(time=A.tempo[i].start, type=EventType.TempoChange, index=i,
                             strength=float(F32(min(1.0, abs(math.log2(A.tempo[i].bpm / A.tempo[i - 1].bpm))))),
                             duration=float(F32(60.0 / A.tempo[i].bpm))))
    thr = max(F32(-50.0), F32(_percentile(A.rmsDb, 0.5) - F32(30.0)))
    below = A.rmsDb < thr
    i = 0
    bl_ = below.tolist()
    while i < nFine:
        if not bl_[i]:
            i += 1
            continue
        s_ = i
        while i < nFine and bl_[i]:
            i += 1
        ln = float(i - s_) * hop
        if ln >= 0.4:
            ev.append(MusicEvent(time=s_ * hop, type=EventType.Silence, duration=float(F32(ln))))
    ev.sort(key=lambda e: (e.time, int(e.type)))
    A.events = ev
    A.beatStrength = [float(v) for v in A.beatStrength]
    return A


def _sections(A: MusicAnalysis, ev: List[MusicEvent], nFine: int, nCoarse: int, opt: AnalysisOptions) -> None:
    hop = A.hop
    bars = list(A.downbeats)
    if len(bars) < 4:
        bars = []
        t = 0.0
        while t < A.duration:
            bars.append(t)
            t += 8.0
    pickup = 1 if bars[0] > 0.5 else 0  # pickup bar from 0 s does not count for the 4-bar phase
    if pickup:
        bars.insert(0, 0.0)
    bars.append(A.duration)
    nbar = len(bars) - 1
    D = 17
    dbLo, dbHi = _percentile(A.rmsDb, 0.05), _percentile(A.rmsDb, 0.97)
    loudC = np.clip((A.rmsDb - dbLo) / max(F32(1.0), F32(dbHi - dbLo)), F32(0), F32(1)).astype(F32)
    bassC = np.minimum(F32(1.0), A.bandOnset[0]).astype(F32)
    barLoud = np.zeros(nbar, F32)
    barBass = np.zeros(nbar, F32)
    fr = []
    for b in range(nbar):
        f0 = int(bars[b] / hop)
        f1 = max(f0 + 1, min(nFine, int(bars[b + 1] / hop)))
        fr.append((f0, f1))
        e = min(f1, nFine)
        n = float(max(1, f1 - f0))
        barLoud[b] = _seqsum(loudC[f0:e]) / n
        barBass[b] = _seqsum(bassC[f0:e]) / n
    bassMax = max(F32(1e-6), _percentile(barBass, 0.95))
    barEnergy = (F32(0.5) * barLoud + F32(0.5) * np.minimum(F32(1.0), barBass / bassMax)).astype(F32)
    feat = np.zeros((nbar, D), F32)
    for b in range(nbar):
        f0, f1 = fr[b]
        e = min(f1, nFine)
        nf = float(f1 - f0)
        inten = _seqsum(A.intensity[f0:e])
        onset = _seqsum(A.onsetEnv[f0:e])
        bass = _seqsum(A.bandOnset[0][f0:e])
        high = _seqsum(A.bandOnset[3][f0:e])
        c0 = int(bars[b] / A.hopCoarse)
        c1 = max(c0 + 1, min(nCoarse, int(bars[b + 1] / A.hopCoarse)))
        ce = min(c1, nCoarse)
        if ce > c0:
            chroma = np.add.accumulate(A.chroma[c0:ce].astype(F64), axis=0)[-1]
        else:
            chroma = np.zeros(12, F64)
        pr = _seqsum(A.percRatio[c0:ce])
        nc = float(c1 - c0)
        feat[b, 0] = inten / nf
        feat[b, 1] = onset / nf
        feat[b, 2] = bass / nf
        feat[b, 3] = high / nf
        feat[b, 4] = pr / nc
        feat[b, 5:] = (chroma / nc).astype(F32) * F32(0.35)
    for d in range(5):
        col = feat[:, d]
        mean = _seqsum(col) / float(nbar)
        dv = col.astype(F64) - mean
        sd = math.sqrt(_seqsum(dv * dv) / float(nbar)) + 1e-6
        feat[:, d] = ((col.astype(F64) - mean) / sd).astype(F32)
    diff = (feat[:, None, :] - feat[None, :, :]).astype(F64)
    dist = np.sqrt(_seqsum_rows(diff * diff)).tolist()
    K = 4
    nov = np.zeros(nbar, F32)
    be = [float(v) for v in barEnergy]
    for b in range(1, nbar):
        cross = within = 0.0
        nc_ = nw = 0
        for i in range(-K, 0):
            for j in range(0, K):
                a, c = b + i, b + j
                if a < 0 or c >= nbar:
                    continue
                w = math.exp(-0.5 * ((i + 0.5) * (i + 0.5) + (j + 0.5) * (j + 0.5)) / (K * K * 0.5))
                cross += w * dist[a][c]
                nc_ += 1
        for i in range(-K, 0):
            for j in range(-K, 0):
                a, c = b + i, b + j
                if a < 0 or c < 0 or i >= j:
                    continue
                within += dist[a][c]
                nw += 1
        for i in range(0, K):
            for j in range(0, K):
                a, c = b + i, b + j
                if a >= nbar or c >= nbar or i >= j:
                    continue
                within += dist[a][c]
                nw += 1
        nov[b] = (cross / nc_ if nc_ else 0.0) - 0.5 * (within / nw if nw else 0.0)
        eB = eA = 0.0
        n0 = n1 = 0
        for i in (1, 2):
            if b - i >= 0:
                eB += be[b - i]
                n0 += 1
            if b + i - 1 < nbar:
                eA += be[b + i - 1]
                n1 += 1
        if n0 and n1:
            nov[b] = nov[b] + F32(4.0 * abs(eA / n1 - eB / n0))
    nmax = nov.max()
    bounds = [0]
    cand = []
    b = 2
    while b + 2 <= nbar:
        v = nov[b]
        if not (v < nmax * F32(0.3)) and not (v < nov[b - 1] or (b + 1 < nbar and v < nov[b + 1])):
            rb = b - pickup
            bonus = F32(1.15) if rb % 4 == 0 else (F32(1.05) if rb % 2 == 0 else F32(1.0))
            cand.append((v * bonus, b))
        b += 1
    cand.sort(key=lambda c: -c[0])
    chosen = []
    for v, b in cand:
        ok = b >= 4 and b + 4 <= nbar
        for c in chosen:
            if abs(b - c) < 4:
                ok = False
        if ok:
            chosen.append(b)
    chosen.sort()
    for ci in range(len(chosen)):
        b = chosen[ci]
        if (b - pickup) % 4 == 0:
            continue
        prevB = chosen[ci - 1] if ci > 0 else 0
        nextB = chosen[ci + 1] if ci + 1 < len(chosen) else nbar
        for d in (-1, 1, -2, 2):
            nb2 = b + d
            if nb2 - prevB < 4 or nextB - nb2 < 4:
                continue
            if (nb2 - pickup) % 4 == 0 and nov[nb2] >= F32(0.75) * nov[b]:
                chosen[ci] = nb2
                break
    for b in chosen:
        if b != bounds[-1]:
            bounds.append(b)
    bounds.append(nbar)

    onset_t = np.array([o.time for o in ev if o.type == EventType.Onset])
    kick_ev = [(o.time, o.strength) for o in ev if o.type == EventType.Kick]

    def compute_stats(sc: Section):
        f0 = int(sc.start / hop)
        f1 = min(nFine, int(sc.end / hop))
        e = _seqsum(A.intensity[f0:f1]) if f1 > f0 else 0.0
        sc.energy = float(F32(e / float(max(1, f1 - f0))))
        cnt = int(np.count_nonzero((onset_t >= sc.start) & (onset_t < sc.end))) if len(onset_t) else 0
        sc.density = float(F32(cnt / max(0.1, sc.end - sc.start)))
        fs = np.arange(f0, f1, 8) if f1 > f0 else np.zeros(0, np.int64)
        n = len(fs)
        if n > 2:
            t = fs * hop
            y = A.intensity[fs].astype(F64)
            sx, sy, sxx, sxy = _seqsum(t), _seqsum(y), _seqsum(t * t), _seqsum(t * y)
            sc.trend = float(F32((n * sxy - sx * sy) / max(1e-9, n * sxx - sx * sx) * 10.0))
        else:
            sc.trend = 0.0
        acc = _seqsum(barEnergy[sc.firstBar:sc.firstBar + sc.barCount])
        sc.power = float(F32(acc / max(1, sc.barCount)))

    def bass_of(sc: Section) -> float:
        k = 0.0
        for t, s in kick_ev:
            if sc.start <= t < sc.end:
                k += 0.5 + 0.5 * s
        acc = _seqsum(barBass[sc.firstBar:sc.firstBar + sc.barCount])
        return k / max(0.1, sc.end - sc.start) + 0.2 * acc / max(1, sc.barCount)

    secs: List[Section] = []
    for i in range(len(bounds) - 1):
        s = Section(firstBar=bounds[i], barCount=bounds[i + 1] - bounds[i], start=bars[bounds[i]], end=bars[bounds[i + 1]])
        compute_stats(s)
        secs.append(s)
    ns = len(secs)
    power = [F32(s.power) for s in secs]
    bassN = [F32(bass_of(s)) for s in secs]
    bassTop = max(F32(1e-6), max(bassN))
    bassN = [b / bassTop for b in bassN]
    pHi, pLo = _percentile(power, 0.75), _percentile(power, 0.25)
    pMax = max(power)
    high = [((power[i] >= max(F32(0.5), pHi - F32(0.05))) or (power[i] >= pMax - F32(0.06))) and bassN[i] >= F32(0.55)
            for i in range(ns)]
    for i in range(ns):
        sc = secs[i]
        lowPower = power[i] <= min(F32(0.4), pLo + F32(0.02))
        nxt = i + 1 < ns and high[i + 1]
        rising = F32(sc.trend) > F32(0.25) and nxt and bassN[i] < bassN[i + 1] * F32(0.9)
        if rising:
            sc.type = SectionType.BuildUp
        elif high[i]:
            sc.type = SectionType.Chorus
        elif lowPower and F32(sc.density) < F32(6.0):
            sc.type = SectionType.Break
        else:
            sc.type = SectionType.Verse
    for i in range(ns - 1, 0, -1):
        if secs[i].type != SectionType.BuildUp:
            continue
        p = secs[i - 1]
        if p.type == SectionType.Verse and F32(p.trend) > F32(0.08) and F32(p.power) < F32(secs[i].power):
            p.type = SectionType.BuildUp
    for i in range(ns):
        sc = secs[i]
        if sc.type != SectionType.Chorus:
            continue
        if i > 0 and secs[i - 1].type == SectionType.Drop:
            sc.type = SectionType.Drop
            continue
        prevP = power[i - 1] if i > 0 else F32(0)
        afterBuild = i > 0 and secs[i - 1].type == SectionType.BuildUp
        if afterBuild or power[i] - prevP > F32(0.2):
            sc.type = SectionType.Drop
    i = 1
    while i < len(secs):
        a, b = secs[i - 1], secs[i]
        if a.type == SectionType.BuildUp and b.type == SectionType.BuildUp:
            a.end = b.end
            a.barCount += b.barCount
            compute_stats(a)
            del secs[i]
        else:
            i += 1
    i = 0
    while i < len(secs):
        p = secs[i]
        if p.type != SectionType.BuildUp or p.barCount <= 16:
            i += 1
            continue
        tailBars = 8
        boundaryBar = p.firstBar + p.barCount - tailBars
        head = Section(start=p.start, end=bars[boundaryBar], type=SectionType.Verse, energy=p.energy, power=p.power,
                       density=p.density, trend=p.trend, firstBar=p.firstBar, barCount=p.barCount - tailBars)
        p.firstBar = boundaryBar
        p.barCount = tailBars
        p.start = head.end
        compute_stats(head)
        compute_stats(p)
        secs.insert(i, head)
        i += 2
    if secs:
        f = secs[0]
        if f.type != SectionType.Drop and F32(f.energy) < F32(0.6):
            f.type = SectionType.Intro
        l = secs[-1]
        if len(secs) > 1 and l.type != SectionType.Drop and (F32(l.energy) < F32(0.5) or F32(l.trend) < F32(-0.1)):
            l.type = SectionType.Outro
    A.sections = secs
    for i, s in enumerate(secs):
        e = MusicEvent(time=s.start, type=EventType.SectionStart, index=i, strength=s.energy, duration=float(F32(s.duration())))
        ev.append(e)
        if s.type == SectionType.Drop:
            ev.append(MusicEvent(time=e.time, type=EventType.Drop, index=i, strength=e.strength, duration=e.duration))
        if s.type == SectionType.BuildUp:
            ev.append(MusicEvent(time=e.time, type=EventType.BuildUp, index=i, strength=e.strength, duration=e.duration))


def analyze_file(path: str, options: Optional[AnalysisOptions] = None) -> MusicAnalysis:
    """Decode a WAV file (mirror of decodeWav) and analyse it. MP3/OGG are not decoded here: convert
    them to WAV first (e.g. `ffmpeg -i song.mp3 -ac 1 song.wav`); note that a different decoder
    than the mod's minimp3/stb_vorbis can shift results slightly."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) >= 12 and data[0:4] == b"RIFF":
        samples, rate = decode_wav(data)
        return analyze(samples, rate, options)
    if data[:4] == b"OggS" or data[:3] == b"ID3" or (len(data) > 1 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0):
        raise ValueError(f"{path}: MP3/OGG decoding is not mirrored in Python; convert to WAV first "
                         "(e.g. `ffmpeg -i in.mp3 -ac 1 out.wav`)")
    raise ValueError(f"{path}: unsupported audio format")
