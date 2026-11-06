"""Aggregate / granular surface exciter (gravel, dirt, grass, debris).

References: Cook, "Physically Informed Sonic Modeling (PhISM/PhISEM)" (1997);
Turchet, "Footstep sounds synthesis" (Applied Acoustics, 2016), aggregate
surface layer.

PhISEM skeleton kept from v1:
- micro-collisions are a Poisson process, density driven by a control signal
  (impact energy / slide speed);
- each event carries a random (exponentially distributed) energy bump.

v2 changes (after listening tests on the v1 renders):
- v1 pushed ALL grains through one shared resonant bandpass. Hundreds of
  identically pitched rings fuse into a common-mode resonance that reads as
  a drum head / awning being hit by beads. v2 synthesizes each grain
  individually with RANDOMIZED center frequency (log-uniform), Q and decay,
  so no shared resonance builds up -- each pebble has its own pitch.
- added a "bed" component: broadband noise, bandpassed, amplitude-modulated
  by the (fast-smoothed) grain energy envelope. Vegetation-like surfaces
  (grass) are mostly THIS -- stochastic AM rustle, no audible resonance --
  per the observation that grass reads better as high-frequency noise.
- per-surface balance via bed_mix (0 = all discrete grains, 1 = all rustle).

v3 changes (grass only; gravel/dirt renders are bit-identical to v2):
- the v2 grass preset was 80% stationary bandpassed noise (2.8-11 kHz) under
  a slow envelope -- a smooth hiss swell ("shhh"/steam), no texture. Its
  spectrogram is a uniform wash with no visible events.
- grass is now a LAYERED preset (`layers`): (a) "swish" = overlapping
  noise grains 10-60 ms long with a soft attack, each with its own band in
  0.7-6 kHz (blades brushing past each other), (b) "crackle" = sparse
  sub-millisecond clicks (stems snapping / dry blades), (c) a small
  low-mid bed for continuity. Grain amplitudes are lognormal (heavy-tailed)
  so a few loud swishes/snaps stand out -- that irregularity is what makes
  it read as material rather than noise.
- new grain options used by (a)/(b): grain_attack (soft onset) and
  amp_sigma (lognormal amplitudes instead of exponential).

v4 (2026-11-06): PHYSICAL drive, used by core/grass.py. `grain_stream_pa`
takes an event rate (events/s) and an ACOUSTIC POWER (W) per sample instead
of a 0..1 envelope, and returns pressure in Pa at 1 m (the pressure-proxy
units of core/radiation.py): every grain is normalized to unit energy and
scaled so the stream's energy equals the acoustic energy radiated into the
half-space above the ground, int p^2 dt = rho c E / (2 pi r^2). The grain
shapes are the v3 grass layers (GRASS_GRAINS). The 0..1 path above is
unchanged (gravel/dirt and the old footstep renders still use it).
"""
from __future__ import annotations

import numpy as np
from scipy.signal import butter, lfilter, sosfilt


def _biquad_bandpass(fc: float, q: float, fs: int):
    w0 = 2 * np.pi * fc / fs
    alpha = np.sin(w0) / (2 * q)
    b = np.array([alpha, 0.0, -alpha])
    a = np.array([1 + alpha, -2 * np.cos(w0), 1 - alpha])
    return b / a[0], a / a[0]


def aggregate_exciter(density_hz, duration: float, fs: int,
                      grain_fc: tuple[float, float] = (1000.0, 4000.0),
                      grain_q: tuple[float, float] = (1.0, 3.0),
                      grain_decay: tuple[float, float] = (0.002, 0.006),
                      bed_mix: float = 0.15,
                      bed_band: tuple[float, float] = (1500.0, 6000.0),
                      bed_smooth: float = 0.008,
                      grain_attack: float | None = None,
                      amp_sigma: float | None = None,
                      energy=1.0, seed: int | None = 1) -> np.ndarray:
    """Poisson-triggered grain stream with per-grain randomized resonance.

    density_hz : scalar or per-sample array -- mean micro-impact rate (Hz).
    grain_fc   : (lo, hi) Hz -- per-grain center frequency, log-uniform.
    grain_q    : (lo, hi) -- per-grain resonance Q, uniform.
    grain_decay: (lo, hi) s -- per-grain energy decay, uniform.
    bed_mix    : 0..1 -- blend of continuous AM-noise bed vs discrete grains.
    bed_band   : bandpass (lo, hi) Hz for the bed noise.
    bed_smooth : smoothing time constant (s) for the bed's AM envelope.
    grain_attack: None = instant onset; else attack time constant as a
                 FRACTION of each grain's decay (envelope
                 (1 - exp(-t/ta)) * exp(-t/td), ta = grain_attack * td).
    amp_sigma  : None = Exp(1) grain amplitudes; else lognormal(0, sigma),
                 clipped at 6 sigma (heavier tail -> a few events stand out).
    energy     : scalar or per-sample array scaling bump amplitude.
    """
    n = int(round(duration * fs))
    dens = np.broadcast_to(np.asarray(density_hz, dtype=float), (n,))
    en = np.broadcast_to(np.asarray(energy, dtype=float), (n,))
    rng = np.random.default_rng(seed)

    p = np.clip(dens / fs, 0.0, 1.0)
    event_idx = np.nonzero(rng.random(n) < p)[0]
    if amp_sigma is None:
        raw = rng.exponential(1.0, len(event_idx))
    else:
        raw = np.minimum(rng.lognormal(0.0, amp_sigma, len(event_idx)),
                         np.exp(6.0 * amp_sigma))
    amps = raw * en[event_idx]

    # ---- discrete grains: individual noise bursts, randomized resonance ----
    grains = np.zeros(n)
    if bed_mix < 1.0 and len(event_idx):
        lo_f, hi_f = grain_fc
        fcs = np.exp(rng.uniform(np.log(lo_f), np.log(hi_f), len(event_idx)))
        qs = rng.uniform(*grain_q, len(event_idx))
        decs = rng.uniform(*grain_decay, len(event_idx))
        for i0, amp, fc, q, dec in zip(event_idx, amps, fcs, qs, decs):
            L = min(int(8 * dec * fs) + 32, n - i0)
            if L <= 0:
                continue
            t = np.arange(L) / fs
            shape = np.exp(-t / dec)
            if grain_attack is not None:
                shape *= 1.0 - np.exp(-t / (grain_attack * dec))
                shape /= shape.max() or 1.0
            burst = rng.standard_normal(L) * shape * amp
            b, a = _biquad_bandpass(min(fc, 0.45 * fs), q, fs)
            grains[i0:i0 + L] += lfilter(b, a, burst)

    # ---- noise bed: bandpassed noise AM'd by smoothed grain energy --------
    bed = np.zeros(n)
    if bed_mix > 0.0:
        bumps = np.zeros(n)
        bumps[event_idx] = amps
        a1 = np.exp(-1.0 / (bed_smooth * fs))
        env = lfilter([1.0 - a1], [1.0, -a1], bumps)
        sos = butter(2, [bed_band[0], min(bed_band[1], 0.47 * fs)],
                     btype="bandpass", fs=fs, output="sos")
        bed = sosfilt(sos, rng.standard_normal(n)) * np.sqrt(np.maximum(env, 0.0))

    def _norm(x):
        pk = float(np.max(np.abs(x)))
        return x / pk if pk > 0 else x

    return (1.0 - bed_mix) * _norm(grains) + bed_mix * _norm(bed) \
        if 0.0 < bed_mix < 1.0 else (_norm(bed) if bed_mix >= 1.0 else _norm(grains))


# Ear-tuned presets (density at full energy; scale density/energy by control)
AGGREGATE_PRESETS: dict[str, dict] = {
    # stones: every grain its own pitch, wide spread, ringy but incoherent
    "gravel": dict(density_hz=300.0, grain_fc=(900.0, 4500.0),
                   grain_q=(1.2, 3.0), grain_decay=(0.002, 0.007),
                   bed_mix=0.15, bed_band=(1500.0, 6000.0)),
    # dull crumble: low, broad, fast-dying grains, some low rustle
    "dirt":   dict(density_hz=350.0, grain_fc=(250.0, 1300.0),
                   grain_q=(0.8, 1.6), grain_decay=(0.0015, 0.004),
                   bed_mix=0.25, bed_band=(300.0, 2000.0)),
    # vegetation (v3, layered): swishing blades + sparse stem snaps + thin bed.
    # Each layer: aggregate_exciter kwargs + "gain" (relative peak level).
    "grass": dict(layers=[
        # swish: long, soft-onset, broad noise grains, own band per grain
        dict(gain=1.0, density_hz=60.0, grain_fc=(700.0, 6000.0),
             grain_q=(0.6, 1.4), grain_decay=(0.010, 0.060),
             grain_attack=0.35, amp_sigma=1.2, bed_mix=0.0),
        # crackle: sparse sub-ms clicks, bright, very uneven levels
        dict(gain=0.55, density_hz=45.0, grain_fc=(2500.0, 9000.0),
             grain_q=(0.7, 1.5), grain_decay=(0.0002, 0.0009),
             amp_sigma=1.3, bed_mix=0.0),
        # bed: low-mid continuity only, so gaps between swishes aren't dead
        dict(gain=0.10, density_hz=400.0, bed_mix=1.0,
             bed_band=(900.0, 4500.0), bed_smooth=0.03),
    ]),
}

# v2 grass kept for A/B listening against v3
AGGREGATE_PRESETS_V2_GRASS = dict(density_hz=800.0, grain_fc=(3000.0, 9000.0),
                                  grain_q=(0.8, 1.8),
                                  grain_decay=(0.0008, 0.002),
                                  bed_mix=0.8, bed_band=(2800.0, 11000.0))


def render_aggregate(preset: str | dict, envelope: np.ndarray, fs: int,
                     seed: int = 1) -> np.ndarray:
    """Render a surface preset driven by a 0..1 control envelope (per sample).

    The envelope scales both grain density and grain energy, so louder
    contact = more AND stronger micro-collisions. Layered presets sum their
    layers (each normalized by aggregate_exciter) with per-layer gains.
    """
    p = AGGREGATE_PRESETS[preset] if isinstance(preset, str) else preset
    dur = len(envelope) / fs
    layers = p["layers"] if "layers" in p else [dict(p, gain=1.0)]
    out = np.zeros(len(envelope))
    for i, layer in enumerate(layers):
        kw = dict(layer)
        gain = kw.pop("gain")
        dens = kw.pop("density_hz")
        y = aggregate_exciter(density_hz=dens * envelope, duration=dur, fs=fs,
                              energy=envelope, seed=seed + i, **kw)
        out[:len(y)] += gain * y
    return out


# ------------------------------------------------- v4: physically driven

RHO_C = 1.2 * 343.0          # air impedance (Pa s / m), as core/radiation.py

# Grain shapes for the physically driven grass (v3 spectral shapes, ear-
# tuned, NOT yet checked against a recording). max_rate: the stream never
# uses more than this many grains/s; above it, each grain carries the
# energy of several blade events (power is kept exact). At ~200 grains/s of
# 10-60 ms the sum is already a continuous rustle, so more grains would only
# cost CPU.
GRASS_GRAINS = {
    # blades brushing the object and each other
    "swish": dict(grain_fc=(700.0, 6000.0), grain_q=(0.6, 1.4),
                  grain_decay=(0.010, 0.060), grain_attack=0.35,
                  amp_sigma=1.2, max_rate=200.0),
    # stems / dry blades snapping under load
    "crackle": dict(grain_fc=(2500.0, 9000.0), grain_q=(0.7, 1.5),
                    grain_decay=(0.0002, 0.0009), grain_attack=None,
                    amp_sigma=1.3, max_rate=150.0),
}


def grain_stream_pa(rate_hz, power_w, fs: int,
                    grain_fc: tuple[float, float], grain_q: tuple[float, float],
                    grain_decay: tuple[float, float],
                    grain_attack: float | None = None,
                    amp_sigma: float = 1.0, max_rate: float = 200.0,
                    r_listen: float = 1.0, seed: int | None = 1) -> np.ndarray:
    """Grain stream in Pa at r_listen, driven by physics.

    rate_hz : per-sample physical event rate (blade events / s).
    power_w : per-sample ACOUSTIC power (W) of this layer.
    Grains fire as a Poisson process at min(rate, max_rate). Grain i gets
    the energy power(t_i) / rate_used(t_i) (times a lognormal factor
    clipped at 3 sigma, mean square 1), radiated into a
    half-space: int p^2 dt = rho c E / (2 pi r^2).
    """
    power = np.maximum(np.asarray(power_w, dtype=float), 0.0)
    n = len(power)
    rate = np.broadcast_to(np.asarray(rate_hz, dtype=float), (n,))
    used = np.minimum(rate, max_rate)
    rng = np.random.default_rng(seed)
    idx = np.nonzero((rng.random(n) < used / fs) & (power > 0))[0]
    out = np.zeros(n)
    if not len(idx):
        return out
    energy = power[idx] / used[idx]                               # J
    # lognormal amplitudes clipped at 2 sigma (still a 20+ dB spread),
    # normalized to mean square 1 by a large reference draw, so the energy
    # is exact on average (checked: 1.04 over 200 renders). A short burst
    # of a few dozen grains still scatters +-2 dB from render to render.
    cap = np.exp(2 * amp_sigma)
    ref = np.minimum(np.random.default_rng(0).lognormal(0.0, amp_sigma,
                                                        200000), cap)
    amp2 = (np.minimum(rng.lognormal(0.0, amp_sigma, len(idx)), cap) ** 2
            / np.mean(ref ** 2))
    p2 = RHO_C * energy * amp2 / (2 * np.pi * r_listen**2)        # Pa^2 s
    fcs = np.exp(rng.uniform(np.log(grain_fc[0]), np.log(grain_fc[1]), len(idx)))
    qs = rng.uniform(*grain_q, len(idx))
    decs = rng.uniform(*grain_decay, len(idx))
    for i0, e2, fc, q, dec in zip(idx, p2, fcs, qs, decs):
        L = min(int(8 * dec * fs) + 32, n - i0)
        if L <= 8:
            continue
        t = np.arange(L) / fs
        shape = np.exp(-t / dec)
        if grain_attack is not None:
            shape *= 1.0 - np.exp(-t / (grain_attack * dec))
        b, a = _biquad_bandpass(min(fc, 0.45 * fs), q, fs)
        g = lfilter(b, a, rng.standard_normal(L) * shape)
        g *= np.sqrt(e2 * fs / (np.sum(g**2) or 1.0))           # sum g^2/fs = e2
        out[i0:i0 + L] += g
    return out
