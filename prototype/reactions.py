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

from . import filters, rng
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
    #: How violently cutoff and Q jump between events, scaled by VOLATILITY.
    chaos: float
    #: This reaction's share of the shared pitch wind, scaled by RANGE.
    wind_depth: float
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


def _held_random(c: Controls, p: Params, stream: int, n_blocks: int) -> np.ndarray:
    """One random value per event, held until the next event starts."""
    held = np.zeros(n_blocks)
    if len(c.starts) == 0:
        return held
    edges = np.clip(c.starts // filters.BLOCK, 0, n_blocks - 1)
    for i, start in enumerate(edges):
        end = edges[i + 1] if i + 1 < len(edges) else n_blocks
        held[start:end] = rng.urand(p.seed, stream, i)
    return held


def _ticks(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Geiger ticks: short synthetic noise bursts on a fraction of events.

    Normalised against the output's own level before scaling, so the delivered
    loudness is predictable instead of whatever the gain chain happened to
    leave. Previously these measured -46 to -110 dB and were never audible.
    """
    n = wet.shape[0]
    emitting = np.array(
        [s for i, s in enumerate(c.starts) if rng.urand(p.seed, 30, i) < 0.45], dtype=int
    )
    if len(emitting) == 0:
        return wet

    burst = _event_burst_envelope(n, emitting, int(0.009 * sr))[:, None]
    noise = np.random.default_rng(p.seed + 1).standard_normal((n, 2))
    noise = filters.static_highpass(noise, 1800.0, sr, q=0.8)
    # Capped well below the top octave: prompt.md rules out HF harshness.
    noise = filters.static_lowpass(noise, 6500.0, sr, q=0.8)

    ticks = noise * burst
    ticks *= (np.sqrt(np.mean(wet**2)) + 1e-12) / (np.sqrt(np.mean(ticks**2)) + 1e-12)
    level = 0.45 * (0.35 + 0.65 * p.squelch) * (1.0 - 0.7 * c.damping)
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
    """Submerged: swept notches instead of peaks, under an octave-down body.

    Inverted resonance reads as hollow and underwater where a resonant peak
    would read as acidic.
    """
    sub = filters.static_lowpass(filters.octave_down(wet), 180.0, sr, q=0.7)
    body = wet + sub * (0.45 * p.squelch * (1.0 - c.damping))

    slow = filters.smooth(c.env, 0.08, sr)
    notch = 260.0 * np.power(2.0, 1.8 * slow)
    y = filters.varying_notch(body, notch, 1.6, sr)
    y = filters.varying_notch(y, notch * 1.9, 1.6, sr)
    y = filters.static_lowpass(y, 900.0 + 1200.0 * p.squelch, sr, q=0.7)
    return np.tanh(y * (1.0 + 1.2 * p.squelch)) / (1.0 + 0.7 * p.squelch)


def _bubble(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Boiling: a second resonant peak that jumps to a new frequency per event."""
    blocks = len(c.env)
    spread = 0.3 + 0.7 * p.volatility
    jump_oct = _held_random(c, p, 40, blocks) * 3.4 * spread
    jump = 350.0 * np.power(2.0, 0.6 + jump_oct + 1.2 * c.env)
    jump = filters.smooth(jump, 0.002, sr)

    q_hold = 4.0 + 16.0 * p.exposure * (0.3 + 0.7 * _held_random(c, p, 41, blocks))
    q_hold = filters.smooth(q_hold, 0.002, sr) * (1.0 - 0.6 * c.damping)

    second = filters.varying_ladder(wet, jump, q_hold, sr, inner_sat=0.25)
    return 0.5 * wet + 0.5 * second


def _shift(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Non-terrestrial: single-sideband frequency sweeps tracking each event."""
    depth = 60.0 + 900.0 * p.range
    shifted = filters.frequency_shift(wet, c.env * depth, sr)
    amount = 0.55 * p.squelch * (1.0 - c.damping)
    return (1.0 - amount) * wet + amount * shifted


PROFILES = {
    "RADIATION": ReactionProfile(
        name="RADIATION",
        cutoff_lo_hz=420.0,
        cutoff_hi_hz=7500.0,
        decay_lo_s=0.025,
        decay_hi_s=0.30,
        resonance_lo=3.0,
        resonance_hi=16.0,
        inner_sat=0.35,
        sub_event_bias=2.0,
        amp_floor=0.30,
        chaos=0.55,
        wind_depth=1.0,
        post=_ticks,
    ),
    "FISSION": ReactionProfile(
        name="FISSION",
        cutoff_lo_hz=340.0,
        cutoff_hi_hz=3600.0,
        decay_lo_s=0.08,
        decay_hi_s=0.60,
        resonance_lo=1.5,
        resonance_hi=8.0,
        inner_sat=0.25,
        sub_event_bias=1.0,
        amp_floor=0.45,
        chaos=0.25,
        wind_depth=0.45,
        post=_phaser,
    ),
    "TOXIC SLUDGE": ReactionProfile(
        name="TOXIC SLUDGE",
        cutoff_lo_hz=150.0,
        cutoff_hi_hz=1300.0,
        decay_lo_s=0.25,
        decay_hi_s=1.60,
        resonance_lo=2.0,
        resonance_hi=10.0,
        inner_sat=0.55,
        sub_event_bias=0.5,
        amp_floor=0.55,
        chaos=0.70,
        wind_depth=0.85,
        post=_sludge,
    ),
    "BEAKER": ReactionProfile(
        name="BEAKER",
        cutoff_lo_hz=480.0,
        cutoff_hi_hz=4200.0,
        decay_lo_s=0.012,
        decay_hi_s=0.16,
        resonance_lo=5.0,
        resonance_hi=22.0,
        inner_sat=0.40,
        sub_event_bias=3.0,
        amp_floor=0.28,
        chaos=1.0,
        wind_depth=0.6,
        post=_bubble,
    ),
    "ALIEN": ReactionProfile(
        name="ALIEN",
        cutoff_lo_hz=440.0,
        cutoff_hi_hz=6500.0,
        decay_lo_s=0.04,
        decay_hi_s=0.50,
        resonance_lo=3.0,
        resonance_hi=14.0,
        inner_sat=0.30,
        sub_event_bias=1.3,
        amp_floor=0.32,
        chaos=0.6,
        wind_depth=1.0,
        post=_shift,
    ),
}
