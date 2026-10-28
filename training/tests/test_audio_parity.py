#!/usr/bin/env python3
"""Parity test: the C++ music analysis (``alcli analyze <wav> --json``) vs the numpy mirror in
``training/autolayout/audio.py``.

Signals:
  * demo_song      - ``alcli demo-song`` (~144 s procedural EDM track, 22050 Hz mono 16-bit)
  * drum_loop      - synth_ref.drum_loop(124 BPM, 16 bars) (same formulas as synth.cpp)
  * tempo_change   - synth_ref.click_track_tempo_change(100 -> 140 BPM at 20 s, 45 s)
  * demo_44k_24bit - first 40 s of the demo song, upsampled to 44.1 kHz stereo 24-bit
                     (exercises the WAV decoder and the Kaiser-sinc resampler)

Both implementations read the same WAV file. Tolerances (see training/README.md):
  * per-frame curves: max |diff| <= 1e-4 (rmsDb: 1e-3 dB) and Pearson r >= 0.999;
    pitchMidi: >= 99.5% of frames identical
  * beats / downbeats: count within +-1, >= 99% matched within 5 ms both ways
  * tempo: same segment count, BPM within 0.1, boundaries within 0.5 s; same meter
  * sections: same count and labels, boundaries within 1 bar; energy/power within 0.01
  * events: onset/kick/snare/hihat/note/sustain matched within 10 ms with >= 97% recall
    both ways; structural events (section/buildup/drop/tempo/silence) same count, within 1 bar

Run with ``python3 -m pytest training/tests`` or standalone ``python3 training/tests/test_audio_parity.py``.
Set AL_ALCLI to use a different alcli binary (default: build/tools/alcli, built on demand).
"""
from __future__ import annotations

import json
import math
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "training"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from autolayout import audio  # noqa: E402
import synth_ref  # noqa: E402

CURVE_MAX_ABS = 1e-4
CURVE_MAX_ABS_DB = 1e-3
CURVE_MIN_CORR = 0.999
PITCH_FRAME_AGREEMENT = 0.995
BEAT_TOL = 0.005
BEAT_RECALL = 0.99
EVENT_TOL = 0.010
EVENT_RECALL = 0.97
BPM_TOL = 0.1
TIMED_EVENT_TYPES = ["onset", "kick", "snare", "hihat", "note", "sustain"]
STRUCT_EVENT_TYPES = ["section", "buildup", "drop", "tempo", "silence"]

_TMP = tempfile.TemporaryDirectory(prefix="al_parity_")
_CACHE: dict = {}


def _skip(msg: str):
    if "pytest" in sys.modules:
        import pytest
        pytest.skip(msg)
    raise SystemExit(f"SKIP: {msg}")


def _alcli() -> Path:
    p = Path(os.environ.get("AL_ALCLI", ROOT / "build" / "tools" / "alcli"))
    if not p.exists() and "AL_ALCLI" not in os.environ and (ROOT / "build").is_dir():
        subprocess.run(["cmake", "--build", str(ROOT / "build"), "--target", "alcli"], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not p.exists():
        _skip(f"alcli not found at {p} (configure and build the C++ project first)")
    return p


def _make_wav(name: str) -> Path:
    out = Path(_TMP.name) / f"{name}.wav"
    if out.exists():
        return out
    if name == "demo_song":
        subprocess.run([str(_alcli()), "demo-song", str(out)], check=True, stdout=subprocess.DEVNULL)
    elif name == "drum_loop":
        out.write_bytes(synth_ref.wav_bytes([synth_ref.render(synth_ref.drum_loop(124, 16))], 22050))
    elif name == "tempo_change":
        sc = synth_ref.click_track_tempo_change(100, 140, 20.0, 45.0)
        out.write_bytes(synth_ref.wav_bytes([synth_ref.render(sc)], 22050))
    elif name == "demo_44k_24bit":
        x, sr = audio.decode_wav(_make_wav("demo_song").read_bytes())
        x = x[: sr * 40].astype(np.float64)
        up = np.interp(np.arange(2 * len(x)) / 2.0, np.arange(len(x)), x)
        right = 0.7 * up + 0.02 * np.sin(np.arange(len(up)) * 0.01)
        out.write_bytes(synth_ref.wav_bytes([0.9 * up, right], 44100, bits=24))
    else:
        raise KeyError(name)
    return out


def _run_case(name: str):
    if name in _CACHE:
        return _CACHE[name]
    wav = _make_wav(name)
    js = wav.with_suffix(".json")
    t0 = time.perf_counter()
    r = subprocess.run([str(_alcli()), "analyze", str(wav), "--json", str(js)], check=True,
                       stdout=subprocess.PIPE, text=True)
    cpp_wall = time.perf_counter() - t0
    first = r.stdout.splitlines()[0] if r.stdout else ""
    cpp_sec = float(first.rsplit("(", 1)[1].split()[0]) if "(" in first else cpp_wall
    C = json.loads(js.read_text())
    if "curves" not in C:
        raise AssertionError("alcli JSON export lacks 'curves' (rebuild alcli with the extended analyze --json export)")
    t0 = time.perf_counter()
    P = audio.analyze_file(str(wav))
    py_sec = time.perf_counter() - t0
    _CACHE[name] = (P, C, cpp_sec, py_sec)
    return _CACHE[name]


# ---------------------------------------------------------------------------------------------
# comparison helpers


def _match_recall(a, b, tol):
    """Fraction of times in a that have a partner in b within tol."""
    a = np.asarray(a, float)
    b = np.sort(np.asarray(b, float))
    if len(a) == 0:
        return 1.0
    if len(b) == 0:
        return 0.0
    i = np.clip(np.searchsorted(b, a), 1, len(b) - 1) if len(b) > 1 else np.zeros(len(a), int)
    d = np.minimum(np.abs(b[i] - a), np.abs(b[np.maximum(i - 1, 0)] - a))
    return float(np.mean(d <= tol))


def _check_curve(name, py, cpp, report, db=False):
    py = np.asarray(py, np.float64)
    cpp = np.asarray(cpp, np.float64)
    assert py.shape == cpp.shape, f"{name}: shape {py.shape} != {cpp.shape}"
    d = np.abs(py - cpp)
    mx = float(d.max()) if d.size else 0.0
    exact = int(np.count_nonzero(d == 0))
    if py.std() > 0 and cpp.std() > 0:
        corr = float(np.corrcoef(py.ravel(), cpp.ravel())[0, 1])
    else:
        corr = 1.0 if mx == 0 else 0.0
    report.append(f"  {name:<14} max|d| {mx:.3g}  r {corr:.6f}  bit-exact {exact}/{d.size}")
    assert mx <= (CURVE_MAX_ABS_DB if db else CURVE_MAX_ABS), f"{name}: max abs diff {mx}"
    assert corr >= CURVE_MIN_CORR, f"{name}: correlation {corr}"


def _compare(name: str):
    P, C, cpp_sec, py_sec = _run_case(name)
    rep = [f"[{name}] duration {C['duration']:.2f} s   C++ {cpp_sec:.2f} s   Python {py_sec:.2f} s"]
    assert abs(P.duration - C["duration"]) < 1e-9
    assert abs(P.hop - C["hop"]) < 1e-15 and abs(P.hopCoarse - C["hopCoarse"]) < 1e-15
    cv = C["curves"]
    _check_curve("rmsDb", P.rmsDb, cv["rmsDb"], rep, db=True)
    _check_curve("onsetEnv", P.onsetEnv, cv["onsetEnv"], rep)
    for b in range(4):
        _check_curve(f"bandOnset[{b}]", P.bandOnset[b], cv["bandOnset"][b], rep)
    for k in ("intensity", "percRatio", "bassPercRatio", "pitchConf", "waveformMin", "waveformMax"):
        _check_curve(k, getattr(P, k), cv[k], rep)
    _check_curve("chroma", P.chroma, cv["chroma"], rep)
    pm_py, pm_c = np.asarray(P.pitchMidi), np.asarray(cv["pitchMidi"])
    assert pm_py.shape == pm_c.shape
    agree = float(np.mean(np.abs(pm_py - pm_c) < 0.01))
    rep.append(f"  pitchMidi      frames identical {agree * 100:.2f}%")
    assert agree >= PITCH_FRAME_AGREEMENT, f"pitchMidi agreement {agree}"

    # tempo / meter
    assert abs(P.mainBpm() - C["bpm"]) <= BPM_TOL, (P.mainBpm(), C["bpm"])
    assert len(P.tempo) == len(C["tempo"]), (len(P.tempo), len(C["tempo"]))
    for a, b in zip(P.tempo, C["tempo"]):
        assert abs(a.bpm - b["bpm"]) <= BPM_TOL and abs(a.start - b["start"]) <= 0.5 and abs(a.end - b["end"]) <= 0.5
    assert P.beatsPerBar == C["beatsPerBar"]
    rep.append(f"  bpm py {P.mainBpm():.6f} / c++ {C['bpm']:.6f}   segments {len(P.tempo)}   meter {P.beatsPerBar}/4")

    # beats / downbeats
    for key, a, b in (("beats", P.beats, C["beats"]), ("downbeats", P.downbeats, C["downbeats"])):
        assert abs(len(a) - len(b)) <= 1, f"{key}: count {len(a)} vs {len(b)}"
        r1, r2 = _match_recall(a, b, BEAT_TOL), _match_recall(b, a, BEAT_TOL)
        mx = float(np.max(np.abs(np.asarray(a) - np.asarray(b)))) if len(a) == len(b) and len(a) else float("nan")
        rep.append(f"  {key:<10} {len(a)} vs {len(b)}  recall {r1 * 100:.1f}% / {r2 * 100:.1f}%  max|dt| {mx:.3g} s")
        assert min(r1, r2) >= BEAT_RECALL, f"{key}: recall {r1}, {r2}"
    if len(P.beatInBar) == len(C["beatInBar"]):
        rep.append(f"  beatInBar identical {P.beatInBar == C['beatInBar']}")

    # sections
    bar = P.beatsPerBar * 60.0 / C["bpm"]
    labels_py = [audio.section_type_name(s.type) for s in P.sections]
    labels_c = [s["type"] for s in C["sections"]]
    assert labels_py == labels_c, f"section labels {labels_py} vs {labels_c}"
    for s, c in zip(P.sections, C["sections"]):
        assert abs(s.start - c["start"]) <= bar and abs(s.end - c["end"]) <= bar
        assert abs(s.energy - c["energy"]) <= 0.01 and abs(s.power - c["power"]) <= 0.01
        assert abs(s.trend - c["trend"]) <= 0.02 and abs(s.density - c["density"]) <= 0.1
    rep.append(f"  sections   {' '.join(labels_py)}")

    # events
    def times(evs, t):
        return [e.time for e in evs if audio.event_type_name(e.type) == t]

    ctimes = {}
    for e in C["events"]:
        ctimes.setdefault(e["type"], []).append(e["time"])
    exact = 0
    for a, b in zip(P.events, C["events"]):
        if audio.event_type_name(a.type) == b["type"] and a.time == b["time"] and a.strength == b["strength"] \
                and a.duration == b.get("duration", 0.0) and a.pitch == b.get("pitch", 0.0) and a.index == b["index"] \
                and int(a.band) == b["band"]:
            exact += 1
    rep.append(f"  events     {len(P.events)} vs {len(C['events'])}, identical (all fields, same order) {exact}")
    for t in TIMED_EVENT_TYPES:
        a, b = times(P.events, t), ctimes.get(t, [])
        r1, r2 = _match_recall(a, b, EVENT_TOL), _match_recall(b, a, EVENT_TOL)
        rep.append(f"    {t:<8} {len(a):4d} vs {len(b):4d}  recall {r1 * 100:.1f}% / {r2 * 100:.1f}%")
        assert min(r1, r2) >= EVENT_RECALL, f"{t}: recall {r1}, {r2}"
    for t in STRUCT_EVENT_TYPES:
        a, b = times(P.events, t), ctimes.get(t, [])
        assert len(a) == len(b), f"{t}: count {len(a)} vs {len(b)}"
        assert _match_recall(a, b, bar) == 1.0, f"{t}: times differ by more than a bar"
    print("\n".join(rep))
    return P, C, cpp_sec, py_sec


# ---------------------------------------------------------------------------------------------
# tests


def test_real_fft_matches_numpy():
    rng = np.random.default_rng(3)
    for n in (1024, 2048):
        x = rng.standard_normal((4, n)).astype(np.float32)
        ours = audio.RealFft(n).magnitude(x)
        ref = np.abs(np.fft.rfft(x.astype(np.float64), axis=1))
        assert np.max(np.abs(ours - ref)) <= 1e-4 * np.max(ref)


def test_demo_song():
    P, C, cpp_sec, py_sec = _compare("demo_song")
    assert py_sec < 60.0, f"Python analysis took {py_sec:.1f} s"


def test_drum_loop():
    _compare("drum_loop")


def test_tempo_change():
    P, C, _, _ = _compare("tempo_change")
    assert len(P.tempo) == 2
    # 100 -> 140 BPM is close to 3:2 and must not be folded into one tempo
    assert abs(P.tempo[0].bpm - 100) < 1.5 and abs(P.tempo[1].bpm - 140) < 2.0, [t.bpm for t in P.tempo]
    tc = [e for e in P.events if e.type == audio.EventType.TempoChange]
    assert len(tc) == 1 and 0 <= tc[0].strength <= 1 and tc[0].index == 1


def test_demo_44k_stereo_24bit():
    _compare("demo_44k_24bit")


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
