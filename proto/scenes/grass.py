"""Scene families: objects dropped, rolled and slid on grass, footsteps on
grass, and the same balls dropped on grass vs concrete.

Everything is driven by contact physics (core/grass.py) and comes out in
Pa at 1 m, so each family shares one gain AND every file lists its
absolute level (peak dB SPL, LAFmax, LAeq): a light ball and a heavy crate,
or a ball and a walker, can be compared by number across families.

Stems:
- <object>       the object's own sound: its modes + rigid-body click,
                 driven by the canopy + soil contact force (core/bodies).
                 For boxes, the panel's "forced radiation" term is left out:
                 it is the near field of a POINT force on a plate, and a
                 box landing on its face is pushed over the whole face. The
                 face's piston radiation is the turf-thud stem instead.
- turf-thud      the dent pushed into the ground (monopole, core/grass)
- grass-rustle   swish + crackle grains, power = eta x canopy loss power

Recordings: proto/recordings/Grass/ (none yet). When files whose names
contain a scene's match key exist, the scene adds render-vs-recording
texture statistics and figures/grass_vs_recordings.png.

Run:  python -m proto.scenes.grass
"""
from dataclasses import replace
from pathlib import Path

import numpy as np

from ..analysis.levels import levels
from ..analysis.texture_stats import compare_line, load, pair_figure, stats
from ..core.bodies import Body, body_sound, floor, with_contact_damping
from ..core.grass import (GRASS, GRASS_PRESETS, blade_numbers, clegg_gmax,
                          drop_on_grass, roll_drive, rustle, slide_drive,
                          turf_contact_law)
from ..core.materials import PHYSICAL
from ..core.rolling import roll_force
from ..core.shapes import plate_ss
from ..util import FIG_DIR, FS
from .common import G, Scene, bounce_scene, write_family
from .drag import PULL_DUR, box, steady_pull
from .roll import baseball, solid_ball

REC_DIR = Path(__file__).resolve().parents[1] / "recordings" / "Grass"

# lumpy lawn soil under the blades, as the rolling contact reads it: ~0.5 mm
# rms lumps, energy at long wavelengths (cm scale). Estimate, not measured.
TURF_PROFILE = dict(h_rms=0.5e-3, gamma=2.6, k_r=100.0, q=1.0,
                    reson_share=0.0, features=None,
                    note="lumpy lawn soil, 0.5 mm rms (estimate)")

# walker (footsteps)
BODY_MASS = 75.0
HEEL = dict(mass=5.0, v=0.4, area=0.005, ramp=0.01,
            # shoe heel: EVA foam midsole, E ~ 3 MPa (rounded)
            mat=dict(E=3e6, nu=0.30))
FOOT_AREA, TOE_AREA = 0.025, 0.006       # m^2, flat shoe sole / toe
TOE_SLIP = 1e-3                          # m the toe slips at push-off (guess)
N_STEPS, STEP_DT = 4, 0.55
WALK_DUR = 0.3 + N_STEPS * STEP_DT + 0.6


def _ramp(t, a, b):
    return np.clip((t - a) / (b - a), 0.0, 1.0)


def _scene_levels(sc: Scene, span=None) -> str:
    lv = levels(sc.mix, FS, span)
    parts = [f"level at 1 m: peak {lv['peak_db']:.0f} dB SPL, LAFmax "
             f"{lv['lafmax']:.0f} dB(A), LAeq {lv['laeq']:.0f} dB(A)"
             + (f" over {span[0]:g}-{span[1]:g} s" if span else "")]
    per = [f"{k} {levels(v, FS, span)['laeq']:.0f}" for k, v in sc.stems.items()]
    parts.append("LAeq per stem (dB(A)): " + ", ".join(per))
    sc.levels = lv
    return "; ".join(parts)


def _object_sound(obj: Body, force: np.ndarray) -> np.ndarray:
    out = body_sound(obj, force, FS, -1.0)
    return out["modes"] + out["click"]


# ------------------------------------------------------------------ drops

def drop_scene(obj: Body, shape: tuple, h0: float, g: dict = GRASS,
               dur: float = 1.5, seed: int = 1) -> Scene:
    r = drop_on_grass(obj.mass, shape, obj.material, FS, dur, t0=0.1, h0=h0, g=g,
                      modes=obj.modes)
    rs = rustle(r["power"], r["rate"], r["load"], FS, g, seed)
    info = r["info"]
    stems = {obj.slug: _object_sound(obj, r["force"]), "turf-thud": r["dent"],
             "grass-rustle": rs["swish"] + rs["crackle"]}
    k_word = ("Hertz sphere, k = %.2e N/m^1.5" % info["k"] if shape[0] == "sphere"
              else "flat face, k = %.2e N/m when fully down, lands tilted: "
                   "contact grows over %.0f mm" % (info["k"], 1e3 * shape[2]))
    facts = [
        f"drop {h0:g} m above the grass tips -> {info['v_canopy']:.2f} m/s at "
        f"the canopy, {info['v_soil']:.2f} m/s at the soil; "
        f"{info['n_bounces']} landing(s)",
        f"canopy ({g['canopy_h']*100:.0f} cm, pushes back {info['p_canopy']:.0f} Pa) "
        f"took {info['W_canopy']*1e3:.1f} mJ of {info['E_in']*1e3:.0f} mJ",
        f"soil contact: {k_word}; first contact {info['contact_time']*1e3:.1f} ms, "
        f"peak force {info['F_peak']:.0f} N ({info['F_peak']/(obj.mass*G):.0f} g), "
        f"restitution {info['restitution']:.2f} (target "
        f"{info['restitution_target']:g})",
        f"rustle: acoustic energy {rs['acoustic_energy_J']*1e9:.1f} nJ",
    ]
    sc = Scene(name="", what="", mover=obj, target=None, stems=stems, facts=facts)
    sc.facts.append(_scene_levels(sc))
    return sc


FACE_CONTACT_LOSS = 0.10   # drag.CONTACT_LOSS: a face lying on a floor
                           # (fit to the sliding recordings); soil is lossier


def flat_box(a, b, c, material, wall, loss, word, joint) -> Body:
    """Box landing on (or sliding on) its bottom face. The ground pushes the
    WHOLE face, so the bottom panel's modes are driven by a uniform load
    (shapes.plate_ss load="uniform": only odd-odd modes, at their plate
    average), and only the part of the load the panel's own inertia does
    not take bends it: (1 - M_panel / M_box). The face lies on the ground,
    so its modes also get contact damping (bodies.with_contact_damping)."""
    bx = box(a, b, c, material, wall, loss, word, joint)
    phys = PHYSICAL[material]
    modes, m_panel = plate_ss(a, b, wall, material, tan_phi=loss,
                              f_max=10000.0, load="uniform")
    L = 1.0 - m_panel / bx.mass
    modes = [replace(md, mass=md.mass / L**2) for md in modes]
    bx = replace(bx, modes=modes, description=bx.description.replace(
        "sliding on its bottom panel", "landing on / sliding on its bottom face"),
        notes=[bx.notes[0], f"face load: uniform over the bottom, {L:.0%} of "
               "it bends the panel (the rest moves the panel's own mass)"])
    return with_contact_damping(bx, FACE_CONTACT_LOSS,
                                "face lying on the ground")


def cardboard_box() -> Body:
    return flat_box(0.30, 0.20, 0.20, "cardboard", 0.004, 0.05, "cardboard",
                    "glued and taped")


def plywood_crate() -> Body:
    return flat_box(0.40, 0.30, 0.25, "wood", 0.012, 0.03, "plywood",
                    "glued")


def wooden_ball() -> Body:
    return solid_ball(0.05, "wood", "wooden", "large", "turned hardwood")


DROPS = [  # (body factory, shape builder, drop height m, words)
    (baseball, lambda b: ("sphere", b.contact_radius), 1.0, "baseball"),
    (wooden_ball, lambda b: ("sphere", b.contact_radius), 1.0,
     "solid wooden ball"),
    # tilted 5 deg: contact grows over (short side) x sin 5 deg
    (cardboard_box, lambda b: ("flat", b.area, 0.20 * np.sin(np.radians(5))),
     0.5, "empty cardboard box"),
    (plywood_crate, lambda b: ("flat", b.area, 0.30 * np.sin(np.radians(5))),
     0.5, "empty plywood crate"),
]


# ------------------------------------------------------------------ rolls

def roll_scene(obj: Body, v0: float, g: dict = GRASS, dur: float = 2.5,
               seed: int = 2) -> Scene:
    """A solid ball rolled across the lawn at v0; it slows by rolling
    resistance (a solid sphere decelerates at c_rr g / 1.4)."""
    R = obj.contact_radius
    t = np.arange(int(dur * FS)) / FS
    dec = g["c_rr"] * G / 1.4
    ramp = 0.5 - 0.5 * np.cos(np.pi * _ramp(t, 0.1, 0.2))
    speed = ramp * np.maximum(v0 - dec * np.maximum(t - 0.1, 0.0), 0.0)
    power, rate, load_b, dinfo = roll_drive(speed, obj.mass, R, g)
    rs = rustle(power, rate, load_b, FS, g, seed)
    k, alpha = turf_contact_law(("sphere", R), obj.material, g)
    e = g["restitution"]["sphere"]
    zeta = -np.log(e) / np.sqrt(np.pi ** 2 + np.log(e) ** 2)
    F, info = roll_force(speed, FS, TURF_PROFILE, R, obj.mass, k, alpha, zeta)
    F[t < 0.1] = 0.0
    stems = {obj.slug: _object_sound(obj, F),
             "grass-rustle": rs["swish"] + rs["crackle"]}
    facts = [
        f"rolls from {v0:g} m/s, stops after {v0/dec:.2f} s / "
        f"{v0**2/(2*dec):.2f} m (rolling resistance c_rr {g['c_rr']})",
        f"sweeps a {dinfo['width']*100:.1f} cm strip of blades: "
        f"{g['shoots_per_m2']*dinfo['width']*v0:.0f} blades/s at the start; "
        f"loss per blade {dinfo['e_per_blade']*1e3:.2f} mJ (crushing one "
        f"blade flat: {blade_numbers(g)['e_crush']*1e3:.2f} mJ)",
        f"soil contact under the ball: static sink {info['delta0']*1e3:.2f} mm, "
        f"contact resonance {info['f_contact']:.0f} Hz, airborne "
        f"{info['airborne']:.1%} of the time, {info['landings_per_s']:.0f} "
        f"landings/s on the {TURF_PROFILE['note']}",
        f"rustle: acoustic energy {rs['acoustic_energy_J']*1e9:.1f} nJ",
    ]
    sc = Scene(name="", what="", mover=obj, target=None, stems=stems, facts=facts)
    sc.facts.append(_scene_levels(sc))
    return sc


# ------------------------------------------------------------------ slides

def slide_scene(obj: Body, g: dict = GRASS, seed: int = 3) -> Scene:
    v = steady_pull()
    N = obj.mass * G
    power, rate, load_b = slide_drive(v, N, obj.area, g)
    rs = rustle(power, rate, load_b, FS, g, seed)
    stems = {"grass-rustle": rs["swish"] + rs["crackle"]}
    vm = float(np.median(v[v > 0.2]))
    facts = [
        f"pulled {PULL_DUR:g} s at ~{vm:.2f} m/s (scenes/drag.steady_pull); "
        f"friction mu_k {g['mu_k']} -> {g['mu_k']*N*vm:.2f} W lost in the grass",
        f"face {obj.area*1e4:.0f} cm^2 presses {N/obj.area:.0f} Pa "
        f"(canopy holds {blade_numbers(g)['p_canopy']:.0f} Pa): "
        + ("rides on the blades" if N / obj.area < 1.5 * blade_numbers(g)["p_canopy"]
           else "crushes the blades to the soil")
        + f"; {g['shoots_per_m2']*obj.area*vm/g['slip_len']:.0f} blade "
        f"slip events/s",
        f"rustle: acoustic energy {rs['acoustic_energy_J']*1e9:.1f} nJ",
        "not modelled yet: the box's own panel driven by the blade flicks "
        "(only the rustle is heard)",
    ]
    sc = Scene(name="", what="", mover=obj, target=None, stems=stems, facts=facts)
    sc.facts.append(_scene_levels(sc))
    return sc


# ------------------------------------------------------------------ walk

def walk_scene(g: dict = GRASS, seed: int = 3) -> Scene:
    """Four steps. Per step: heel strike (5 kg heel at 0.4 m/s, solved on
    the turf: thud + canopy rustle), the sole rolling down to flat (blades
    crushed: n_A dA/dt blades/s, e_crush each), unloading to the toe
    (blades spring back: springback x e_crush), and a toe slip of TOE_SLIP
    at push-off (mu_k N v)."""
    rng = np.random.default_rng(seed)
    n = int(WALK_DUR * FS)
    t = np.arange(n) / FS
    bn = blade_numbers(g)
    nA = g["shoots_per_m2"]
    power, rate, load_b = np.zeros(n), np.zeros(n), np.zeros(n)
    force, dent = np.zeros(n), np.zeros(n)
    heel_info = None
    for i in range(N_STEPS):
        t0 = 0.3 + i * STEP_DT + rng.uniform(-0.015, 0.015)
        gain = rng.uniform(0.9, 1.1)
        r = drop_on_grass(HEEL["mass"], ("flat", HEEL["area"], HEEL["ramp"]),
                          HEEL["mat"], FS, WALK_DUR, t0=t0,
                          v_in=HEEL["v"] * gain, g=g)
        heel_info = heel_info or r["info"]
        m = min(n, len(r["force"]))
        force[:m] += r["force"][:m]
        dent[:m] += r["dent"][:m]
        power[:m] += r["power"][:m]
        rate[:m] += r["rate"][:m]
        # sole contact area beyond the heel: down to flat in 0.1 s, unload
        # to the toe 0.40-0.55 s, toe off by 0.62 s
        s = lambda a, b: 0.5 - 0.5 * np.cos(np.pi * _ramp(t, t0 + a, t0 + b))
        A = (HEEL["area"] + (FOOT_AREA - HEEL["area"]) * s(0.0, 0.10)
             - (FOOT_AREA - TOE_AREA) * s(0.40, 0.55) - TOE_AREA * s(0.55, 0.62))
        A[t < t0] = 0.0
        dA = np.gradient(A) * FS
        press = np.maximum(dA, 0.0)
        rel = np.maximum(-dA, 0.0)
        power += nA * bn["e_crush"] * gain * (press + g["springback"] * rel)
        rate += nA * (press + rel)
        # toe slip at push-off: raised-cosine speed, integral TOE_SLIP
        win = (t > t0 + 0.48) & (t < t0 + 0.56)
        v_slip = np.where(win, (1 - np.cos(2 * np.pi * (t - t0 - 0.48) / 0.08))
                          * TOE_SLIP / 0.08, 0.0)
        N_toe = 0.6 * BODY_MASS * G
        power += g["mu_k"] * N_toe * v_slip
        rate += nA * TOE_AREA * v_slip / g["slip_len"]
        stance = (t >= t0) & (t < t0 + 0.62)
        load_b[stance] = np.maximum(load_b[stance], BODY_MASS * G /
                                    (nA * np.maximum(A[stance], TOE_AREA)))
    rs = rustle(power, rate, load_b, FS, g, seed)
    stems = {"turf-thud": dent, "grass-rustle": rs["swish"] + rs["crackle"]}
    e_step_press = nA * (FOOT_AREA - HEEL["area"]) * bn["e_crush"]
    facts = [
        f"walker {BODY_MASS:g} kg, {N_STEPS} steps {STEP_DT:g} s apart; heel "
        f"strike {HEEL['mass']:g} kg at {HEEL['v']:g} m/s on a foam heel: "
        f"peak {heel_info['F_peak']:.0f} N",
        f"per step the sole crushes {nA*FOOT_AREA:.0f} blades "
        f"({e_step_press*1e3:.0f} mJ), the toe slips {TOE_SLIP*1e3:g} mm "
        f"({g['mu_k']*0.6*BODY_MASS*G*TOE_SLIP*1e3:.0f} mJ); load "
        f"{BODY_MASS*G/(nA*FOOT_AREA):.1f} N per blade -> "
        f"{100*g['dryness']*(1-np.exp(-BODY_MASS*G/(nA*FOOT_AREA)/g['f_snap'])):.0f} "
        f"% of blade events snap (dryness {g['dryness']})",
        f"rustle: acoustic energy {rs['acoustic_energy_J']*1e9:.1f} nJ",
        "foot/shoe click and the swing of the leg through the grass are "
        "not modelled",
    ]
    sc = Scene(name="", what="", mover=None, target=None, stems=stems, facts=facts)
    sc.facts.append(_scene_levels(sc, (0.3, 0.3 + N_STEPS * STEP_DT)))
    return sc


# ------------------------------------------------------------------ recordings

# scene match key -> words a recording's file name must contain (any)
REC_KEYS = {"drop_ball": ("drop", "ball"), "drop_box": ("drop", "box"),
            "roll_ball": ("roll", "ball"), "slide_box": ("slide", "box"),
            "walk": ("walk", "step")}


def recordings_for(key: str) -> list[Path]:
    if not REC_DIR.exists():
        return []
    words = REC_KEYS[key]
    if key == "walk":
        return sorted(p for p in REC_DIR.glob("*.wav")
                      if any(w in p.name.lower() for w in words))
    return sorted(p for p in REC_DIR.glob("*.wav")
                  if all(w in p.name.lower() for w in words))


def compare(sc: Scene, key: str, pairs: list) -> None:
    recs = recordings_for(key)
    s_r = stats(sc.mix, FS)
    sc.facts.append(f"texture: {s_r['events_s']:.1f} clicks/s, centroid "
                    f"{s_r['centroid']:.0f} Hz, >1 kHz kurtosis "
                    f"{s_r['kurtosis']:.1f}, energy below 300 Hz "
                    f"{100*s_r['low_share']:.0f} %")
    if not recs:
        sc.facts.append("no grass recording yet for this case "
                        "(recordings/README.md, 'Grass')")
        return
    for rec in recs:
        y, fs_r = load(rec)
        s_rec = stats(y, fs_r)
        sc.facts.append(f"vs {rec.name}: " + compare_line(s_r, s_rec))
        pairs.append((sc.name, sc.mix, FS, y, fs_r, s_r, s_rec, rec.name))


# ------------------------------------------------------------------ main

def main():
    gm, tc = clegg_gmax()
    print(f"turf check: Clegg hammer reads {gm:.0f} g, {tc*1e3:.1f} ms")
    pairs = []
    drops = []
    for make, shape_of, h0, word in DROPS:
        obj = make()
        sc = drop_scene(obj, shape_of(obj), h0)
        sc.name = f"drop_{obj.slug}_from-{h0:g}m_onto_grass"
        sc.what = (f"a {word} ({obj.mass*1e3:.0f} g) dropped from {h0:g} m "
                   "onto a mown lawn: thud, short swish, a few small bounces")
        compare(sc, "drop_ball" if "ball" in word else "drop_box", pairs)
        drops.append(sc)
        print(f"  {sc.name}: {sc.facts[-2]}")
    write_family(
        "grass_drops", "Objects dropped onto grass",
        "Balls and empty boxes dropped onto a 5 cm mown lawn (core/grass.py). "
        "Thud = the dent pushed into the turf; rustle = blades crushed on the "
        "way down; the object's own ring is hardly excited because a soil "
        "contact lasts milliseconds. Turf stiffness calibrated to a Clegg "
        f"hammer reading of {gm:.0f} g.", drops)

    motion = []
    for make, v0 in ((baseball, 3.0), (wooden_ball, 3.0)):
        obj = make()
        sc = roll_scene(obj, v0)
        sc.name = f"roll_{obj.slug}_from-{v0:g}mps_on_grass"
        sc.what = (f"a {obj.description.split(',')[0]} rolled across a mown "
                   f"lawn from {v0:g} m/s until it stops")
        compare(sc, "roll_ball", pairs)
        motion.append(sc)
        print(f"  {sc.name}: {sc.facts[-2]}")
    for make in (cardboard_box, plywood_crate):
        obj = make()
        sc = slide_scene(obj)
        sc.name = f"slide_{obj.slug}_pulled_on_grass"
        sc.what = (f"an empty {obj.description.split(',')[0]} pulled across a "
                   "mown lawn in one long drag")
        compare(sc, "slide_box", pairs)
        motion.append(sc)
        print(f"  {sc.name}: {sc.facts[-2]}")
    for slug in ("lawn", "lawn-lush", "lawn-dry"):
        g = GRASS_PRESETS[slug]
        sc = walk_scene(g)
        sc.name = f"walk_{N_STEPS}-steps_every-{STEP_DT:g}s_on_{slug}"
        sc.what = (f"four footsteps on a mown lawn (dryness {g['dryness']}: "
                   "share of crushed blades that snap)")
        compare(sc, "walk", pairs)
        motion.append(sc)
        print(f"  {sc.name}: {sc.facts[-2]}")
    write_family(
        "grass_motion", "Rolling, sliding and walking on grass",
        "Balls rolled, boxes pulled and a person walking on a 5 cm mown lawn. "
        "The rustle's power is eta x the mechanical power lost in the grass "
        "(rolling resistance, friction, crushed blades), so heavier and faster "
        "is louder by physics.", motion)

    # grass vs a hard floor, same balls, same drop
    ab = []
    for make, e_hard in ((baseball, 0.55), (wooden_ball, None)):
        obj = make()
        sc = drop_scene(obj, ("sphere", obj.contact_radius), 1.0)
        sc.name = f"drop_{obj.slug}_from-1m_onto_grass"
        sc.what = f"{obj.description.split(',')[0]} dropped 1 m onto grass"
        ab.append(sc)
        hard = bounce_scene(obj, floor("concrete-slab"), 1.0, e_target=e_hard)
        hard.name = f"drop_{obj.slug}_from-1m_onto_concrete-slab"
        hard.what = (f"the same {obj.description.split(',')[0]} dropped 1 m "
                     "onto a concrete slab")
        hard.facts.append(_scene_levels(hard))
        ab.append(hard)
        print(f"  {sc.name} vs concrete: {sc.levels['lafmax']:.0f} vs "
              f"{hard.levels['lafmax']:.0f} dB(A) LAFmax")
    write_family(
        "grass_vs_hard", "The same balls dropped on grass and on concrete",
        "Each ball is dropped 1 m onto the lawn and onto a 15 cm concrete "
        "slab (scenes/common.bounce_scene; baseball restitution 0.55 on "
        "concrete, the MLB ball specification). One shared gain.", ab)

    if pairs:
        pair_figure(pairs, FIG_DIR / "grass_vs_recordings.png",
                    "Grass: render vs real recording")
    return drops + motion + ab


if __name__ == "__main__":
    main()
