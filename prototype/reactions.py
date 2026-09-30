"""The five REACTION types.

Each profile sets the reactor's filter/envelope territory, then contributes one
extra stage that gives that reaction its character. Every number here is a
prototype starting point to be swept and confirmed from Designer feedback, not a
final value — prompt.md describes these reactions qualitatively only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

import numpy as np

from . import filters
from .controls import Controls
from .params import Params


@dataclass
class ReactionProfile:
    name: str
    cutoff_lo_hz: float
    cutoff_hi_hz: float
    decay_lo_s: float
    decay_hi_s: float
    resonance_lo: float
    resonance_hi: float
    inner_sat: float
    sub_event_bias: float
    amp_floor: float
    post: Callable[[np.ndarray, np.ndarray, Controls, Params, int], np.ndarray]


def _event_burst_envelope(n: int, starts: np.ndarray, length: int) -> np.ndarray:
    """A very short decaying spike at each event start, at sample rate."""
    env = np.zeros(n)
    shape = np.exp(-np.linspace(0.0, 6.0, length))
    for s in starts:
        end = min(s + length, n)
        if end <= s:
            continue
        env[s:end] = np.maximum(env[s:end], shape[: end - s])
    return env


def _ticks(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Geiger-counter ticks: very short high-passed slices of the input itself."""
    n = wet.shape[0]
    length = int(0.012 * sr)
    burst = _event_burst_envelope(n, c.starts, length)[:, None]
    ticks = filters.static_highpass(dry, 2600.0, sr, q=0.9) * burst
    level = 0.30 * p.squelch * (1.0 - c.damping)
    return wet + ticks * level


def _phaser(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Splitting: a swept allpass chain, run per channel in opposite directions."""
    sweep = 220.0 * np.power(2.0, 4.2 * c.env)
    feedback = 0.72 * p.exposure * (1.0 - c.damping)
    left = filters.varying_allpass_chain(wet[:, :1], sweep, sr, stages=6, feedback=feedback)
    right = filters.varying_allpass_chain(wet[:, 1:], sweep[::-1], sr, stages=6, feedback=feedback)
    phased = np.concatenate([left, right], axis=1)
    return 0.5 * wet + 0.5 * phased


def _sludge(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Contamination: an octave-down body layer under a saturated, dark signal."""
    sub = filters.static_lowpass(filters.octave_down(wet), 180.0, sr, q=0.7)
    level = 0.45 * p.squelch * (1.0 - c.damping)
    thickened = wet + sub * level
    return np.tanh(thickened * (1.0 + 1.5 * p.squelch)) / (1.0 + 0.8 * p.squelch)


def _bubble(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Boiling: a second resonant peak that jumps to a new frequency per block."""
    jump = 400.0 * np.power(2.0, 3.4 * np.roll(c.env, 3))
    q = 3.0 + 12.0 * p.exposure * (1.0 - c.damping)
    second = filters.varying_ladder(wet, jump, np.full_like(jump, q), sr, inner_sat=0.2)
    return 0.62 * wet + 0.38 * second


def _shift(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Non-terrestrial: single-sideband frequency sweeps tracking each event."""
    depth = 60.0 + 900.0 * p.range
    shifted = filters.frequency_shift(wet, c.env * depth, sr)
    amount = 0.55 * p.squelch * (1.0 - c.damping)
    return (1.0 - amount) * wet + amount * shifted


PROFILES = {
    "RADIATION": ReactionProfile(
        name="RADIATION",
        cutoff_lo_hz=180.0,
        cutoff_hi_hz=4200.0,
        decay_lo_s=0.03,
        decay_hi_s=0.35,
        resonance_lo=2.0,
        resonance_hi=14.0,
        inner_sat=0.35,
        sub_event_bias=1.6,
        amp_floor=0.30,
        post=_ticks,
    ),
    "FISSION": ReactionProfile(
        name="FISSION",
        cutoff_lo_hz=150.0,
        cutoff_hi_hz=2600.0,
        decay_lo_s=0.08,
        decay_hi_s=0.60,
        resonance_lo=1.5,
        resonance_hi=8.0,
        inner_sat=0.25,
        sub_event_bias=1.0,
        amp_floor=0.45,
        post=_phaser,
    ),
    "TOXIC SLUDGE": ReactionProfile(
        name="TOXIC SLUDGE",
        cutoff_lo_hz=70.0,
        cutoff_hi_hz=900.0,
        decay_lo_s=0.25,
        decay_hi_s=1.60,
        resonance_lo=2.0,
        resonance_hi=10.0,
        inner_sat=0.55,
        sub_event_bias=0.5,
        amp_floor=0.55,
        post=_sludge,
    ),
    "BEAKER": ReactionProfile(
        name="BEAKER",
        cutoff_lo_hz=220.0,
        cutoff_hi_hz=3000.0,
        decay_lo_s=0.02,
        decay_hi_s=0.25,
        resonance_lo=4.0,
        resonance_hi=18.0,
        inner_sat=0.40,
        sub_event_bias=1.8,
        amp_floor=0.28,
        post=_bubble,
    ),
    "ALIEN": ReactionProfile(
        name="ALIEN",
        cutoff_lo_hz=200.0,
        cutoff_hi_hz=5200.0,
        decay_lo_s=0.04,
        decay_hi_s=0.50,
        resonance_lo=3.0,
        resonance_hi=14.0,
        inner_sat=0.30,
        sub_event_bias=1.3,
        amp_floor=0.32,
        post=_shift,
    ),
}
