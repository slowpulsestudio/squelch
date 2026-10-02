"""The five REACTION types.

Each profile sets the reactor's filter/envelope territory, then contributes one
extra stage that gives that reaction its character. Every number here is a
prototype starting point to be swept and confirmed from Designer feedback, not a
final value — README.md describes these reactions qualitatively only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

import numpy as np
from scipy.ndimage import maximum_filter1d
from scipy.signal import lfilter

from . import filters, rng
from .controls import Controls
from .params import Params

# SPREAD drives each reaction's own signature movement, not only the filter
# sweep, and every one of them reaches zero when SPREAD does.

#: The noise beds follow the incoming audio rather than sitting under it as a
#: constant hiss, so contamination reads as rhythmic.
SIDECHAIN_DEPTH = 0.9
SIDECHAIN_ATTACK_S = 0.005
SIDECHAIN_RELEASE_S = 0.080

#: How slowly the bed's level is matched to the output's. Same reasoning as the
#: output stage: slow enough to settle on a figure rather than ride the music.
BED_MATCH_S = 1.5


@dataclass
class ReactionProfile:
    name: str

    #: Where the ladder rests, and how far SPREAD can carry it above that in
    #: octaves. Octaves rather than hertz so the sweep stays musical wherever
    #: it starts.
    base_hz: float
    sweep_octaves: float
    #: How long the cutoff takes to fall back to rest, at DECAY zero and full.
    tau_lo_s: float
    tau_hi_s: float
    #: Loop feedback. Four is self-oscillation, so this is how close this
    #: reaction gets to singing on its own.
    feedback_lo: float
    feedback_hi: float
    #: Saturation inside the feedback loop.
    drive_lo: float
    drive_hi: float
    #: Which poles are summed: lowpass, bandpass or highpass out of the one
    #: ladder. The weights sum to the tap's gain at DC, so a set that sums to
    #: zero has no low end at all: that is what left FISSION and ALIEN thin and
    #: reading as harsh. A small positive sum keeps the bandpass or highpass
    #: character while putting the body back under it.
    #:
    #: Which stage carries the negative weight sets the top end, because each
    #: stage rolls off 6 dB/octave steeper than the one before it. Moving that
    #: weight down the ladder takes the harshness out without touching the
    #: feedback, which turned out to be worth only 0.2 dB here and is the one
    #: thing that must not be traded away: the feedback is the squelch.
    tap: tuple[float, float, float, float]
    #: Which steps are accented. Accents open the sweep further and push the
    #: feedback closer to oscillation, so they change character and not level.
    accents: tuple[int, ...]

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
    #: "sweep" layers overlapping events and takes the most extreme value.
    #: "acid" is monophonic like a TB-303: each note retriggers the filter
    #: envelope, accented notes open it further, and slid notes glide in.
    voice: str
    #: This reaction's share of the shared pitch wind, scaled by SPREAD.
    wind_depth: float
    #: How FALLOUT disperses this reaction: across the stereo field, or as a
    #: staccato midrange wobble. Reactions can have some of both.
    stereo_weight: float
    wobble_weight: float
    #: This reaction's appetite for DRIVE, and how strongly HALF-LIFE carries
    #: its filter state from one reaction to the next.
    drive_weight: float
    persistence: float
    #: Delivered level of this reaction's noise bed at full CONTAMINATION,
    #: relative to the output's own RMS. Half travel lands 12dB below it.
    noise_full_level: float
    #: The bed generators make noise at their own fixed level rather than
    #: scaling with the input, so this is what each one measures at. The bed is
    #: divided by it and multiplied by the programme level instead.
    noise_unit_rms: float
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
    # Hashes (seed, channel, sample index) rather than drawing from a
    # stateful generator: standard_normal(n) needs the whole render length n
    # up front, which a block-wise processBlock never has (see rng.py).
    left = rng.gaussian_array(n, seed, 0)
    right = rng.gaussian_array(n, seed, 1)
    return np.stack([left, right], axis=1)


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
        divergence = octaves * (0.5 + 1.5 * p.spread)
        band = filters.varying_bandpass(
            noise, 1400.0 * np.power(2.0, spread * divergence), 9.0, sr
        )
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


def _chemical_fizz(wet: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
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


def _input_follower(dry: np.ndarray, sr: int) -> np.ndarray:
    """Envelope of the incoming audio, normalised to average 1.

    Fast attack via a rolling peak, slow release via a one-pole, so the beds
    arrive with the source and fall away in the gaps.

    Both halves look backwards only. A centred rolling peak reads a couple of
    milliseconds into the future, and dividing by the envelope's whole-buffer
    mean reads all of it.
    """
    magnitude = np.abs(dry).max(axis=1)
    size = max(int(SIDECHAIN_ATTACK_S * sr), 1)
    peak = maximum_filter1d(magnitude, size=size, origin=(size - 1) // 2)
    coeff = float(np.exp(-1.0 / max(SIDECHAIN_RELEASE_S * sr, 1.0)))
    envelope = lfilter([1.0 - coeff], [1.0, -coeff], peak)
    return envelope / np.maximum(filters.running_mean(envelope, sr, BED_MATCH_S), 1e-12)


def contaminate(
    wet: np.ndarray,
    dry: np.ndarray,
    c: Controls,
    p: Params,
    profile: ReactionProfile,
    sr: int,
    md=None,
) -> np.ndarray:
    """Add this reaction's noise bed at a predictable delivered level.

    The bed is sidechained by the input so it moves with the music rather than
    sitting under it as a constant hiss, then normalised against the output's
    own RMS before scaling, so CONTAMINATION maps to a level in dB rather than
    to whatever the gain chain happened to leave. Squaring the control puts
    full travel 12dB above half.
    """
    amount = md.samples("contamination") if md else np.full(len(wet), p.contamination)
    if amount.max() <= 0.0:
        return wet

    bed = profile.noise(wet, c, p, sr)
    follower = _input_follower(dry, sr)
    bed = bed * (1.0 - SIDECHAIN_DEPTH + SIDECHAIN_DEPTH * follower)[:, None]

    # Normalising the bed against the output's level is what makes
    # CONTAMINATION map to a predictable level in dB rather than to whatever
    # the gain chain happened to leave. Both levels are tracked by the same
    # slow one-pole the output stage uses, so nothing here measures a part of
    # the render that has not played yet.
    # The bed is referenced to the programme's level and to a constant, never
    # to its own running level. Dividing by its own level sounds reasonable and
    # is not: a sparse bed like RADIATION's ticks decays towards silence
    # between them, so the gain runs away in the gaps and the next tick arrives
    # enormous. Measured across the five beds, no single time constant worked
    # for all of them — RADIATION wanted two minutes and CHEMICAL wanted a
    # second and a half — which is the sign that the mechanism is wrong rather
    # than the tuning.
    wet_level = filters.running_rms(wet, sr, BED_MATCH_S)
    bed = bed * (wet_level / profile.noise_unit_rms)[:, None]
    level = profile.noise_full_level * amount**2 * (1.0 - 0.5 * c.damping)
    return wet + bed * level[:, None]


def _bubble(wet: np.ndarray, dry: np.ndarray, c: Controls, p: Params, sr: int) -> np.ndarray:
    """CHEMICAL's character is its acid voice, which the reactor applies."""
    return wet



PROFILES = {
    "RADIATION": ReactionProfile(
        name="RADIATION",
        base_hz=220.0,
        sweep_octaves=4.4,
        tau_lo_s=0.03,
        tau_hi_s=0.22,
        feedback_lo=1.6,
        feedback_hi=3.55,
        drive_lo=1.1,
        drive_hi=2.6,
        tap=(0.0, 0.0, 0.0, 1.0),
        accents=(0, 0, 1, 0, 1, 0, 0, 1),
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
        voice="sweep",
        wind_depth=1.0,
        stereo_weight=0.3,
        wobble_weight=1.0,
        drive_weight=0.90,
        persistence=0.90,
        noise_full_level=0.0229,
        noise_unit_rms=0.005499,
        noise=_geiger_ticks,
        post=_passthrough,
    ),
    "FISSION": ReactionProfile(
        name="FISSION",
        base_hz=320.0,
        sweep_octaves=3.2,
        tau_lo_s=0.06,
        tau_hi_s=0.38,
        feedback_lo=2.0,
        feedback_hi=3.82,
        drive_lo=1.0,
        drive_hi=2.1,
        tap=(0.0, 0.0, -0.65, 1.0),
        accents=(0, 1, 0, 0, 1, 0, 1, 0),
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
        voice="sweep",
        wind_depth=0.45,
        stereo_weight=1.0,
        wobble_weight=0.25,
        drive_weight=0.70,
        persistence=0.60,
        noise_full_level=0.0902,
        noise_unit_rms=0.069393,
        noise=_fission_shimmer,
        # Mechanism lives in reactor._fission_engine; post is a passthrough.
        post=_bubble,
    ),
    "SLUDGE": ReactionProfile(
        name="SLUDGE",
        base_hz=100.0,
        sweep_octaves=2.4,
        tau_lo_s=0.12,
        tau_hi_s=0.7,
        feedback_lo=1.4,
        feedback_hi=2.95,
        drive_lo=1.6,
        drive_hi=3.6,
        tap=(0.0, 0.3, 0.0, 0.8),
        accents=(0, 0, 0, 1, 0, 0, 1, 0),
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
        voice="sweep",
        wind_depth=0.85,
        stereo_weight=0.2,
        wobble_weight=0.9,
        drive_weight=1.30,
        persistence=1.00,
        noise_full_level=0.1373,
        noise_unit_rms=0.099999,
        noise=_sludge_rumble,
        # SLUDGE's mechanism lives in reactor._sludge_engine now, not a post
        # hook on top of a filter voice it no longer has.
        post=_bubble,
    ),
    "CHEMICAL": ReactionProfile(
        name="CHEMICAL",
        base_hz=180.0,
        sweep_octaves=4.8,
        tau_lo_s=0.035,
        tau_hi_s=0.26,
        feedback_lo=1.5,
        feedback_hi=3.25,
        drive_lo=1.1,
        drive_hi=2.5,
        tap=(0.0, 0.0, 0.0, 1.0),
        accents=(0, 0, 1, 0, 0, 1, 0, 1),
        cutoff_lo_hz=180.0,
        cutoff_hi_hz=3800.0,
        decay_lo_s=0.03,
        decay_hi_s=0.90,
        resonance_lo=4.0,
        resonance_hi=20.0,
        inner_sat=0.45,
        sub_event_bias=2.0,
        amp_floor=0.30,
        chaos=0.45,
        voice="acid",
        wind_depth=0.6,
        stereo_weight=0.35,
        wobble_weight=0.55,
        drive_weight=1.00,
        # A 303 retriggers cleanly; heavy carry-over blunts the per-note
        # envelope that the whole voice depends on.
        persistence=0.45,
        # Dense continuous fizz in the most sensitive part of the ear's range
        # reads louder than its level suggests, so it sits below the others.
        noise_full_level=0.0530,
        noise_unit_rms=0.033510,
        noise=_chemical_fizz,
        post=_bubble,
    ),
    "ALIEN": ReactionProfile(
        name="ALIEN",
        base_hz=400.0,
        sweep_octaves=5.2,
        tau_lo_s=0.02,
        tau_hi_s=0.16,
        feedback_lo=2.2,
        feedback_hi=3.6,
        drive_lo=1.0,
        drive_hi=1.8,
        tap=(0.5, -1.15, 0.0, 1.0),
        accents=(1, 0, 1, 0, 1, 1, 0, 1),
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
        voice="sweep",
        wind_depth=1.0,
        stereo_weight=0.5,
        wobble_weight=1.0,
        drive_weight=0.80,
        persistence=0.70,
        noise_full_level=0.1147,
        noise_unit_rms=0.045540,
        noise=_alien_whirr,
        # ALIEN's mechanism lives in reactor._alien_engine now, as an actual
        # event-gated source oscillator, not a frequency-shifted filter voice.
        post=_bubble,
    ),
}
