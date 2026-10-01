"""Saturation curves and the oversampling that keeps them clean.

A pointwise nonlinearity generates harmonics far above Nyquist. At base rate
those fold back as inharmonic content sitting underneath the signal, which is
the fizzy, brittle quality that reads as "digital distortion" rather than as
drive. Running the curve at a higher rate and filtering on the way back down
removes almost all of it.
"""

from __future__ import annotations

from typing import Callable

import numpy as np
from scipy.signal import resample_poly

#: Rate multiplier for the nonlinear stages. 4x puts the first problematic
#: harmonics above the downsampling filter's stopband.
FACTOR = 4

#: Kaiser window for the polyphase resampler. Linear phase, so the passband is
#: not smeared; the group delay it costs is compensated by resample_poly here
#: and would be reported to the host in a real-time port.
WINDOW = ("kaiser", 8.0)

#: Below this the curve is linear. tanh starts bending immediately, so quiet
#: material picks up harmonics it has no business having.
KNEE = 0.62


def soft_clip(x: np.ndarray, drive: float = 1.0) -> np.ndarray:
    """Linear up to the knee, then a smooth approach to the limit.

    Gentler than a bare tanh, which curves from zero and so generates
    high-order content even at modest levels.
    """
    y = x * drive
    magnitude = np.abs(y)
    above = magnitude > KNEE
    out = np.array(y, copy=True)
    excess = (magnitude[above] - KNEE) / (1.0 - KNEE)
    out[above] = np.sign(y[above]) * (KNEE + (1.0 - KNEE) * np.tanh(excess))
    return out


def hard_clip(x: np.ndarray, ceiling: float = 1.0) -> np.ndarray:
    return np.clip(x, -ceiling, ceiling)


def oversampled(x: np.ndarray, curve: Callable[[np.ndarray], np.ndarray]) -> np.ndarray:
    """Apply a nonlinearity at FACTOR times the sample rate."""
    up = resample_poly(x, FACTOR, 1, axis=0, window=WINDOW)
    down = resample_poly(curve(up), 1, FACTOR, axis=0, window=WINDOW)
    return down[: len(x)]
