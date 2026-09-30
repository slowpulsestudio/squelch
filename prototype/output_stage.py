"""Everything after the reactor: DRIVE, COLLIMATOR, FALLOUT and peak safety."""

from __future__ import annotations

import numpy as np
from scipy.ndimage import maximum_filter1d

from . import filters
from .params import Params

#: The spectral point COLLIMATOR closes in on. Deliberately low-mid rather than
#: centred in the audible band, to keep a focused beam warm instead of shrill.
COLLIMATOR_CENTRE_HZ = 650.0

#: Below this, FALLOUT leaves the signal mono so club systems stay solid.
STEREO_BASS_MONO_HZ = 150.0

LIMITER_LOOKAHEAD_S = 0.005
LIMITER_CEILING = 0.97


def drive(x: np.ndarray, p: Params) -> np.ndarray:
    """Saturation with the level change taken back out.

    DRIVE is a contamination control, not a gain control, so the RMS it adds is
    removed afterwards.
    """
    if p.drive <= 0.0:
        return x
    amount = np.power(p.drive, 0.6)
    gain = 1.0 + 11.0 * amount
    before = np.sqrt(np.mean(x**2)) + 1e-12
    y = np.tanh(x * gain)
    after = np.sqrt(np.mean(y**2)) + 1e-12
    return y * (before / after)


def collimate(x: np.ndarray, sr: int, p: Params) -> np.ndarray:
    """Close a high-pass and a low-pass in on COLLIMATOR_CENTRE_HZ."""
    if p.collimator <= 0.0:
        return x
    hp_open, lp_open = 20.0, 18000.0
    hp = hp_open * np.power(COLLIMATOR_CENTRE_HZ * 0.62 / hp_open, p.collimator)
    lp = lp_open * np.power(COLLIMATOR_CENTRE_HZ * 2.2 / lp_open, p.collimator)
    q = 0.707 + 0.5 * p.collimator
    y = filters.static_highpass(x, hp, sr, q=q)
    return filters.static_lowpass(y, lp, sr, q=q)


def fallout(x: np.ndarray, sr: int, p: Params) -> np.ndarray:
    """Spread the reaction across the stereo field, keeping the bass centred."""
    if p.fallout <= 0.0:
        return x
    low = filters.static_lowpass(x, STEREO_BASS_MONO_HZ, sr)
    high = x - low

    delay = int(p.fallout * 0.004 * sr)
    if delay > 0:
        shifted = np.zeros_like(high)
        shifted[:, 0] = high[:, 0]
        shifted[delay:, 1] = high[: len(high) - delay, 1]
        high = shifted

    mid = high.mean(axis=1)
    side = (high[:, 0] - high[:, 1]) * 0.5
    side *= 1.0 + 2.2 * p.fallout

    spread = np.stack([mid + side, mid - side], axis=1)
    return low.mean(axis=1)[:, None] + spread


def match_rms(y: np.ndarray, reference: np.ndarray) -> np.ndarray:
    target = np.sqrt(np.mean(reference**2)) + 1e-12
    current = np.sqrt(np.mean(y**2)) + 1e-12
    return y * (target / current)


def peak_limit(x: np.ndarray, sr: int) -> np.ndarray:
    """Lookahead peak limiter.

    Gain is derived from a rolling forward-looking max and applied to audio
    delayed by the same window, so single-sample transients are caught rather
    than slipping through ahead of the envelope.
    """
    window = max(int(LIMITER_LOOKAHEAD_S * sr), 1)
    magnitude = np.max(np.abs(x), axis=1)
    padded = np.concatenate([magnitude, np.zeros(window)])
    rolling = maximum_filter1d(padded, size=2 * window + 1, mode="nearest")[:len(magnitude)]

    gain = np.minimum(1.0, LIMITER_CEILING / np.maximum(rolling, 1e-9))

    delayed = np.zeros_like(x)
    delayed[window:] = x[: len(x) - window]
    return delayed * gain[:, None]


def process(wet: np.ndarray, dry: np.ndarray, sr: int, p: Params, mix: float = 1.0) -> np.ndarray:
    """Run the output chain, blend with dry, then protect the peaks.

    Peak safety runs after the blend rather than on the wet path alone: the
    reaction decorrelates phase against the dry signal, so the mix can peak
    higher than either part on its own.
    """
    y = drive(wet, p)
    y = collimate(y, sr, p)
    y = fallout(y, sr, p)
    y = match_rms(y, dry)
    y = (1.0 - mix) * dry + mix * y
    return peak_limit(y, sr)
