"""Grass ground: a soft, lossy turf contact + a blade canopy + rustle, all
driven by contact physics. Every output is in the units of the rest of the
prototype: force in N, pressure in Pa at 1 m (core/radiation.py).

What grass is, mechanically (three layers, top to bottom):

1. CANOPY: ~15 000 blades per m^2, ~5 cm tall. A blade is a thin strip
   (3 mm x 0.3 mm, leaf E ~ 0.3 GPa). Clamped at the root it buckles under
   P_cr = pi^2 E I / (4 L^2) ~ 2 mN, so the whole canopy pushes back with
   only p_b = n_A P_cr ~ 30 Pa. That carries an empty cardboard box (30 Pa)
   but not a ball (a 145 g ball presses ~300 Pa): balls and feet go through
   to the soil, a light wide box rides on the blades.
   Crushing one blade flat costs e_crush ~ P_cr h_c ~ 0.1 mJ; a blade hit at
   speed v also gets kinetic energy (m_blade / 4) v^2 / 2 (cantilever tip
   mass). That energy is lost in the canopy: it is the RUSTLE's source.
2. TURF/SOIL: thatch + moist topsoil, a soft and lossy half-space. Contact
   law Hertz (sphere, k = 4/3 E* sqrt(R)) or flat punch (k = 2 E* sqrt(A/pi),
   linear). E (~1 MPa) is set so this model's drop of a Clegg impact hammer
   reads 70 g, a typical kept lawn (materials.PHYSICAL["turf"], clegg_gmax).
   Losses: the soil is plastic (elasto-plastic law, see drop_on_grass):
   it keeps a dent and returns only e^2 of the work (e = 0.3 for a ball,
   0.2 for a flat face).
   A soil contact lasts milliseconds (a hard floor: ~0.1-0.5 ms), so the
   force has almost nothing above a few hundred Hz: the object's own ring
   is hardly excited. That is the "muffled" grass sound.
3. GROUND SOUND ("thud"): a body whose bottom is below the original ground
   level, moving at v, changes the air volume above the ground at
   Q = A_section v (A_section = its cross-section at ground level: it is
   filling or leaving its dent). Near a rigid baffle that is a monopole,
   p = rho_0 dQ/dt / (2 pi r). The soil around the dent also moves; that
   is not counted (a lower bound).

RUSTLE (core/aggregate.grain_stream_pa): acoustic power = eta x the
mechanical power lost in the canopy, P_mech:
   impact / step : canopy force x speed (+ blade kinetic energy),
   roll          : c_rr N v   (rolling resistance on grass),
   slide         : mu_k N v   (sliding friction on grass).
ONE efficiency eta for every motion (the main assumption). Event rate =
blades disturbed per second; snaps (crackle) take a share p_snap of the
power, growing with load per blade (a foot snaps stems, a ball hardly) and
with dryness.

Numbers are order-of-magnitude estimates with a stated physical reading,
NOT measurements of one lawn. None has been checked against a grass
recording yet (proto/recordings/ has none); eta is set from one published
level (Fuda et al. 2015, walking on park grass 28.5 dB(A)), see GRASS.
"""
from __future__ import annotations

import numpy as np

from .aggregate import GRASS_GRAINS, grain_stream_pa
from .impact import effective_modulus
from .radiation import R_LISTEN, RHO_AIR

G = 9.81

# Kept lawn, ~5 cm, summer. See the module docstring for the reading of each.
GRASS: dict = dict(
    name="lawn",
    shoots_per_m2=15000.0,   # ~8 000 (1963 ryegrass count) .. 25 000+ (dense lawns)
    canopy_h=0.05,           # m, mown height
    blade_w=3e-3, blade_t=0.3e-3,     # m
    blade_E=0.3e9, blade_rho=900.0,   # leaf tissue, Pa and kg/m^3
    tip_spread=0.01,         # m: blade tips are not level (canopy-top roughness)
    springback=0.3,          # share of a crushed blade's resistance that returns
    turf="turf",             # materials.PHYSICAL key
    restitution=dict(sphere=0.3, flat=0.2),   # on the soil, first landing
    soil_ramp=0.002,         # m, uneven soil surface: a flat face's soil
                             # contact area grows over this depth
    c_rr=0.3,                # rolling resistance (see roll_drive)
    mu_k=0.35,               # sliding friction, smooth box bottom on grass
    slip_len=3e-3,           # m slid per blade stick-slip event (~blade width)
    dryness=0.3,             # max share of blade events that snap (0 lush .. 1 hay)
    f_snap=0.2,              # N per blade: load at which snapping gets likely
    # acoustic efficiency: share of the canopy's mechanical loss radiated as
    # sound. Set so the RUSTLE of the scenes' 4-step walk has LAeq 32 dB(A)
    # at 1 m (energy mean of 8 renders): Fuda et al. 2015 measured 28.5
    # dB(A) walking on park grass with a dummy head (ear ~1.5 m from the
    # feet: +3.5 dB at 1 m). Their value includes park background, so this
    # is an UPPER bound. NOTE: the modelled heel thud alone is 37 dB(A),
    # above their total (see docs/plugin_body_design.md Part 7). Recalibrate
    # from our own recordings (recordings/README.md, "Grass").
    eta=2.2e-8,
)

GRASS_PRESETS = {
    "lawn": GRASS,
    "lawn-lush": dict(GRASS, name="lawn-lush", dryness=0.05),
    "lawn-dry": dict(GRASS, name="lawn-dry", dryness=0.8),
}


def blade_numbers(g: dict = GRASS) -> dict:
    """Per-blade mechanics (see module docstring)."""
    I = g["blade_w"] * g["blade_t"] ** 3 / 12.0
    L = g["canopy_h"]
    p_cr = np.pi ** 2 * g["blade_E"] * I / (4.0 * L ** 2)
    m_b = g["blade_rho"] * g["blade_w"] * g["blade_t"] * L
    return dict(P_cr=p_cr, p_canopy=g["shoots_per_m2"] * p_cr,
                e_crush=p_cr * L, m_blade=m_b)


def snap_prob(load_per_blade, g: dict = GRASS):
    """Share of blade events that snap: dryness x (1 - exp(-F / f_snap))."""
    f = np.maximum(np.asarray(load_per_blade, dtype=float), 0.0)
    return g["dryness"] * (1.0 - np.exp(-f / g["f_snap"]))


def rustle(power_mech, rate, load_per_blade, fs: int, g: dict = GRASS,
           seed: int = 1) -> dict:
    """Swish + crackle pressure (Pa at 1 m) from the canopy's mechanical
    loss power (W), the blade event rate (1/s) and the load per blade (N)."""
    p_ac = g["eta"] * np.maximum(np.asarray(power_mech, dtype=float), 0.0)
    ps = snap_prob(load_per_blade, g)
    sw = grain_stream_pa(rate * (1 - ps), p_ac * (1 - ps), fs,
                         r_listen=R_LISTEN, seed=seed, **GRASS_GRAINS["swish"])
    cr = grain_stream_pa(rate * ps, p_ac * ps, fs, r_listen=R_LISTEN,
                         seed=seed + 7, **GRASS_GRAINS["crackle"])
    return {"swish": sw, "crackle": cr,
            "acoustic_energy_J": float(np.sum(p_ac) / fs)}


# ------------------------------------------------------------ roll / slide

def chord_width(radius: float, depth: float) -> float:
    """Width of a ball's cross-section `depth` above its lowest point."""
    d = min(depth, radius)
    return 2.0 * np.sqrt(max(2 * radius * d - d * d, 0.0))


def roll_drive(speed, mass: float, radius: float, g: dict = GRASS):
    """Rustle drive of a ball rolling through the canopy (resting on the
    soil): it pushes aside every blade in a strip of the ball's width.
    P = c_rr N v; rate = n_A w v; load per blade = N / (n_A w^2 pi / 4)."""
    v = np.maximum(np.asarray(speed, dtype=float), 0.0)
    N = mass * G
    w = chord_width(radius, g["canopy_h"])
    nA = g["shoots_per_m2"]
    power = g["c_rr"] * N * v
    rate = nA * w * v
    load = np.full_like(v, N / (nA * np.pi * w * w / 4))
    return power, rate, load, dict(width=w, e_per_blade=g["c_rr"] * N / (nA * w))


def slide_drive(speed, normal, area: float, g: dict = GRASS):
    """Rustle drive of a flat face sliding over grass: every blade under the
    face rubs; one stick-slip event per slip_len of travel.
    P = mu_k N v; rate = n_A A v / slip_len; load per blade = N / (n_A A)."""
    v = np.abs(np.asarray(speed, dtype=float))
    N = np.broadcast_to(np.asarray(normal, dtype=float), v.shape)
    nA = g["shoots_per_m2"]
    return (g["mu_k"] * N * v, nA * area * v / g["slip_len"],
            N / (nA * area))


# ------------------------------------------------------------ impacts

def turf_contact_law(shape: tuple, obj_mat, g: dict = GRASS):
    """(k, alpha) on the turf: ("sphere", R) Hertz, ("flat", A[, ramp])
    flat punch of the whole face (see drop_on_grass for the ramp)."""
    e_star = effective_modulus(obj_mat, g["turf"])
    if shape[0] == "sphere":
        return 4.0 / 3.0 * e_star * np.sqrt(shape[1]), 1.5
    return 2.0 * e_star * np.sqrt(shape[1] / np.pi), 1.0


def _smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3 - 2 * x)


ZETA_UNLOAD = 0.3     # light damping on the unloading branch (lets a body settle)


def drop_on_grass(mass: float, shape: tuple, obj_mat, fs: int, dur: float,
                  t0: float = 0.1, h0: float | None = None,
                  v_in: float | None = None, g: dict = GRASS,
                  oversample: int = 4, modes: list | None = None) -> dict:
    """An object falls through the canopy onto the turf and bounces to rest.

    shape: ("sphere", R) or ("flat", A[, ramp]). A flat face never lands
    perfectly flat: tilted by theta, its soil contact area grows from an
    edge to the whole face over ramp = side x sin(theta) of sinking (default
    g["soil_ramp"]); the stiffness grows with it, F = k sqrt(A(d)/A) d.
    (A perfectly flat 0.18 kg box face stops in 0.6 ms at ~1000 g: a slap
    no box on grass makes.)

    Soil = ELASTO-PLASTIC, not viscous. Loading follows the elastic law
    above; at the deepest point the soil keeps a dent and gives back only
    e^2 of the loading work along a steeper unloading curve
    F = F_max ((d - d_p) / (d_max - d_p))^n (n = alpha), so the restitution
    is e at any speed (plus ZETA_UNLOAD of damping on that branch, so a body
    comes to rest). Every new landing starts a fresh loading curve from the
    bottom of the dent. (A viscous Hunt-Crossley law needs mu v ~ 5-10 for
    e = 0.2; its force then jumps at first touch, a spike that rang the
    cardboard box at 104 dB(A).)

    modes: the object's modes (Mode list with coupling masses) are solved
    INSIDE the soil contact, as bodies.contact_pulse does on hard floors:
    the soil is compressed by z - sum(q_n), so a face softer than the soil
    (an empty cardboard box's bottom: its first mode is ~3x softer than the
    turf under it) cushions its own landing. Give drop height h0 (from the canopy
    top) or the speed v_in at the canopy top. It reaches the canopy top at
    t0 (s). Returns per-sample arrays at fs:
      force   contact force on the object (canopy + soil), N
      dent    ground monopole pressure, Pa at 1 m
      power   canopy mechanical loss power, W (rustle drive)
      rate    blade events / s
      load    load per blade, N
    plus an info dict.
    """
    fs_s = fs * oversample
    dt = 1.0 / fs_s
    bn = blade_numbers(g)
    nA, hc = g["shoots_per_m2"], g["canopy_h"]
    k, alpha = turf_contact_law(shape, obj_mat, g)
    sphere = shape[0] == "sphere"
    v0 = np.sqrt(2 * G * h0) if v_in is None else v_in
    e_t = g["restitution"]["sphere" if sphere else "flat"]

    def foot(z):            # blade footprint at depth z below the canopy top
        if z <= 0.0:
            return 0.0
        if sphere:
            R = shape[1]
            zc = min(z, R)
            return np.pi * (2 * R * zc - zc * zc)
        return shape[1] * float(_smoothstep(z / g["tip_spread"]))

    def soil_area(d):
        """Contact area at depth d into fresh soil (flat: tilted face)."""
        if d <= 0.0:
            return 0.0
        if sphere:
            return np.pi * shape[1] * d          # Hertz: a^2 = R delta
        return shape[1] * float(_smoothstep(d / ramp))

    def ground_section(d):
        """The object's cross-section at the ORIGINAL ground level when its
        bottom is d below it: the air volume changes at section x speed,
        in contact or not (it keeps flowing while the body climbs out of
        its dent, so nothing jumps when the soil lets go)."""
        if d <= 0.0:
            return 0.0
        if sphere:
            dc = min(d, shape[1])
            return np.pi * (2 * shape[1] * dc - dc * dc)
        return shape[1] * float(_smoothstep(d / ramp))

    ramp = shape[2] if (not sphere and len(shape) > 2) else g["soil_ramp"]

    def elastic(d):
        if sphere:
            return k * d ** alpha
        return k * np.sqrt(soil_area(d) / shape[1]) * d

    n = int(dur * fs_s)
    F = np.zeros(n)
    Q = np.zeros(n)
    P = np.zeros(n)
    RATE = np.zeros(n)
    LOAD = np.zeros(n)
    i = int(t0 * fs_s)
    z, v = 0.0, v0
    z_crushed = 0.0
    a_prev = 0.0
    landings, soil_hits = [], []
    in_soil = unloading = False
    base = 0.0                  # depth of the dent left by earlier landings
    W_l = d_max = F_max = d_p = c_un = 0.0
    rest_count = 0
    W_canopy = 0.0
    if modes:
        w2 = np.array([(2 * np.pi * md.freq) ** 2 for md in modes])
        dmp = np.array([2.0 * md.decay for md in modes])
        inv_mn = np.array([1.0 / md.mass for md in modes])
        q = np.zeros(len(modes))
        qd = np.zeros(len(modes))
    q_sum = 0.0
    while i < n:
        if z < 0.0 and v < 0.0:                 # flying up above the canopy:
            t_fl = 2.0 * (-v) / G               # analytic flight back down
            i += max(1, int(round(t_fl * fs_s)))
            z, v = 0.0, -v
            continue
        a_fp = foot(z)
        fc = 0.0
        if v > 0.0 and z > 0.0:
            fresh = z > z_crushed
            fc = bn["p_canopy"] * a_fp * (1.0 if fresh else g["springback"])
            p = fc * v
            if fresh:
                p += nA * max(a_fp - a_prev, 0.0) / dt * 0.5 * bn["m_blade"] / 4 * v * v
                z_crushed = z
            P[i] = p
            dadz = (a_fp - a_prev) / (v * dt) if v * dt > 0 else 0.0
            RATE[i] = nA * (a_fp * v / hc + max(dadz, 0.0) * v)
        a_prev = a_fp
        d = z - hc - base - q_sum
        fs_ = 0.0
        if d > 0.0:
            if not in_soil:                     # new landing: fresh soil
                soil_hits.append([i / fs_s, v, None, None])
                in_soil, unloading = True, False
                W_l, d_max, F_max = 0.0, 0.0, 0.0
            if unloading and d > d_max:         # pushed past the old bottom
                unloading = False
            if not unloading and v < 0.0 and F_max > 0.0:
                unloading = True                # start of the rebound
                span = min(e_t ** 2 * W_l * (alpha + 1) / F_max, d_max)
                d_p = d_max - span
                k_un = alpha * F_max / max(span, 1e-9)
                c_un = 2 * ZETA_UNLOAD * np.sqrt(k_un * mass)
            if not unloading:
                fs_ = elastic(d)
                if d > d_max:
                    W_l += 0.5 * (fs_ + F_max) * (d - d_max)
                    d_max, F_max = d, fs_
            elif d > d_p:
                fs_ = max(F_max * ((d - d_p) / (d_max - d_p)) ** alpha
                          + c_un * v, 0.0)
            else:                               # left the dent
                base += d_p
                in_soil = False
                soil_hits[-1][2:] = [i / fs_s, -v]
        elif in_soil:
            in_soil = False
            soil_hits[-1][2:] = [i / fs_s, -v]      # leave time, rebound
        Q[i] = ground_section(z - hc) * v
        ftot = fc + fs_
        F[i] = ftot
        if a_fp > 0:
            LOAD[i] = ftot / (nA * a_fp)
        W_canopy += fc * max(v, 0.0) * dt
        if modes and (in_soil or q_sum != 0.0):
            qd += dt * (fs_ * inv_mn - dmp * qd - w2 * q)
            q += dt * qd
            q_sum = float(q.sum())
        v += dt * (G - ftot / mass)
        z += dt * v
        if z <= 0.0 and v < 0.0:
            landings.append(i / fs_s)
        # at rest on the soil: hold the static load and stop integrating
        if d > 0 and abs(v) < 2e-4 and abs(ftot - mass * G) < 0.02 * mass * G:
            rest_count += 1
            if rest_count > int(0.02 * fs_s):
                F[i:] = mass * G
                break
        else:
            rest_count = 0
        i += 1

    def dec(x):
        m = len(x) // oversample
        return x[:m * oversample].reshape(m, oversample).mean(axis=1)

    Qd = dec(Q)
    dent = -RHO_AIR / (2 * np.pi * R_LISTEN) * np.gradient(Qd) * fs
    info = dict(k=k, alpha=alpha, dent_depth=base + (d_p if in_soil else 0.0),
                restitution_target=e_t,
                soil_hits=soil_hits, v_canopy=v0,
                W_canopy=W_canopy, E_in=0.5 * mass * v0 ** 2,
                p_canopy=bn["p_canopy"])
    if soil_hits:
        t_on, v_on, t_off, v_off = soil_hits[0]
        if t_off is not None:
            info["contact_time"] = t_off - t_on
            info["restitution"] = v_off / v_on
        info["v_soil"] = v_on
        info["F_peak"] = float(F.max())
        info["n_bounces"] = len(soil_hits)
    return dict(force=dec(F), dent=dent, power=dec(P), rate=dec(RATE),
                load=dec(LOAD), info=info)


def clegg_gmax(g: dict = GRASS, fs: int = 44100) -> tuple[float, float]:
    """Peak deceleration (in g) and contact time (s) of a Clegg impact
    hammer (2.25 kg, 50 mm flat steel face, dropped 0.45 m) on this turf:
    the check that ties the turf stiffness to field measurements."""
    r = drop_on_grass(2.25, ("flat", np.pi * 0.025 ** 2, 0.0005), "metal", fs,
                      0.3, v_in=np.sqrt(2 * G * 0.45), g=g)
    return r["info"]["F_peak"] / (2.25 * G), r["info"]["contact_time"]
