"""Deterministic hashing for all probabilistic choices.

Every random decision hashes (seed, stream, index) rather than drawing from a
running generator, so a bounced render matches what was heard and replaying a
bar fires the same pattern.
"""

import numpy as np

_MASK = (1 << 64) - 1


def uhash(*values: int) -> int:
    h = 0xCBF29CE484222325
    for v in values:
        h ^= int(v) & _MASK
        h = (h * 0x100000001B3) & _MASK
        h ^= (h >> 33)
        h = (h * 0xFF51AFD7ED558CCD) & _MASK
        h ^= (h >> 29)
    return h


def urand(*values: int) -> float:
    """Uniform in [0, 1)."""
    return (uhash(*values) >> 11) / float(1 << 53)


def urand_range(lo: float, hi: float, *values: int) -> float:
    return lo + (hi - lo) * urand(*values)


def ubipolar(*values: int) -> float:
    """Uniform in [-1, 1)."""
    return urand(*values) * 2.0 - 1.0


def urand_array(n: int, *values: int) -> np.ndarray:
    """Uniform in [0, 1) per index, matching urand(*values, i) for i in range(n).

    Vectorised over fixed-width uint64 arrays rather than looping urand in
    Python: this is on the per-sample noise path for RADIATION and FISSION's
    continuous stochastic state, so it has to run fast enough to drive a whole
    render. Overflow in the uint64 arithmetic is the point (it is the same
    wraparound `& _MASK` does in uhash), not a bug — suppressed rather than
    left to warn on every call.
    """
    with np.errstate(over="ignore"):
        h = np.uint64(0xCBF29CE484222325)
        for v in values:
            h ^= np.uint64(int(v) & _MASK)
            h = h * np.uint64(0x100000001B3)
            h ^= h >> np.uint64(33)
            h = h * np.uint64(0xFF51AFD7ED558CCD)
            h ^= h >> np.uint64(29)

        idx = np.arange(n, dtype=np.uint64)
        h = np.full(n, h, dtype=np.uint64) ^ idx
        h = h * np.uint64(0x100000001B3)
        h ^= h >> np.uint64(33)
        h = h * np.uint64(0xFF51AFD7ED558CCD)
        h ^= h >> np.uint64(29)

    return (h >> np.uint64(11)).astype(np.float64) / float(1 << 53)


def ubipolar_array(n: int, *values: int) -> np.ndarray:
    """Uniform in [-1, 1) per index, vectorised. See urand_array."""
    return urand_array(n, *values) * 2.0 - 1.0


def gaussian_array(n: int, *values: int) -> np.ndarray:
    """Standard-normal per index, vectorised, via Box-Muller on two
    independent hash streams (the `0`/`1` discriminators below). Used for
    noise beds that were written against np.random's standard_normal, which
    also needed the whole render length n up front to draw from — the same
    offline-only pattern urand_array replaces for uniform draws.
    """
    u1 = np.clip(urand_array(n, *values, 0), 1e-12, 1.0)
    u2 = urand_array(n, *values, 1)
    return np.sqrt(-2.0 * np.log(u1)) * np.cos(2.0 * np.pi * u2)
