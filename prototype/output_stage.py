"""Everything after the reactor: DRIVE, COLLIMATOR, FALLOUT and peak safety."""

from __future__ import annotations

import numpy as np
from scipy.ndimage import maximum_filter1d

from . import filters, rng
from .controls import Controls
from .params import Params

#: The spectral point COLLIMATOR closes in on. Deliberately low-mid rather than
#: centred in the audible band, to keep a focused beam warm instead of shrill.
COLLIMATOR_CENTRE_HZ = 650.0

#: Below this, FALLOUT leaves the signal mono so club systems stay solid.
STEREO_BASS_MONO_HZ = 150.0

#: The staccato midrange wobble FALLOUT produces on reactions that disperse in
#: pitch rather than in space.
WOBBLE_LO_HZ = 300.0
WOBBLE_HI_HZ = 2500.0
WOBBLE_RATE_HZ = 6.0
WOBBLE_MAX_DELAY_S = 0.0035
WOBBLE_BURST_S = 0.09
WOBBLE_CHANCE = 0.45

LIMITER_LOOKAHEAD_S = 0.005
LIMITER_CEILING = 0.97

#: Pultec-style voicing, applied to every reaction. Tuned for future garage /
#: breaks / warm techno: weight low down, mud pulled out above it, the harsh
#: 4-8kHz region scooped, and the air above it left intact.
VOICE_LOW_HZ = 70.0
VOICE_LOW_DB = 2.5
VOICE_DIP_HZ = 260.0
VOICE_DIP_DB = -1.8
VOICE_DEHARSH_HZ = 6000.0
VOICE_DEHARSH_DB = -3.5
VOICE_DEHARSH_Q = 0.75
VOICE_AIR_HZ = 15000.0
VOICE_AIR_DB = 1.0


def drive(x: np.ndarray, p: Params, weight: float = 1.0) -> np.ndarray:
    """Saturation with the level change taken back out.

    DRIVE is a contamination control, not a gain control, so the RMS it adds is
    removed afterwards. The weight is the reaction's own appetite for it.
    """
    if p.drive <= 0.0:
        return x
    amount = np.power(p.drive, 0.6) * weight
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


def _stereo_spread(x: np.ndarray, sr: int, amount: float) -> np.ndarray:
    """Disperse into the stereo field, keeping the bass centred."""
    if amount <= 0.0:
        return x
    low = filters.static_lowpass(x, STEREO_BASS_MONO_HZ, sr)
    high = x - low

    delay = int(amount * 0.004 * sr)
    if delay > 0:
        shifted = np.zeros_like(high)
        shifted[:, 0] = high[:, 0]
        shifted[delay:, 1] = high[: len(high) - delay, 1]
        high = shifted

    mid = high.mean(axis=1)
    side = (high[:, 0] - high[:, 1]) * 0.5
    side *= 1.0 + 2.2 * amount

    spread = np.stack([mid + side, mid - side], axis=1)
    return low.mean(axis=1)[:, None] + spread


def _mid_wobble(x: np.ndarray, sr: int, amount: float, c: Controls) -> np.ndarray:
    """Staccato vibrato on the midrange only.

    Dispersion for reactions that should stay put in the stereo field: the
    material scatters in pitch instead of in space. Gated to short bursts on a
    subset of events so it reads as rhythmic rather than as a constant warble.
    """
    if amount <= 0.0 or len(c.env) == 0:
        return x

    blocks = len(c.env)
    ctrl_sr = sr / filters.BLOCK
    gate = np.zeros(blocks)
    burst = max(int(WOBBLE_BURST_S * ctrl_sr), 1)
    for i, start in enumerate(np.clip(c.starts // filters.BLOCK, 0, blocks - 1)):
        if rng.urand(1, 70, i) < WOBBLE_CHANCE:
            gate[start : start + burst] = 1.0
    gate = filters.smooth(gate, 0.004, sr)

    lfo = 0.5 + 0.5 * np.sin(2.0 * np.pi * WOBBLE_RATE_HZ * np.arange(blocks) / ctrl_sr)

    mid = filters.static_highpass(x, WOBBLE_LO_HZ, sr, q=0.7)
    mid = filters.static_lowpass(mid, WOBBLE_HI_HZ, sr, q=0.7)
    rest = x - mid

    wobbled = filters.pitch_wind(mid, lfo * gate * amount, sr, WOBBLE_MAX_DELAY_S)
    return rest + wobbled


def fallout(x: np.ndarray, sr: int, p: Params, profile, c: Controls) -> np.ndarray:
    """FALLOUT disperses the reaction, in whichever way suits the reaction.

    FISSION scatters across the stereo field because splitting is what it does;
    the others scatter in pitch instead, so they stay centred and physical.
    """
    y = _stereo_spread(x, sr, p.fallout * profile.spread_weight)
    return _mid_wobble(y, sr, p.fallout * profile.wobble_weight, c)


def match_rms(y: np.ndarray, reference: np.ndarray) -> np.ndarray:
    target = np.sqrt(np.mean(reference**2)) + 1e-12
    current = np.sqrt(np.mean(y**2)) + 1e-12
    return y * (target / current)


def voice(x: np.ndarray, sr: int) -> np.ndarray:
    """Fixed Pultec-style house voicing: weight at the bottom, bite off the top.

    The low shelf and the dip just above it are the Pultec boost-and-attenuate
    trick, which lifts the very bottom while thinning the mud above it. The
    de-harsh stage is a broad bell rather than a shelf on purpose: a shelf from
    5kHz also pulled 15kHz down 2.4dB, which removes harshness by dulling the
    whole top rather than by scooping the region that is actually harsh.
    """
    y = filters.low_shelf(x, VOICE_LOW_HZ, VOICE_LOW_DB, sr)
    y = filters.peaking(y, VOICE_DIP_HZ, VOICE_DIP_DB, 0.9, sr)
    y = filters.peaking(y, VOICE_DEHARSH_HZ, VOICE_DEHARSH_DB, VOICE_DEHARSH_Q, sr)
    return filters.high_shelf(y, VOICE_AIR_HZ, VOICE_AIR_DB, sr)



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


def process(
    wet: np.ndarray,
    dry: np.ndarray,
    sr: int,
    p: Params,
    profile,
    c: Controls,
    mix: float = 1.0,
) -> np.ndarray:
    """Run the output chain, blend with dry, then protect the peaks.

    Peak safety runs after the blend rather than on the wet path alone: the
    reaction decorrelates phase against the dry signal, so the mix can peak
    higher than either part on its own.
    """
    y = drive(wet, p, profile.drive_weight)
    y = collimate(y, sr, p)
    y = fallout(y, sr, p, profile, c)
    y = voice(y, sr)
    y = match_rms(y, dry)
    y = (1.0 - mix) * dry + mix * y
    return peak_limit(y, sr)
