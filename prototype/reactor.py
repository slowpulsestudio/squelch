"""The reactor core.

Turns scheduled events into control-rate filter movement, then runs the input
through a resonant ladder driven by it. This is where DECAY, RANGE, EXPOSURE,
VOLATILITY, HALF-LIFE, SQUELCH and RODS act.
"""

from __future__ import annotations

import numpy as np

from . import filters
from .controls import Controls
from .params import Params
from .reactions import PROFILES, ReactionProfile
from .scheduler import Event, schedule

#: Envelope attack, long enough to avoid a click on the filter sweep.
ATTACK_S = 0.002


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
    cut_num = np.zeros(nb)
    cut_den = np.zeros(nb)
    res_num = np.zeros(nb)

    decay_s = profile.decay_lo_s + (profile.decay_hi_s - profile.decay_lo_s) * p.decay
    hold = p.half_life * 0.85

    prev_peak = base_oct
    prev_q = q_lo

    for ev in events:
        this_decay = max(decay_s * ev.decay_scale, 0.005)
        excursion = span_oct * p.range * squelch_depth * (1.0 - 0.75 * damping)
        peak = base_oct + excursion * (0.35 + 0.65 * ev.tone) * ev.intensity
        peak = hold * prev_peak + (1.0 - hold) * peak

        q = q_lo * np.power(q_hi / q_lo, exposure_curve) * ev.intensity
        q *= 0.35 + 0.65 * p.squelch
        q *= 1.0 - 0.8 * damping
        q = max(hold * prev_q + (1.0 - hold) * q, 0.5)

        prev_peak, prev_q = peak, q

        c0 = int(ev.start / filters.BLOCK)
        length = int(this_decay * 6.0 * ctrl_sr)
        c1 = min(c0 + length, nb)
        if c1 <= c0:
            continue

        tau = this_decay * ctrl_sr
        env = _event_envelope(c1 - c0, max(int(ATTACK_S * ctrl_sr), 1), tau)

        env_total[c0:c1] = np.maximum(env_total[c0:c1], env)
        cut_num[c0:c1] += env * (base_oct + (peak - base_oct) * env)
        cut_den[c0:c1] += env
        res_num[c0:c1] += env * q

    active = cut_den > 1e-9
    cut_oct = np.full(nb, base_oct)
    cut_oct[active] = cut_num[active] / cut_den[active]
    resonance = np.full(nb, q_lo * 0.6)
    resonance[active] = res_num[active] / cut_den[active]

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

    wet = profile.post(wet, x, controls, p, sr)
    return wet, controls
