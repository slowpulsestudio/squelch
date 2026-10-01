"""The reactor core.

Turns scheduled events into control-rate filter movement, then runs the input
through a resonant ladder driven by it. This is where DECAY, RANGE, EXPOSURE,
VOLATILITY, HALF-LIFE, SQUELCH and RODS act.
"""

from __future__ import annotations

import numpy as np

from . import filters, rng
from .controls import Controls
from .params import Params
from .reactions import PROFILES, ReactionProfile, contaminate
from .scheduler import Event, schedule

#: Envelope attack. Long enough that the ramp spans many control blocks, so the
#: envelope glides rather than stepping into a click on short, high-Q events.
ATTACK_S = 0.005

#: Always-on smoothing of the control signals, short enough to leave fast
#: squelch movement intact but long enough to take the staircase off each step.
ANTI_STEP_S = 0.0015

#: Fraction of a reaction's spectral span still travelled at RANGE = 0.
MIN_EXCURSION = 0.45

#: Delay swing available to the pitch wind, and how heavily the wind envelope is
#: slowed before driving it. Pitch change is the delay's rate of change, so the
#: slowing is what keeps a wind musical instead of a glitch.
MAX_WIND_S = 0.020
WIND_SMOOTH_S = 0.030


def _event_envelope(length: int, attack: int, tau: float) -> np.ndarray:
    t = np.arange(length, dtype=float)
    env = np.exp(-t / max(tau, 1e-6))
    if attack > 0:
        ramp = np.minimum(t[:attack] / attack, 1.0)
        env[:attack] *= ramp
    return env


def _one_pole_smooth(x: np.ndarray, coeff: float) -> np.ndarray:
    if coeff <= 0.0:
        return x
    y = np.empty_like(x)
    state = x[0]
    for i, v in enumerate(x):
        state += (v - state) * (1.0 - coeff)
        y[i] = state
    return y


def build_controls(
    events: list[Event], n_samples: int, sr: int, p: Params, profile: ReactionProfile
) -> Controls:
    nb = filters.n_blocks(n_samples)
    ctrl_sr = sr / filters.BLOCK

    damping = p.rods
    squelch_depth = 0.40 + 0.60 * p.squelch
    exposure_curve = np.power(p.exposure, 0.8)

    base_oct = np.log2(profile.cutoff_lo_hz)
    span_oct = np.log2(profile.cutoff_hi_hz / profile.cutoff_lo_hz)
    q_lo, q_hi = profile.resonance_lo, profile.resonance_hi

    env_total = np.zeros(nb)
    cut_oct = np.full(nb, base_oct)
    resonance = np.zeros(nb)

    decay_s = profile.decay_lo_s + (profile.decay_hi_s - profile.decay_lo_s) * p.decay
    hold = p.half_life * 0.85

    # RANGE never closes the sweep down to nothing. At zero the reactor still
    # travels a fraction of its span, so the filter is never parked at its
    # resting cutoff stripping everything above it — prompt.md requires the
    # processed signal to keep a relationship to the input.
    excursion = span_oct * (MIN_EXCURSION + (1.0 - MIN_EXCURSION) * p.range)
    excursion *= 0.55 + 0.45 * squelch_depth
    excursion *= 1.0 - 0.5 * damping

    prev_peak = base_oct
    prev_q = q_lo

    # How far this reaction throws cutoff and Q around from event to event,
    # on top of the ordinary VOLATILITY deviation.
    chaos = profile.chaos * p.volatility

    for ev in events:
        this_decay = max(decay_s * ev.decay_scale, 0.005)
        peak = base_oct + excursion * (0.35 + 0.65 * ev.tone) * ev.intensity
        peak += span_oct * chaos * 0.6 * rng.ubipolar(p.seed, 20, ev.index)
        peak = hold * prev_peak + (1.0 - hold) * peak
        peak = float(np.clip(peak, base_oct - 0.5, base_oct + span_oct + 0.5))

        # Q is reduced once, gently, by each of intensity, SQUELCH and RODS.
        # Stacking three aggressive reductions collapsed it to Q~1.2, which is
        # no resonance at all and left nothing to squelch.
        q = q_lo * np.power(q_hi / q_lo, exposure_curve)
        q *= 0.85 + 0.15 * ev.intensity
        q *= 0.80 + 0.20 * p.squelch
        q *= 1.0 - 0.45 * damping
        q *= 1.0 + chaos * 0.9 * rng.ubipolar(p.seed, 21, ev.index)
        q = float(np.clip(hold * prev_q + (1.0 - hold) * q, 0.7, q_hi * 1.4))

        prev_peak, prev_q = peak, q

        c0 = int(ev.start / filters.BLOCK)
        length = int(this_decay * 6.0 * ctrl_sr)
        c1 = min(c0 + length, nb)
        if c1 <= c0:
            continue

        tau = this_decay * ctrl_sr
        env = _event_envelope(c1 - c0, max(int(ATTACK_S * ctrl_sr), 1), tau)

        env_total[c0:c1] = np.maximum(env_total[c0:c1], env)
        # Overlapping events take the most extreme value rather than averaging.
        # Averaging meant raising REACTIVITY diluted the sweep instead of
        # intensifying it, capping the excursion at ~55% of its span.
        cut_oct[c0:c1] = np.maximum(cut_oct[c0:c1], base_oct + (peak - base_oct) * env)
        resonance[c0:c1] = np.maximum(resonance[c0:c1], q * env)

    # Anti-step smoothing runs always; RODS adds much heavier damping on top.
    cut_oct = filters.smooth(cut_oct, ANTI_STEP_S, sr)
    resonance = filters.smooth(np.maximum(resonance, q_lo * 0.6), ANTI_STEP_S, sr)
    env_total = filters.smooth(env_total, ANTI_STEP_S, sr)

    smoothing = 0.92 * damping
    cut_oct = _one_pole_smooth(cut_oct, smoothing)
    resonance = _one_pole_smooth(resonance, smoothing)

    starts = np.array([ev.start for ev in events], dtype=int)

    return Controls(
        env=env_total,
        cutoff=np.power(2.0, cut_oct),
        resonance=resonance,
        starts=starts,
        damping=damping,
    )


def process(x: np.ndarray, sr: int, p: Params, bpm: float) -> tuple[np.ndarray, Controls]:
    profile = PROFILES[p.reaction]
    events = schedule(x, sr, p, bpm, sub_event_bias=profile.sub_event_bias)
    controls = build_controls(events, len(x), sr, p, profile)

    inner_sat = profile.inner_sat * (0.3 + 0.7 * p.squelch) * (1.0 - 0.7 * p.rods)
    wet = filters.varying_ladder(x, controls.cutoff, controls.resonance, sr, inner_sat=inner_sat)

    depth = (1.0 - profile.amp_floor) * (0.35 + 0.65 * p.squelch) * (1.0 - 0.6 * p.rods)
    amp = filters.to_sample_rate(1.0 - depth + depth * controls.env, len(x))
    wet *= amp[:, None]

    # RANGE is how far a reaction travels in pitch, so it drives the wind as
    # well as the filter excursion.
    wind_depth = p.range * profile.wind_depth * (1.0 - 0.6 * p.rods)
    if wind_depth > 0.0:
        wind = filters.smooth(controls.env, WIND_SMOOTH_S, sr) * wind_depth
        wet = filters.pitch_wind(wet, wind, sr, MAX_WIND_S)

    wet = profile.post(wet, x, controls, p, sr)
    wet = contaminate(wet, controls, p, profile, sr)
    return wet, controls
