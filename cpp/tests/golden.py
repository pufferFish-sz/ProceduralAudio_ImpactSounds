"""Golden tests: the C++ core against the Python prototype (the reference).

1. Exact paths, same inputs on both sides:
   modal   ModalBank on a force signal                 -> sample error
   impact  implicit Hunt-Crossley solve                -> force, contact time
   roll    roll_force on the SAME surface table        -> force correlation
   slide   profile_slide_force on the SAME table       -> force correlation
   Python's surface_profile is wrapped to produce 2^k-point tables, so the
   C++ periodic spline can read them; random parts (noise, wobble) are off.
   Remaining differences are by design: analytic vs np.gradient
   derivatives, the causal decimator (aligned by its 10-sample delay) and
   periodic vs not-a-knot spline ends.
2. Scene statistics: C++ renders of roll_spheres and drag_boxes against the
   Python renders in proto/renders/scenes/ (texture_stats; different random
   surfaces, so only statistics can agree).

Run from the repo root after building cpp/ (Release):
    python cpp/tests/golden.py
"""
from __future__ import annotations

import glob
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
EXE = ROOT / "cpp" / "build" / "Release" / "procaudio_render.exe"
WORK = ROOT / "cpp" / "build" / "golden"
FS = 44100

from proto.analysis.texture_stats import load, ltas_distance, stats  # noqa: E402
from proto.core import impact as IMP  # noqa: E402
from proto.core import rolling as ROLL  # noqa: E402
from proto.core import slide_profile as SP  # noqa: E402
from proto.core.bodies import floor  # noqa: E402
from proto.core.modal_bank import ModalBank  # noqa: E402
from proto.scenes.roll import BALLS, solid_ball  # noqa: E402

results: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str) -> None:
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")


def case_dir(name: str) -> Path:
    d = WORK / name
    d.mkdir(parents=True, exist_ok=True)
    for f in d.glob("cpp_*"):
        f.unlink()
    return d


def put(d: Path, **arrays) -> None:
    for k, v in arrays.items():
        np.asarray(v, dtype="<f8").tofile(d / f"{k}.f64")


def params(d: Path, **kv) -> None:
    (d / "params.txt").write_text("".join(f"{k} {v}\n" for k, v in kv.items()))


def run(kind: str, d: Path) -> dict:
    subprocess.run([str(EXE), "golden", kind, str(d)], check=True)
    out = {}
    for line in (d / "cpp_stats.txt").read_text().splitlines():
        if line.strip():
            k, v = line.split()
            out[k] = float(v)
    return out


def get(d: Path, name: str) -> np.ndarray:
    return np.fromfile(d / f"{name}.f64", dtype="<f8")


def modes_arr(modes) -> np.ndarray:
    return np.array([[m.freq, m.decay, m.gain, m.mass] for m in modes]).ravel()


def corr(a, b):
    return float(np.corrcoef(a, b)[0, 1])


# ------------------------------------------------------------ 1. exact paths

def golden_modal():
    flr = floor("wood-floor")
    ball = solid_ball(0.03, "metal", "steel", "large", "")
    modes = flr.modes[:60] + ball.modes
    rng = np.random.default_rng(3)
    force = rng.standard_normal(FS) * np.exp(-np.arange(FS) / 4000)
    d = case_dir("modal")
    params(d, fs=FS)
    put(d, modes=modes_arr(modes), force=force)
    run("modal", d)
    ref = ModalBank(modes, FS).process(force)
    err = np.max(np.abs(get(d, "cpp_out") - ref)) / np.max(np.abs(ref))
    check("modal bank", err < 1e-9, f"max relative error {err:.1e}")


def golden_impact():
    flr = floor("wood-floor")
    for label, ball, v, mu in (
            ("steel ball r30mm, 1 m/s", solid_ball(0.03, "metal", "steel", "large", ""), 1.0, 0.3),
            ("wooden ball r50mm, 2 m/s", solid_ball(0.05, "wood", "wooden", "large", ""), 2.0, 0.6)):
        k = IMP.hertz_stiffness(ball.material, flr.material, ball.contact_radius)
        modes = ball.modes + flr.modes
        _, info = IMP._simulate_implicit(ball.mass, v, k, 1.5, mu * k, modes, FS, 0.05, 0.05)
        d = case_dir("impact")
        params(d, fs=FS, m_s=ball.mass, v_in=v, k=k, alpha=1.5, lam=mu * k, t_max=0.05,
               oversample=1)
        put(d, modes=modes_arr(modes))
        st = run("impact", d)
        fc = get(d, "cpp_force")
        fp = info["force"]
        n = min(len(fc), len(fp))
        err = np.max(np.abs(fc[:n] - fp[:n])) / fp.max()
        dct = abs(st["contact_time"] - info["contact_time"]) / info["contact_time"]
        dv = abs(st["v_out"] - info["v_out"]) / abs(info["v_out"])
        check(f"impact {label}", err < 1e-6 and dct < 1e-6 and dv < 1e-6,
              f"force err {err:.1e}, contact time {st['contact_time']*1e3:.3f} vs "
              f"{info['contact_time']*1e3:.3f} ms, v_out {st['v_out']:.4f} vs {info['v_out']:.4f}")


N_ROLL = 1 << 17
N_SLIDE = 1 << 18


def wrap_surface_profile(n_points):
    orig = SP.surface_profile

    def fixed(spec, length, sigma_c=SP.SIGMA_C, seed=0):
        return orig(spec, n_points * SP.DX * (1 + 1e-9), sigma_c, seed)
    return orig, fixed


def golden_roll():
    surf_name = "varnished-wood-floor"
    spec = SP.SURFACE_PROFILES[surf_name]
    flr = floor("wood-floor")
    t = np.arange(int(3.0 * FS)) / FS
    for R, mat, word, size, fin, rim in (BALLS[3], BALLS[2]):
        ball = solid_ball(R, mat, word, size, fin)
        k, alpha = ROLL.contact_law("sphere", R, 0.0, ball.material, flr.material)
        d0 = ROLL.static_contact(k, alpha, ball.mass)[0]
        a_h = np.sqrt(R * d0)
        orig, fixed = wrap_surface_profile(N_ROLL)
        ROLL.surface_profile = fixed
        try:
            z = fixed(spec, 0.0, sigma_c=SP.DX, seed=0)
            s_tab = ROLL.gaussian_smooth(ROLL.dilate_by_circle(z, R), max(a_h, SP.DX))
            speed = 0.59 * (0.5 - 0.5 * np.cos(np.pi * np.clip((t - 0.1) / 0.3, 0, 1)))
            fp, info = ROLL.roll_force(speed, FS, spec, R, ball.mass, k, alpha, 0.22,
                                       obj_spec=None, seed=0)
        finally:
            ROLL.surface_profile = orig
        d = case_dir("roll")
        params(d, fs=FS, radius=R, mass=ball.mass, k=k, alpha=alpha, zeta=0.22,
               oversample=info["oversample"], rim_dx=2 * np.pi * R / 8)
        put(d, surface=s_tab, rim=np.zeros(8), speed=speed)
        st = run("roll", d)
        fc = get(d, "cpp_force")[10:]                 # causal decimator delay
        n = min(len(fc), len(fp))
        a, b = fp[int(0.5 * FS):n], fc[int(0.5 * FS):n]
        c = corr(a, b)
        ratio = np.std(b) / np.std(a)
        hops = info["airborne"] > 0.05
        if hops:     # chaotic hopping: compare statistics, not waveforms
            ok = (abs(st["airborne"] - info["airborne"]) < 0.35 * info["airborne"] and
                  abs(st["landings_per_s"] - info["landings_per_s"]) <
                  0.35 * info["landings_per_s"] and 0.6 < ratio < 1.6)
        else:
            ok = c > 0.99 and abs(ratio - 1) < 0.05
        check(f"roll {ball.slug}", ok,
              f"force corr {c:.4f}, rms ratio {ratio:.3f}; airborne {st['airborne']:.1%} vs "
              f"{info['airborne']:.1%}, landings {st['landings_per_s']:.0f} vs "
              f"{info['landings_per_s']:.0f}/s" + (" (hopping: statistics)" if hops else ""))


def golden_slide():
    t = np.arange(int(3.0 * FS)) / FS
    pull = 0.5 * (0.5 - 0.5 * np.cos(np.pi * np.clip((t - 0.1) / 0.6, 0, 1)))
    cases = [("steel box on concrete, 1 track, 4x", "concrete-slab", "metal", "concrete",
              2.01 * 9.81, 0.0097, 60e-6, 4),
             ("wood plank on wood floor, 1 track, 1x", "wood-floor", "wood", "wood",
              0.42 * 9.81, 0.010, 1e-4, 1),
             ("wood plank on wood floor, 1 track, 4x", "wood-floor", "wood", "wood",
              0.42 * 9.81, 0.010, 1e-4, 4)]
    for label, surf, smat, fmat, N0, m_p, sig, os_ in cases:
        spec = SP.SURFACE_PROFILES[surf]
        orig, fixed = wrap_surface_profile(N_SLIDE)
        SP.surface_profile = fixed
        try:
            tab = fixed(spec, 0.0, sigma_c=sig, seed=0)
            fp, info = SP.profile_slide_force(pull, FS, spec, N0, m_p, smat, fmat,
                                              wobble=0.0, seed=0, oversample=os_,
                                              sigma_c=sig)
        finally:
            SP.surface_profile = orig
        d = case_dir("slide")
        params(d, fs=FS, N0=N0, m_p=m_p, slider_mat=smat, floor_mat=fmat, wobble=0.0,
               oversample=os_, n_tracks=1)
        put(d, tab0=tab, velocity=pull)
        st = run("slide", d)
        delay = 10 if os_ > 1 else 0
        fc = get(d, "cpp_force")[delay:]
        n = min(len(fc), len(fp))
        a, b = fp[int(0.8 * FS):n], fc[int(0.8 * FS):n]
        c = corr(a, b)
        ratio = np.std(b) / np.std(a)
        lifts = info["lift_off_fraction"] > 0.01
        if lifts:
            ok = 0.7 < ratio < 1.4 and abs(st["lift_off"] - info["lift_off_fraction"]) < \
                0.35 * info["lift_off_fraction"] + 0.005
        else:
            ok = c > 0.99 and abs(ratio - 1) < 0.05
        check(f"slide {label}", ok,
              f"force corr {c:.4f}, rms ratio {ratio:.3f}; lift-off {st['lift_off']:.1%} vs "
              f"{info['lift_off_fraction']:.1%}" + (" (lift-offs: statistics)" if lifts else ""))


# ------------------------------------------------------------ 2. scenes

def golden_scenes():
    from proto.scenes.drag import steady_pull
    speed_file = WORK / "pull_speed.f64"
    np.asarray(steady_pull(), dtype="<f8").tofile(speed_file)
    for fam in ("roll_spheres", "drag_boxes"):
        out = WORK / "scenes" / fam
        # drags: both sides use the SAME (Python) pull-speed track; the
        # texture brightness follows speed, so two random speed paths alone
        # move the spectrum by several dB
        extra = [str(speed_file)] if fam == "drag_boxes" else []
        subprocess.run([str(EXE), "scene", fam, str(out), str(FS), *extra], check=True,
                       stdout=subprocess.DEVNULL)
        py_dir = ROOT / "proto" / "renders" / "scenes" / fam
        # the pulled distance in drag names depends on each side's random hand
        # variation (numpy vs PCG32), so match names without it
        key = lambda s: re.sub(r"pulled-[\d.]+m", "pulled-m", s)  # noqa: E731
        py_files = {key(Path(f).stem): f for f in glob.glob(str(py_dir / "*.wav"))}
        rows = []
        for f in sorted(glob.glob(str(out / "*.wav"))):
            name = key(Path(f).stem)
            if name not in py_files:
                check(f"scene {fam}/{name}", False, "no Python render with this name")
                continue
            yc, _ = load(f)
            yp, _ = load(py_files[name])
            sc, sp = stats(yc), stats(yp)
            rows.append((name, sc, sp, np.std(yc), np.std(yp)))
        # family-relative levels (both families share one gain internally)
        lc0 = max(r[3] for r in rows)
        lp0 = max(r[4] for r in rows)
        for name, sc, sp, rc, rp in rows:
            dist = ltas_distance(sc, sp)
            dlev = 20 * np.log10(rc / lc0) - 20 * np.log10(rp / lp0)
            # scenes made of a few random events (board-joint clunks: kurtosis
            # in the hundreds) vary by realization: Python vs Python with other
            # surface seeds already differs by up to 4.8 dB there (2026-11-06),
            # continuous textures by < 1 dB
            sparse = max(sc["kurtosis"], sp["kurtosis"]) > 100
            ok = dist < (8.0 if sparse else 6.0) and abs(dlev) < 6.0
            check(f"scene {name[:58]}", ok,
                  f"LTAS shape distance {dist:.1f} dB, relative level {dlev:+.1f} dB, "
                  f"centroid {sc['centroid']:.0f} vs {sp['centroid']:.0f} Hz, "
                  f"kurtosis {sc['kurtosis']:.0f} vs {sp['kurtosis']:.0f}")


def main():
    if not EXE.exists():
        sys.exit(f"build the C++ core first: {EXE} not found")
    print("exact paths:")
    golden_modal()
    golden_impact()
    golden_roll()
    golden_slide()
    print("scene statistics (C++ vs Python renders):")
    golden_scenes()
    n_ok = sum(ok for _, ok, _ in results)
    print(f"golden: {n_ok}/{len(results)} PASS")
    return 0 if n_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
