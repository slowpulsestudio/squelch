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
    return np.array([urand(*values, i) for i in range(n)])
