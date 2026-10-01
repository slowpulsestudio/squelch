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
from scipy.signal import lfilter

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
    #: "ladder" for a 4-pole lowpass, "bandpass" for a resonant band.
    filter_mode: str
    #: When true the filter holds each event's frequency instead of sweeping to
    #: it, so successive events read as steps rather than as a glide.
    stepped: bool
    #: This reaction's share of the shared pitch wind, scaled by RANGE.
    wind_depth: float
    #: Delivered level of this reaction's noise bed at full CONTAMINATION,
    #: relative to the output's own RMS. Half travel lands 12dB below it.
    noise_full_level: float
    #: Builds this reaction's noise bed. Level is applied by contaminate().
    noise: Callable[[np.ndarray, Controls, Params, int], np.ndarray]
    post: Callable[[np.ndarray, np.ndarray, Controls, Params, int], np.ndarray]


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


def _passthrough(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """RADIATION's character is its filter movement, wind and sparse ticks."""
    return wet


def _noise(n: int, seed: int) -> np.ndarray:
    return np.random.default_rng(seed).standard_normal((n, 2))


def _ctrl_time(c: Controls, sr: int) -> np.ndarray:
    return np.arange(len(c.env)) / (sr / filters.BLOCK)


def _event_burst_envelope(
    n: int, starts: np.ndarray, length: int, attack: int = 0
) -> np.ndarray:
    """A short decaying spike at each event start, at sample rate."""
    shape = np.exp(-np.linspace(0.0, 6.0, length))
    if attack > 0:
        ramp = np.minimum(np.arange(length) / attack, 1.0)
        shape = shape * ramp
    env = np.zeros(n)
    for s in starts:
        end = min(s + length, n)
        if end <= s:
            continue
        env[s:end] = np.maximum(env[s:end], shape[: end - s])
    return env


def _geiger_ticks(wet: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Sparse bright grains on an unpredictable subset of events.

    The onset stays fast, because a fast onset is what makes a tick a tick. It
    is kept legible only by level, not by blunting its shape.
    """
    n = wet.shape[0]
    emitting = np.array(
        [s for i, s in enumerate(c.starts) if rng.urand(p.seed, 30, i) < 0.30], dtype=int
    )
    if len(emitting) == 0:
        return np.zeros_like(wet)

    burst = _event_burst_envelope(
        n, emitting, int(0.008 * sr), attack=max(int(0.0002 * sr), 1)
    )[:, None]
    noise = filters.static_highpass(_noise(n, p.seed + 1), 2000.0, sr, q=0.8)
    noise = filters.static_lowpass(noise, 6000.0, sr, q=0.8)
    return noise * burst


def _fission_shimmer(wet: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Continuous metallic bed whose resonant peaks drift apart over time."""
    n = wet.shape[0]
    t = _ctrl_time(c, sr)
    spread = 0.5 - 0.5 * np.cos(2.0 * np.pi * 0.07 * t)
    noise = _noise(n, p.seed + 2)

    bed = np.zeros_like(wet)
    for i, octaves in enumerate((-0.65, 0.0, 0.8)):
        band = filters.varying_bandpass(noise, 1400.0 * np.power(2.0, spread * octaves * 1.6), 9.0, sr)
        pan = 0.5 + 0.5 * np.cos(i * 2.1)
        bed += band * np.array([pan, 1.0 - pan])

    amp = filters.to_sample_rate(0.35 + 0.65 * filters.smooth(c.env, 0.05, sr), n)
    return bed * amp[:, None]


def _sludge_rumble(wet: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Continuous low pressure bed with no onsets at all."""
    n = wet.shape[0]
    bed = filters.static_lowpass(_noise(n, p.seed + 3), 420.0, sr, q=0.7)
    bed = filters.static_highpass(bed, 55.0, sr, q=0.7)
    bed = np.tanh(bed * 1.6)

    amp = filters.to_sample_rate(0.30 + 0.70 * filters.smooth(c.env, 0.25, sr), n)
    return bed * amp[:, None]


def _flatten_level(x: np.ndarray, sr: int, seconds: float = 0.015, amount: float = 0.8) -> np.ndarray:
    """Take the level swing out of a signal while leaving its spectral motion.

    A resonant bandpass swept across noise swings in level by as much as it
    changes in timbre, which reads as individual events rather than a texture.
    """
    coeff = float(np.exp(-1.0 / max(seconds * sr, 1.0)))
    magnitude = np.abs(x).mean(axis=1)
    envelope = lfilter([1.0 - coeff], [1.0, -coeff], magnitude)
    reference = np.sqrt(np.mean(magnitude**2)) + 1e-12
    return x / (np.power((envelope + 1e-9) / reference, amount)[:, None])


def _beaker_fizz(wet: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """Overlapping micro-grains, dense enough to fuse into carbonation."""
    n = wet.shape[0]
    blocks = len(c.env)

    hold = max(int(0.003 * sr / filters.BLOCK), 1)
    steps = int(np.ceil(blocks / hold))
    jumps = np.repeat(np.array([rng.urand(p.seed, 50, i) for i in range(steps)]), hold)[:blocks]
    jumps = filters.smooth(jumps, 0.001, sr)

    bed = filters.varying_bandpass(
        _noise(n, p.seed + 4), 600.0 * np.power(2.0, 2.7 * jumps), 4.0, sr
    )
    bed = _flatten_level(bed, sr)

    # Grains must overlap several deep to fuse into carbonation. At a rate that
    # merely fills the timeline they stay individually countable, which is the
    # ticking this bed exists to avoid.
    rate = 420.0 + 900.0 * p.reactivity
    count = max(int(rate * n / sr), 1)
    grain = int(0.006 * sr)
    shape = np.exp(-np.linspace(0.0, 5.0, grain)) * np.minimum(
        np.arange(grain) / max(int(0.0005 * sr), 1), 1.0
    )
    env = np.zeros(n)
    for i in range(count):
        start = int(rng.urand(p.seed, 51, i) * n)
        end = min(start + grain, n)
        if end > start:
            env[start:end] += shape[: end - start]
    env /= env.max() + 1e-12

    return bed * env[:, None]


def _alien_whirr(wet: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """A hovering craft: a narrow resonance rotating, with a detuned second
    rotation beating against it and the whole thing circling the stereo field."""
    n = wet.shape[0]
    t = _ctrl_time(c, sr)
    drift = 0.5 - 0.5 * np.cos(2.0 * np.pi * 0.08 * t)
    base = 700.0 * np.power(2.0, 1.2 * drift)
    noise = _noise(n, p.seed + 5)

    bed = np.zeros_like(wet)
    for channel, phase in ((0, 0.0), (1, np.pi * 0.5)):
        rotation = np.sin(2.0 * np.pi * 5.0 * t + phase) + 0.7 * np.sin(
            2.0 * np.pi * 5.9 * t + phase * 1.3
        )
        bed[:, channel : channel + 1] = filters.varying_bandpass(
            noise[:, channel : channel + 1], base * np.power(2.0, 0.55 * rotation), 11.0, sr
        )[:, 0:1]

    amp = filters.to_sample_rate(0.45 + 0.55 * filters.smooth(c.env, 0.12, sr), n)
    return bed * amp[:, None]


def contaminate(
    wet: np.ndarray, c: Controls, p: Params, profile: ReactionProfile, sr: int
) -> np.ndarray:
    """Add this reaction's noise bed at a predictable delivered level.

    The bed is normalised against the output's own RMS before scaling, so
    CONTAMINATION maps to a level in dB rather than to whatever the gain chain
    happened to leave. Squaring the control puts full travel 12dB above half.
    """
    if p.contamination <= 0.0:
        return wet

    bed = profile.noise(wet, c, p, sr)
    bed_rms = np.sqrt(np.mean(bed**2))
    if bed_rms < 1e-12:
        return wet

    bed *= (np.sqrt(np.mean(wet**2)) + 1e-12) / bed_rms
    level = profile.noise_full_level * p.contamination**2 * (1.0 - 0.5 * c.damping)
    return wet + bed * level


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
    """BEAKER's character is its stepping bandpass, which the reactor applies."""
    return wet


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
        filter_mode="ladder",
        stepped=False,
        wind_depth=1.0,
        noise_full_level=0.0398,
        noise=_geiger_ticks,
        post=_passthrough,
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
        filter_mode="ladder",
        stepped=False,
        wind_depth=0.45,
        noise_full_level=0.1259,
        noise=_fission_shimmer,
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
        filter_mode="ladder",
        stepped=False,
        wind_depth=0.85,
        noise_full_level=0.1995,
        noise=_sludge_rumble,
        post=_sludge,
    ),
    "BEAKER": ReactionProfile(
        name="BEAKER",
        cutoff_lo_hz=300.0,
        cutoff_hi_hz=2600.0,
        decay_lo_s=0.012,
        decay_hi_s=0.16,
        resonance_lo=4.0,
        resonance_hi=16.0,
        inner_sat=0.40,
        sub_event_bias=3.0,
        amp_floor=0.28,
        chaos=1.0,
        filter_mode="bandpass",
        stepped=True,
        wind_depth=0.6,
        noise_full_level=0.1000,
        noise=_beaker_fizz,
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
        filter_mode="ladder",
        stepped=False,
        wind_depth=1.0,
        noise_full_level=0.1585,
        noise=_alien_whirr,
        post=_shift,
    ),
}
