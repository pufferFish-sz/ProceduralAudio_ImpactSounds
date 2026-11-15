"""Run the real-FMOD integration test and check its render.

Run after building cpp/ with FMOD_SDK_DIR set. procaudio_fmod_host plays the
plug-in the way FMOD Studio does (a new instance per Play) through FMOD's
NRT WAV writer; this script checks every segment:
  - sound starts within the first mix block of a Play (no build wait)
  - no clipping, no non-finite samples, no hard edges
  - impact / drop segments are one-shots: they decay to silence
  - roll / slide segments start WITHOUT a hit (onset not louder than the
    steady part) and Impact does not fire in those modes
  - after the motion stops the voice goes silent
  - v2.0: pairs with no preset (held stick -> wooden ball, two equal glass
    marbles) sound within 200 ms of Play (cold build, no prewarm); Render
    "Target only" plays the target; changing the object's Size while it
    rings never touches the ring: the render equals the same hit without
    the change (ring_ref) within -60 dB
  - v2.3: a held ball asked to Roll slides (and the Speed label says so);
    a long metal ring (1 m steel stick on the ground) is not cut after 3 s
    (no 5 ms frame drops > 20 dB while above -90 dBFS); the render capture
    (PROCAUDIO_CAPTURE=1) writes the last Play as a WAV that matches it
  - v2.4: Fit Length. A steel-ball hit fitted to 0.15 s equals the unfitted
    hit (ring_ref) until its fade, ends below -50 dB re its peak in the last
    2 ms before the end and is silent (< -120 dBFS) after; a roll fitted to
    0.4 s likewise. The Fit Length dial says what it does.

    python cpp/tests/fmod_host_check.py
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "cpp" / "build" / "plugins" / "fmod" / "Release"
OUT = ROOT / "cpp" / "build" / "fmod_host"
BLOCK = 1024


def db(x: float) -> float:
    return 20 * np.log10(x + 1e-30)


def frames(s, fs, w=0.005):
    n = int(w * fs)
    m = len(s) // n
    return np.sqrt(np.mean(s[:m * n].reshape(m, n) ** 2, axis=1))


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    wav, segs = OUT / "fmod_render.wav", OUT / "segments.txt"
    cap_dir = OUT / "captures"                       # the render capture, forced on
    for old in cap_dir.glob("render_*.wav"):
        old.unlink()
    env = dict(os.environ, PROCAUDIO_CAPTURE="1", PROCAUDIO_CAPTURE_DIR=str(cap_dir))
    r = subprocess.run([str(BIN / "procaudio_fmod_host.exe"), str(BIN / "procaudio_fmod.dll"),
                        str(wav), str(segs)], capture_output=True, text=True, env=env)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(r.stderr)
        return 1
    ok = '(same as object) -> "same as object"' in r.stdout   # ignored dials say so
    print(f"  {'PASS' if ok else 'FAIL'}  Tgt dials read 'same as object' when Target = Same as object")
    good = 'label Fit Length = 0.15 -> "ends at 0.15 s (45 ms fade)"' in r.stdout
    ok &= good
    print(f"  {'PASS' if good else 'FAIL'}  the Fit Length dial reads 'ends at 0.15 s (45 ms fade)'")
    good = """(held ball, Roll) -> "a held ball can't turn\"""" in r.stdout
    ok &= good
    print(f"  {'PASS' if good else 'FAIL'}  held ball + Roll: the Speed label reads \"a held ball can't turn\"")
    y, fs = sf.read(str(wav), always_2d=True)
    y = y.mean(axis=1)
    spans = {}
    for line in segs.read_text().split("\n"):
        if not line.strip():
            continue
        label, t0, t1 = line.split()
        spans[label] = (float(t0), float(t1))
        if label in ("gap", "size_swap", "ring_ref"):    # gap: stop + release between Plays
            continue
        if label == "size_swap_after":                   # the ring across the Size change
            t0 = spans["size_swap"][0]
        s = y[int(float(t0) * fs):int(float(t1) * fs)]
        fr = frames(s, fs)
        rms, peak = np.sqrt(np.mean(s ** 2)), np.max(np.abs(s))
        clipped = int(np.sum(np.abs(s) >= 0.999))
        sounding = np.nonzero(fr > 10 ** (-70 / 20))[0]
        first_ms = sounding[0] * 5.0 if len(sounding) else float("nan")
        notes, good = [], bool(np.all(np.isfinite(s))) and clipped == 0
        if label == "stopped":
            tail = db(np.sqrt(np.mean(s[-int(fs):] ** 2)))
            good &= tail < -80
            notes.append(f"last 1 s {tail:.0f} dBFS")
        else:
            # sounding: rms for continuous sound; a lone click (two marbles) has a
            # clear peak but a tiny rms over the whole segment
            good &= rms > 10 ** (-70 / 20) or peak > 10 ** (-50 / 20)
            if label.startswith("pair_"):                       # cold build, no prewarm
                good &= first_ms <= 200.0
                notes.append(f"first sound {first_ms:.0f} ms after Play")
            elif (label.startswith(("impact", "drop", "render_target", "size_swap"))
                    and label != "impact_retrigger"):
                good &= first_ms <= 1000.0 * BLOCK / fs + 5.0   # within one mix block
                notes.append(f"first sound {first_ms:.0f} ms after Play")
            if label == "size_swap_after":
                # the old voice rings out untouched and the new one is silent
                # until its next contact: identical to the unchanged hit
                # separate Plays start a few samples apart: align first
                r0 = int(spans["ring_ref"][0] * fs)
                n = int(0.1 * fs)
                lags = range(-16, 17)
                r0 += max(lags, key=lambda k: float(np.dot(s[:n], y[r0 + k:r0 + k + n])))
                ref = y[r0:r0 + len(s)]
                diff = db(np.sqrt(np.mean((s - ref) ** 2))) - db(np.sqrt(np.mean(ref ** 2)))
                good &= diff < -60.0
                notes.append(f"differs from the unchanged ring by {diff:.0f} dB")
            elif label == "long_ring":
                lv = db(fr)
                cliff = min((lv[i] - lv[i - 1] for i in range(100, len(lv)) if lv[i - 1] > -90.0),
                            default=0.0)
                audible = np.nonzero(lv > -90.0)[0]
                end_s = (audible[-1] + 1) * 0.005 if len(audible) else 0.0
                good &= cliff > -20.0 and end_s > 3.5
                notes.append(f"rings to {end_s:.1f} s (> -90 dBFS), largest 5 ms drop {cliff:+.1f} dB")
            elif label in ("fit_hit", "fit_roll"):
                L = 0.15 if label == "fit_hit" else 0.4
                fade = min(max(0.3 * L, 0.005), 0.1)
                if label == "fit_hit":
                    # separate Plays start a few samples apart: align on ring_ref
                    r0 = int(spans["ring_ref"][0] * fs)
                    n = int(0.05 * fs)
                    k = max(range(-16, 17), key=lambda k: float(np.dot(s[:n], y[r0 + k:r0 + k + n])))
                    pre = int((L - 0.001 - fade) * fs) - 32         # before the fade starts
                    a, ref = s[:pre], y[r0 + k:r0 + k + pre]
                    diff = db(np.sqrt(np.mean((a - ref) ** 2))) - db(np.sqrt(np.mean(ref ** 2)))
                    good &= diff < -60.0
                    notes.append(f"before the fade differs from the unfitted hit by {diff:.0f} dB")
                end = int(L * fs)
                pk = np.max(np.abs(s[:end]))
                last2 = db(np.max(np.abs(s[end - int(0.003 * fs):end - int(0.001 * fs)]))) - db(pk)
                after = db(np.max(np.abs(s[end + 16:])))
                good &= last2 < -50.0 and after < -120.0
                notes.append(f"last 2 ms {last2:.0f} dB re peak, after the end {after:.0f} dBFS")
            elif label in ("roll", "slide", "held_roll"):     # starts gently (80 ms)
                # held_roll: a rounded slider's tables are not prewarmed (they
                # depend on its radius and material): ~0.7 s on the first Play
                good &= first_ms <= (1000.0 if label == "held_roll" else 300.0)
                notes.append(f"audible {first_ms:.0f} ms after Play")
        if label.startswith(("impact", "drop", "pair_", "render_target", "size_swap")):
            tail = db(np.sqrt(np.mean(s[-int(0.25 * fs):] ** 2)))
            good &= tail < db(rms) - 20                 # a one-shot: it dies away
            notes.append(f"tail {tail - db(rms):+.0f} dB re segment rms")
        if label in ("roll", "slide"):
            onset = db(fr[:int(0.6 / 0.005)].max()) - db(fr[int(1.0 / 0.005):].max())
            good &= onset < 6.0                          # no hit at the start
            notes.append(f"onset {onset:+.1f} dB re steady peak")
        ok &= good
        print(f"  {'PASS' if good else 'FAIL'}  {label:16s} rms {db(rms):6.1f} dBFS, peak "
              f"{db(peak):6.1f} dBFS, clipped {clipped}; " + ", ".join(notes))
    # the capture of the last Play (long_ring) against what FMOD rendered
    last = cap_dir / "last_render.txt"
    good, note = False, "no capture written"
    if last.exists():
        cfile = Path(last.read_text().strip())
        c, cfs = sf.read(str(cfile))
        t0, t1 = spans["long_ring"]
        ref = y[int(t0 * fs):int(t1 * fs)]
        n = min(len(c), len(ref))
        d_db = db(np.sqrt(np.mean(c[:n] ** 2))) - db(np.sqrt(np.mean(ref[:n] ** 2)))
        good = cfs == fs and len(c) > 3.5 * fs and abs(d_db) < 0.5 and "long" not in cfile.name
        note = (f"{cfile.name}: {len(c) / cfs:.1f} s, level {d_db:+.2f} dB re the FMOD render; "
                f"{len(list(cap_dir.glob('render_*.wav')))} renders kept")
    ok &= good
    print(f"  {'PASS' if good else 'FAIL'}  render capture   {note}")
    print(f"fmod host check: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
