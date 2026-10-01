"""The reactor core.

Turns scheduled events into control-rate filter movement, then runs the input
through a resonant ladder driven by it. This is where DECAY, SPREAD, EXPOSURE,
VOLATILITY, HALF-LIFE, TOXICITY and CONTAINMENT act.
"""

from __future__ import annotations

import numpy as np

from . import filters, rng
from .controls import Controls
from .meltdown import Meltdown
from .params import Params
from .reactions import PROFILES, ReactionProfile, contaminate
from .reverb import reverb
from .scheduler import Event, schedule

#: Envelope attack. Long enough that the ramp spans many control blocks, so the
#: envelope glides rather than stepping into a click on short, high-Q events.
ATTACK_S = 0.005

#: Always-on smoothing of the control signals, short enough to leave fast
#: squelch movement intact but long enough to take the staircase off each step.
ANTI_STEP_S = 0.0015

#: Where a fully-closed sweep rests, as a fraction up the reaction's span. A
#: static filter parked at the bottom of its range is just mud.
STATIC_CENTRE = 0.45

#: Delay swing available to the pitch wind, and how heavily the wind envelope is
#: slowed before driving it. Pitch change is the delay's rate of change, so the
#: slowing is what keeps a wind musical instead of a glitch.
MAX_WIND_S = 0.020
WIND_SMOOTH_S = 0.030

#: Acid voice. An accented note opens the filter further and rings harder;
#: unaccented notes sit back in level, which is what gives a 303 line its
#: internal rhythm. Slid notes glide in over SLIDE_S instead of jumping.
ACCENT_ENV_MOD = 1.45
ACCENT_RESONANCE = 1.30
UNACCENTED_LEVEL = 0.74
SLIDE_S = 0.060

#: How far VOLATILITY bends an event envelope away from a plain exponential.
SHAPE_RANGE = 0.8

#: How far IONIZE throws an event's resonant peak from where the sweep left it.
IONIZE_SPECTRAL = 0.55

#: Placement glides over this, so an event landing hard left does not click.
PAN_SMOOTH_S = 0.004

#: AFTERGLOW's voicing. Damped well down so a long tail stays warm rather than
#: hissing on top of the reaction.
AFTERGLOW_DAMPING = 0.45
AFTERGLOW_LEVEL = 0.9


def _event_envelope(length: int, attack: int, tau: float, curve: float = 1.0) -> np.ndarray:
    t = np.arange(length, dtype=float)
    env = np.power(np.exp(-t / max(tau, 1e-6)), curve)
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
    events: list[Event],
    n_samples: int,
    sr: int,
    p: Params,
    profile: ReactionProfile,
    md: Meltdown | None = None,
) -> Controls:
    nb = filters.n_blocks(n_samples)
    ctrl_sr = sr / filters.BLOCK

    # An unfired MELTDOWN is inert, so every parameter reads as its knob value.
    md = md or Meltdown(p, n_samples, sr)

    spread_ctrl = md.ctrl("spread")
    toxicity_ctrl = md.ctrl("toxicity")
    containment_ctrl = md.ctrl("containment")
    exposure_ctrl = md.ctrl("exposure")

    base_oct = np.log2(profile.cutoff_lo_hz)
    span_oct = np.log2(profile.cutoff_hi_hz / profile.cutoff_lo_hz)
    q_lo, q_hi = profile.resonance_lo, profile.resonance_hi

    # As SPREAD closes the sweep down, the filter's resting point rises to meet
    # it, so a static filter sits in the middle of its range rather than parked
    # at the bottom stripping everything above it.
    resting_ctrl = base_oct + span_oct * STATIC_CENTRE * (1.0 - spread_ctrl)

    env_total = np.zeros(nb)
    pan_gain = np.ones((nb, 2))
    pan_loudest = np.zeros(nb)
    send = np.zeros(nb)
    cut_oct = resting_ctrl.copy()
    resonance = np.zeros(nb)

    #: IONIZE scatters each event across stereo, spectrum and depth at once.
    ionize = p.ionize_amount if p.ionize else 0.0

    decay_s = profile.decay_lo_s + (profile.decay_hi_s - profile.decay_lo_s) * p.decay
    hold = p.half_life * 0.85 * profile.persistence

    prev_peak = float(resting_ctrl[0])
    prev_q = q_lo

    acid = profile.voice == "acid"
    slide_blocks = max(int(SLIDE_S * ctrl_sr), 1)
    held_oct = prev_peak

    for ev in events:
        block = min(max(ev.start // filters.BLOCK, 0), nb - 1)
        spread = float(spread_ctrl[block])
        toxicity = float(toxicity_ctrl[block])
        damping = float(containment_ctrl[block])
        resting_oct = float(resting_ctrl[block])

        # How far this reaction throws cutoff and Q around from event to event,
        # on top of the ordinary VOLATILITY deviation.
        chaos = profile.chaos * p.volatility
        excursion = span_oct * spread
        excursion *= 0.55 + 0.45 * (0.40 + 0.60 * toxicity)
        excursion *= 1.0 - 0.5 * damping

        this_decay = max(decay_s * ev.decay_scale, 0.005)
        peak = resting_oct + excursion * (0.35 + 0.65 * ev.tone) * ev.intensity
        peak += span_oct * chaos * 0.6 * spread * rng.ubipolar(p.seed, 20, ev.index)
        # Each event pops into its own register rather than all of them
        # landing wherever the sweep happens to be.
        peak += span_oct * ionize * IONIZE_SPECTRAL * rng.ubipolar(p.seed, 22, ev.index)
        if acid and ev.accent:
            peak = resting_oct + (peak - resting_oct) * ACCENT_ENV_MOD
        peak = hold * prev_peak + (1.0 - hold) * peak
        peak = float(np.clip(peak, base_oct - 0.5, base_oct + span_oct + 0.5))

        # Q is reduced once, gently, by each of intensity, TOXICITY and
        # CONTAINMENT. Stacking three aggressive reductions collapsed it to
        # Q~1.2, which is no resonance at all and left nothing to squelch.
        q = q_lo * np.power(q_hi / q_lo, np.power(float(exposure_ctrl[block]), 0.8))
        q *= 0.85 + 0.15 * ev.intensity
        q *= 0.80 + 0.20 * toxicity
        q *= 1.0 - 0.45 * damping
        q *= 1.0 + chaos * 0.9 * rng.ubipolar(p.seed, 21, ev.index)
        if acid and ev.accent:
            q *= ACCENT_RESONANCE
        q = float(np.clip(hold * prev_q + (1.0 - hold) * q, 0.7, q_hi * 1.4))

        # prompt.md lists stereo position among the things VOLATILITY varies.
        pan_spread = p.volatility * (1.0 - 0.7 * damping)
        placement = ev.pan
        if ionize > 0.0:
            # Consecutive events throw to opposite sides, which is what makes
            # it read as bouncing rather than merely wide.
            side = 1.0 if ev.index % 2 else -1.0
            scattered = side * (0.55 + 0.45 * abs(ev.pan))
            pan_spread = max(pan_spread, ionize)
            placement = ev.pan * (1.0 - ionize) + scattered * ionize

        prev_peak, prev_q = peak, q

        c0 = int(ev.start / filters.BLOCK)
        length = int(this_decay * 6.0 * ctrl_sr)
        c1 = min(c0 + length, nb)
        if c1 <= c0:
            continue

        tau = this_decay * ctrl_sr
        # VOLATILITY varies the envelope's shape and attack, not just its
        # length: a soft swell and a sharp pluck are different events.
        curve = 1.0 + SHAPE_RANGE * p.volatility * (ev.shape * 2.0 - 1.0)
        attack = ATTACK_S * (1.0 + 2.5 * p.volatility * ev.shape)
        env = _event_envelope(c1 - c0, max(int(attack * ctrl_sr), 1), tau, curve)
        accent_level = 1.0 if (not acid or ev.accent) else UNACCENTED_LEVEL

        env_total[c0:c1] = np.maximum(env_total[c0:c1], env * accent_level)

        # Each event takes its own stereo position, so successive reactions
        # bounce across the field instead of all arriving dead centre. The
        # loudest event present owns the placement; averaging overlapping
        # events pulls every one of them back to the middle.
        angle = (placement * pan_spread + 1.0) * 0.25 * np.pi
        louder = np.flatnonzero(env > pan_loudest[c0:c1]) + c0
        pan_gain[louder, 0] = np.cos(angle) * np.sqrt(2.0)
        pan_gain[louder, 1] = np.sin(angle) * np.sqrt(2.0)
        pan_loudest[c0:c1] = np.maximum(pan_loudest[c0:c1], env)

        # Depth: with IONIZE every event sits at its own distance, otherwise
        # they all share AFTERGLOW's single send.
        distance = 1.0 - ionize + ionize * (0.15 + 1.5 * ev.depth)
        send[c0:c1] = np.maximum(send[c0:c1], env * distance)
        if acid:
            # Monophonic, like the machine this imitates: each note retriggers
            # the filter envelope and owns the line until the next one starts.
            curve = resting_oct + (peak - resting_oct) * env
            if ev.slide:
                glide = min(slide_blocks, len(curve))
                ramp = np.linspace(0.0, 1.0, glide)
                curve[:glide] = held_oct * (1.0 - ramp) + curve[:glide] * ramp
            cut_oct[c0:c1] = curve
            resonance[c0:c1] = q
            held_oct = float(curve[-1])
        else:
            # Overlapping events take the most extreme value rather than
            # averaging. Averaging meant raising REACTIVITY diluted the sweep
            # instead of intensifying it, capping it at ~55% of its span.
            cut_oct[c0:c1] = np.maximum(cut_oct[c0:c1], resting_oct + (peak - resting_oct) * env)
            resonance[c0:c1] = np.maximum(resonance[c0:c1], q * env)

    # Anti-step smoothing runs always; CONTAINMENT adds heavier damping on top.
    cut_oct = filters.smooth(cut_oct, ANTI_STEP_S, sr)
    resonance = filters.smooth(np.maximum(resonance, q_lo * 0.6), ANTI_STEP_S, sr)
    env_total = filters.smooth(env_total, ANTI_STEP_S, sr)
    pan_gain = np.stack(
        [filters.smooth(pan_gain[:, 0], PAN_SMOOTH_S, sr),
         filters.smooth(pan_gain[:, 1], PAN_SMOOTH_S, sr)],
        axis=1,
    )

    smoothing = 0.92 * float(containment_ctrl.min())
    cut_oct = _one_pole_smooth(cut_oct, smoothing)
    resonance = _one_pole_smooth(resonance, smoothing)

    starts = np.array([ev.start for ev in events], dtype=int)

    return Controls(
        env=env_total,
        pan_gain=pan_gain,
        send=filters.smooth(send, ANTI_STEP_S, sr),
        cutoff=np.power(2.0, cut_oct),
        resonance=resonance,
        starts=starts,
        damping=p.containment,
    )


def process(x: np.ndarray, sr: int, p: Params, bpm: float) -> tuple[np.ndarray, Controls]:
    profile = PROFILES[p.reaction]
    md = Meltdown(p, len(x), sr)
    events = schedule(x, sr, p, bpm, sub_event_bias=profile.sub_event_bias, md=md)
    controls = build_controls(events, len(x), sr, p, profile, md)

    toxicity = md.ctrl("toxicity")
    containment = md.ctrl("containment")

    inner_sat = profile.inner_sat * (0.3 + 0.7 * toxicity) * (1.0 - 0.7 * containment)
    wet = filters.varying_ladder(x, controls.cutoff, controls.resonance, sr, inner_sat=inner_sat)

    depth = (1.0 - profile.amp_floor) * (0.35 + 0.65 * toxicity) * (1.0 - 0.6 * containment)
    articulation = filters.to_sample_rate(1.0 - depth + depth * controls.env, len(x))
    placement = np.stack(
        [filters.to_sample_rate(controls.pan_gain[:, ch], len(x)) for ch in (0, 1)],
        axis=1,
    )
    wet *= articulation[:, None] * placement

    # SPREAD is how far a reaction travels in pitch, so it drives the wind as
    # well as the filter excursion.
    wind_depth = md.ctrl("spread") * profile.wind_depth * (1.0 - 0.6 * containment)
    if wind_depth.max() > 0.0:
        wind = filters.smooth(controls.env, WIND_SMOOTH_S, sr) * wind_depth
        wet = filters.pitch_wind(wet, wind, sr, MAX_WIND_S)

    wet = profile.post(wet, x, controls, p, sr)
    wet = contaminate(wet, x, controls, p, profile, sr, md)

    if p.afterglow > 0.0:
        send = filters.to_sample_rate(controls.send, len(x))[:, None]
        glow = reverb(wet * send, sr, decay=p.afterglow, damping=AFTERGLOW_DAMPING)
        wet = wet + glow * (AFTERGLOW_LEVEL * p.afterglow)

    return wet, controls, md
