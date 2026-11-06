"""Sound levels of pressure signals in Pa (the prototype's pressure proxy,
Pa at 1 m): peak SPL, A-weighted LAeq and LAFmax (IEC 61672 A curve,
'fast' = 125 ms exponential time weighting)."""
from __future__ import annotations

import numpy as np
from scipy.signal import lfilter

P_REF = 20e-6


def a_weight_gain(f):
    f = np.asarray(f, dtype=float)
    f2 = f * f
    ra = (12194.0 ** 2 * f2 * f2) / ((f2 + 20.6 ** 2) * np.sqrt(
        (f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2))
    return ra * 10 ** (2.0 / 20)


def a_weighted(p: np.ndarray, fs: int) -> np.ndarray:
    n = len(p)
    nfft = 1 << int(np.ceil(np.log2(n)))
    X = np.fft.rfft(p, nfft)
    return np.fft.irfft(X * a_weight_gain(np.fft.rfftfreq(nfft, 1 / fs)), nfft)[:n]


def levels(p: np.ndarray, fs: int, t_span: tuple[float, float] | None = None) -> dict:
    """peak dB SPL, LAeq over t_span (default: whole signal), LAFmax."""
    pa = a_weighted(p, fs)
    sl = slice(None) if t_span is None else slice(int(t_span[0] * fs),
                                                   int(t_span[1] * fs))
    laeq = 10 * np.log10(np.mean(pa[sl] ** 2) / P_REF ** 2 + 1e-30)
    a = np.exp(-1.0 / (0.125 * fs))
    ms = lfilter([1 - a], [1, -a], pa ** 2)
    return dict(peak_db=20 * np.log10(np.max(np.abs(p)) / P_REF + 1e-30),
                laeq=float(laeq),
                lafmax=float(10 * np.log10(ms.max() / P_REF ** 2 + 1e-30)))


def db_spl(p_rms: float) -> float:
    return 20 * np.log10(p_rms / P_REF + 1e-30)
