"""A small diffusion reverb with a ping-pong tail.

Built for IONIZE, which gives every reaction event its own send amount so some
arrive close and dry while others wash back. The tail crosses between channels
rather than sitting still, so events scatter in depth and in space at once.

Everything here is a feedback structure, so it is normalised by energy rather
than by DC gain: with DC normalisation a short decay comes out audibly louder
than a long one and the control reads as a level rather than a time.
"""

from __future__ import annotations

import numpy as np
from scipy.signal import lfilter

#: Schroeder-ish comb lengths, deliberately mutually prime-ish so their echoes
#: do not line up into a pitch.
COMB_MS = (29.7, 37.1, 41.1, 43.7)
ALLPASS_MS = (5.0, 1.7, 12.3)
ALLPASS_G = 0.62
PINGPONG_MS = 95.0


def _damped_comb(x: np.ndarray, delay: int, feedback: float, damping: float) -> np.ndarray:
    """One lowpassed feedback comb, as a single recursive filter."""
    a = np.zeros(delay + 2)
    a[0] = 1.0
    a[delay] = -feedback * (1.0 - damping)
    a[delay + 1] = -feedback * damping
    return lfilter([np.sqrt(max(1.0 - feedback**2, 1e-6))], a, x, axis=0)


def _allpass(x: np.ndarray, delay: int, g: float) -> np.ndarray:
    b = np.zeros(delay + 1)
    b[0] = -g
    b[delay] = 1.0
    a = np.zeros(delay + 1)
    a[0] = 1.0
    a[delay] = -g
    return lfilter(b, a, x, axis=0)


def _shift(x: np.ndarray, delay: int) -> np.ndarray:
    out = np.zeros_like(x)
    if delay < len(x):
        out[delay:] = x[: len(x) - delay]
    return out


def _ping_pong(x: np.ndarray, delay: int, feedback: float) -> np.ndarray:
    """Cross-fed delay pair: what leaves one side arrives on the other.

    Solved as a recursion on each channel rather than a sample loop — feeding
    L into R and back into L means each side sees itself one round trip later,
    which is a single denominator of length 2*delay.
    """
    a = np.zeros(2 * delay + 1)
    a[0] = 1.0
    a[2 * delay] = -feedback**2

    left = x[:, 0] + feedback * _shift(x[:, 1], delay)
    right = x[:, 1] + feedback * _shift(x[:, 0], delay)
    scale = np.sqrt(max(1.0 - feedback**2, 1e-6))
    return np.stack(
        [lfilter([scale], a, left), lfilter([scale], a, right)], axis=1
    )


def reverb(
    x: np.ndarray, sr: int, decay: float = 0.7, damping: float = 0.35
) -> np.ndarray:
    """Diffuse, then bounce, then let it ring."""
    y = x
    for ms in ALLPASS_MS:
        y = _allpass(y, max(int(ms * sr / 1000.0), 1), ALLPASS_G)

    y = _ping_pong(y, max(int(PINGPONG_MS * sr / 1000.0), 1), 0.45 + 0.35 * decay)

    feedback = 0.70 + 0.21 * decay
    tail = np.zeros_like(y)
    for ms in COMB_MS:
        tail += _damped_comb(y, max(int(ms * sr / 1000.0), 1), feedback, damping)
    return tail / len(COMB_MS)
