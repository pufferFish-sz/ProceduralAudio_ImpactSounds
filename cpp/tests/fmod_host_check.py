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

    python cpp/tests/fmod_host_check.py
"""
from __future__ import annotations

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
    r = subprocess.run([str(BIN / "procaudio_fmod_host.exe"), str(BIN / "procaudio_fmod.dll"),
                        str(wav), str(segs)], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(r.stderr)
        return 1
    y, fs = sf.read(str(wav), always_2d=True)
    y = y.mean(axis=1)
    ok = True
    for line in segs.read_text().split("\n"):
        if not line.strip():
            continue
        label, t0, t1 = line.split()
        if label == "gap":                               # stop + release between Plays
            continue
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
            good &= rms > 10 ** (-70 / 20)
            if label.startswith(("impact", "drop")) and label != "impact_retrigger":
                good &= first_ms <= 1000.0 * BLOCK / fs + 5.0   # within one mix block
                notes.append(f"first sound {first_ms:.0f} ms after Play")
            elif label in ("roll", "slide"):                   # starts gently (80 ms)
                good &= first_ms <= 300.0
                notes.append(f"audible {first_ms:.0f} ms after Play")
        if label.startswith(("impact", "drop")):
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
    print(f"fmod host check: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
