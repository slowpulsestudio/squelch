"""The reactor core.

Turns scheduled events into control-rate filter movement, then runs the input
through a resonant ladder driven by it. This is where DECAY, SPREAD, EXPOSURE,
VOLATILITY, HALF-LIFE, TOXICITY and CONTAINMENT act.
"""

from __future__ import annotations

from typing import Callable

import numpy as np
from scipy.signal import lfilter

from . import filters, rng, saturation
from .controls import Controls
from .meltdown import Meltdown
from .params import Params
from .reactions import PROFILES, ReactionProfile, contaminate
from .scheduler import MAX_SUB_EVENTS, Event, schedule

#: Envelope attack. Long enough that the ramp spans many control blocks, so the
#: envelope glides rather than stepping into a click on short, high-Q events.
ATTACK_S = 0.005

#: Always-on smoothing of the control signals, short enough to leave fast
#: squelch movement intact but long enough to take the staircase off each step.
ANTI_STEP_S = 0.015

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

#: How hard the cutoff and feedback envelopes are rounded off at the instant an
#: event retriggers. This is de-stepping, not sliding: it has to be far shorter
#: than the sweep's own decay or it flattens the thing it is smoothing. At
#: SLIDE_S it cost RADIATION 0.8 of its 5.2 octaves and left ALIEN with a
#: quarter of the movement its reference render has.
GLIDE_S = 0.002

#: An accent opens the sweep further and pushes the feedback nearer to
#: oscillation. It changes the voice of the note, not its volume.
ACCENT_SWEEP_OCT = 0.8
ACCENT_FEEDBACK = 0.25

#: How far the feedback falls back between events, as a fraction of its peak,
#: across HALF-LIFE's travel. This is the length of the ring: at the bottom the
#: filter stops dead between events, at the top it never quite stops.
CARRY_LO = 0.30
CARRY_HI = 0.95

#: How far VOLATILITY bends an event envelope away from a plain exponential.
SHAPE_RANGE = 0.8

#: How far IONIZE throws an event's resonant peak from where the sweep left it.
IONIZE_SPECTRAL = 0.55

#: Placement glides over this, so an event landing hard left does not click.
PAN_SMOOTH_S = 0.004

#: How far IONIZE opens the gaps between events so each one stands alone.
IONIZE_ARTICULATION = 0.8

#: AFTERGLOW's voicing. Damped well down so a long tail stays warm rather than
#: hissing on top of the reaction.
AFTERGLOW_DAMPING = 0.45
AFTERGLOW_LEVEL = 0.9

#: SLUDGE's body/snap envelope followers. Fixed architecture constants, not
#: knobs: f_q is the slow body corner, f_r the fast snap corner that DECAY
#: slows down from. maths.md requires f_r > f_q; this stays true across the
#: whole DECAY range by construction.
SLUDGE_F_Q_HZ = 5.0
SLUDGE_F_R_LO_HZ = 45.0
SLUDGE_F_R_HI_HZ = 12.0

#: HALF-LIFE's long reactor memory, in seconds.
SLUDGE_TAU_M_LO_S = 0.3
SLUDGE_TAU_M_HI_S = 5.0

#: Asymmetry of SLUDGE's saturator. Fixed: TOXICITY drives the drive amount,
#: not the asymmetry itself, which is what gives it even-order content CHEMICAL's
#: symmetric tanh cannot produce.
SLUDGE_GAMMA = 0.6

#: Subharmonic mix weights for the /2 and /4 components. Fixed so the
#: generator is always present rather than being a knob some setting zeroes.
SLUDGE_W1 = 0.65
SLUDGE_W2 = 0.45


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
    open_rest: bool = False,
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

    if open_rest:
        # The filter idles open and only comes down as REACTIVITY asks it to,
        # so at the bottom of the control the plugin passes the source through
        # instead of lowpassing it at under a kilohertz.
        open_oct = float(np.log2(sr * 0.45))
        resting_ctrl = resting_ctrl + (open_oct - resting_ctrl) * (1.0 - p.reactivity)

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

        # README.md lists stereo position among the things VOLATILITY varies.
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


def envelopes(
    events: list[Event], n: int, sr: int, p: Params, profile: ReactionProfile, md
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Cutoff, feedback and loop drive, one sample at a time.

    The cutoff is the 303's: it jumps to its peak the instant an event fires
    and falls back exponentially,

        fc(t) = base * 2 ** (sweep * exp(-t / tau))

    with the sweep in octaves so it stays musical wherever base sits. An
    accented step opens the sweep further and pushes the feedback nearer to
    oscillation, so accents change the voice rather than the volume.

    Nothing here touches amplitude. Multiplying the source by an event envelope
    replaces its dynamics with a uniform stream, which is what flattened the
    first build: 6.2dB of range in the source came out as 2.8dB.
    """
    spread = md.at("spread", 0)
    toxicity = md.at("toxicity", 0)
    exposure = md.at("exposure", 0)
    containment = md.at("containment", 0)

    sweep = profile.sweep_octaves * spread * (1.0 - 0.7 * containment)
    tau = profile.tau_lo_s + (profile.tau_hi_s - profile.tau_lo_s) * p.decay
    drive_amount = profile.drive_lo + (profile.drive_hi - profile.drive_lo) * toxicity

    # HALF-LIFE sets how far the feedback falls between events, which is what
    # decides how long the filter keeps ringing. Letting it drop all the way
    # back kills the tail: measured 8.7dB of decay per 100ms against 5.2dB when
    # the floor is held at just over half the peak. The ring is the reverb —
    # there is no reverb in this path at all.
    peak = profile.feedback_lo + (profile.feedback_hi - profile.feedback_lo) * exposure
    carry = CARRY_LO + (CARRY_HI - CARRY_LO) * p.half_life
    floor = peak * carry

    ionize = p.ionize_amount if p.ionize else 0.0
    base_oct = float(np.log2(profile.base_hz))

    cutoff = np.full(n, profile.base_hz)
    feedback = np.full(n, floor)
    drive = np.full(n, drive_amount)

    slide = max(int(SLIDE_S * sr), 1)
    held = base_oct
    starts = [max(int(e.start), 0) for e in events]
    for i, event in enumerate(events):
        start = starts[i]
        if start >= n:
            continue
        end = min(starts[i + 1], n) if i + 1 < len(starts) else n
        if end <= start:
            continue

        # The reaction's own accent pattern rides on the grid step, so each one
        # keeps its rhythmic signature whatever the seed does, and the
        # scheduler's own accents add to it. Indexing by the event's own index
        # instead would step through the pattern five at a time, because the
        # scheduler numbers sub-events inside each step, which scrambles it.
        pattern = profile.accents[(event.index // MAX_SUB_EVENTS) % len(profile.accents)]
        accent = min(1.0, pattern + (0.6 if event.accent else 0.0))

        # A step that fans out into sub-events marks the extras as weaker. The
        # sweep has to honour that: retriggering every one of them to full
        # depth means the cutoff is pulled back to the top before it has fallen
        # anywhere, and a filter that never falls does not squelch.
        weight = event.intensity

        tau_event = max(tau * event.decay_scale, 0.005)
        shape = np.exp(-np.arange(end - start) / (tau_event * sr))

        # IONIZE throws each event into its own register, so they pop apart
        # instead of every sweep starting from the same place.
        register = base_oct + ionize * IONIZE_SPECTRAL * rng.ubipolar(p.seed, 22, event.index)

        depth = (sweep + ACCENT_SWEEP_OCT * accent * spread) * weight
        curve = register + depth * shape
        if event.slide:
            # A slid note glides in from wherever the last one finished rather
            # than jumping. This is the 303's portamento, and it is the reason
            # the glide that de-steps the envelope has to stay separate from
            # it: one is a musical gesture, the other is an anti-click.
            #
            # Capped at half the note. SLIDE_S is a fixed time, so against a
            # fast grid it would otherwise cover most of the step and the
            # sweep would spend its life ramping instead of falling.
            ramp = np.linspace(0.0, 1.0, min(slide, max(len(curve) // 2, 1)))
            curve[: len(ramp)] = held * (1.0 - ramp) + curve[: len(ramp)] * ramp
        held = float(curve[-1])

        cutoff[start:end] = np.power(2.0, curve)
        feedback[start:end] = (
            floor
            + (peak - floor) * shape * weight
            + ACCENT_FEEDBACK * accent * exposure * weight
        )

    # The ladder rings hard, so a step in its cutoff is a click.
    glide = max(int(GLIDE_S * sr), 1)
    cutoff = np.maximum(_glide(cutoff, glide), 20.0)
    feedback = np.clip(_glide(feedback, glide), 0.0, 3.95)
    return cutoff, feedback, drive


def _glide(ctrl: np.ndarray, samples: int) -> np.ndarray:
    coeff = float(np.exp(-1.0 / max(samples, 1)))
    return lfilter([1.0 - coeff], [1.0, -coeff], ctrl, zi=np.array([coeff * ctrl[0]]))[0]


def _ladder_engine(
    x: np.ndarray,
    cutoff: np.ndarray,
    feedback: np.ndarray,
    drive: np.ndarray,
    sr: int,
    profile: ReactionProfile,
    events: list[Event],
    controls: Controls,
    p: Params,
    md: Meltdown,
) -> np.ndarray:
    """CHEMICAL's engine: the nonlinear resonant feedback ladder.

    maths.md's audit table names this mechanism CHEMICAL's and forbids the
    other four from using it as their own voice. It is still every other
    reaction's placeholder engine until each gets the one maths.md specifies
    for it (see plan.md); until then they measure as one effect, which is the
    open defect `check_reactions_are_distinct` exists to catch.
    """
    return filters.ladder(x, cutoff, feedback, drive, sr, tap=profile.tap)


def _one_pole_hz(x: np.ndarray, f_hz: float, sr: int) -> np.ndarray:
    """Constant-coefficient one-pole lowpass, maths.md's g = 1 - e^(-2*pi*f/fs)."""
    g = 1.0 - np.exp(-2.0 * np.pi * f_hz / sr)
    return lfilter([g], [1.0, -(1.0 - g)], x, axis=0)


def _sludge_engine(
    x: np.ndarray,
    cutoff: np.ndarray,
    feedback: np.ndarray,
    drive: np.ndarray,
    sr: int,
    profile: ReactionProfile,
    events: list[Event],
    controls: Controls,
    p: Params,
    md: Meltdown,
) -> np.ndarray:
    """SLUDGE's engine: nonlinear subharmonic generation plus long memory.

    maths.md's SLUDGE section. No event sweep and no ladder: the reaction
    tracks its own slow/fast envelope followers off the input, generates the
    /2 and /4 subharmonics from its own reactor frequency (no pitch tracking),
    and runs the asymmetric-saturated result through a memory-driven lowpass.
    """
    n = x.shape[0]
    e = np.abs(x).max(axis=1)

    q = _one_pole_hz(e, SLUDGE_F_Q_HZ, sr)
    decay_time = profile.decay_lo_s + (profile.decay_hi_s - profile.decay_lo_s) * p.decay
    f_r = SLUDGE_F_R_LO_HZ + (SLUDGE_F_R_HI_HZ - SLUDGE_F_R_LO_HZ) * (
        decay_time / profile.decay_hi_s
    )
    r = _one_pole_hz(q, f_r, sr)
    s_snap = q - r

    tau_m = SLUDGE_TAU_M_LO_S + (SLUDGE_TAU_M_HI_S - SLUDGE_TAU_M_LO_S) * p.half_life
    a_m = float(np.exp(-1.0 / max(tau_m * sr, 1.0)))
    m = lfilter([1.0 - a_m], [1.0, -a_m], q)
    m_b = np.tanh(m)

    # Slow cutoff movement: the molasses lurch, bounded to this reaction's own
    # cutoff territory so SPREAD at zero collapses it to the resting centre.
    span_oct = np.log2(profile.cutoff_hi_hz / profile.cutoff_lo_hz)
    f_base_oct = np.log2(profile.cutoff_lo_hz) + span_oct * 0.5
    delta_f = span_oct * 0.5 * p.spread
    f_c = np.power(2.0, f_base_oct + delta_f * m_b)

    # The fast state briefly recoils the cutoff before it settles, per the
    # correction in maths.md's Viscous snapback section: q - r already carries
    # its own rise/decay from f_r > f_q, with no separate state needed.
    delta_snap = 0.4 * p.reactivity
    f_effective = np.clip(f_c * np.power(2.0, delta_snap * s_snap), 20.0, sr * 0.45)

    beta_m = 0.3 + 0.9 * p.exposure
    beta_h = 0.2 + 0.6 * p.exposure

    # Subharmonics derived from SLUDGE's own reactor frequency, not the input.
    delta_h = 0.5 * p.spread
    f_h = profile.base_hz * np.power(2.0, delta_h * m_b)
    theta_raw = np.cumsum(2.0 * np.pi * f_h / sr)
    h1 = np.sin(np.mod(theta_raw, 4.0 * np.pi) / 2.0)
    h2 = np.sin(np.mod(theta_raw, 8.0 * np.pi) / 4.0)
    a_h = 0.5 + (0.5 + 1.0 * p.exposure) * np.tanh(q)
    h = a_h * (SLUDGE_W1 * h1 + SLUDGE_W2 * h2)

    delta = 1.0 + 3.0 * p.toxicity
    z = x + beta_m * m[:, None] + beta_h * h[:, None]

    def curve(u: np.ndarray) -> np.ndarray:
        return np.where(u >= 0.0, np.tanh(delta * u), SLUDGE_GAMMA * np.tanh(delta * u))

    sat = saturation.oversampled(z, curve)

    # The one lowpass stage whose cutoff genuinely varies every sample: the
    # memory-driven movement is slow by construction (HALF-LIFE's seconds), so
    # a Python loop here costs nothing a smoothed control-rate approximation
    # would have saved, and stays exactly what maths.md specifies.
    s = np.zeros_like(sat)
    state = np.zeros(sat.shape[1])
    g_c = 1.0 - np.exp(-2.0 * np.pi * f_effective / sr)
    for i in range(n):
        state += g_c[i] * (sat[i] - state)
        s[i] = state

    mu_h = 0.3 + 0.5 * p.reactivity
    return (1.0 - mu_h) * s + mu_h * h[:, None]


#: ALIEN's deterministic per-event pitch hash stream.
ALIEN_PITCH_STREAM = 401

#: Total pitch jump range each event can land on, in semitones either side of
#: base_hz. Wide enough that consecutive events read as discrete jumps, not a
#: wobble around one note.
ALIEN_D_MAX_SEMITONES = 36.0

#: How far TOXICITY can drive the FM modulation index, and how fast it
#: collapses back toward the plain carrier within one event.
ALIEN_BETA0_MAX = 6.0
ALIEN_TAU_BETA_S = 0.03

#: AM burst depth at full EXPOSURE, and the modulator's own rate range.
ALIEN_MU_MAX = 1.5
ALIEN_AM_HZ_LO = 30.0
ALIEN_AM_HZ_HI = 200.0

#: The short high-frequency accent riding above the carrier, and how fast it
#: collapses: it is an accent, not a second voice.
ALIEN_CHIRP_SEMITONES = 12.0
ALIEN_TAU_Z_S = 0.01
ALIEN_ATTACK_S = 0.003

#: Input energy's share of an event's amplitude: an event fires from silence,
#: but a loud input drives it harder, per maths.md's A_i = A_event + g_x E_x.
ALIEN_INPUT_GAIN = 0.6


def _alien_engine(
    x: np.ndarray,
    cutoff: np.ndarray,
    feedback: np.ndarray,
    drive: np.ndarray,
    sr: int,
    profile: ReactionProfile,
    events: list[Event],
    controls: Controls,
    p: Params,
    md,
) -> np.ndarray:
    """ALIEN: an actual event-gated source oscillator, not a filtered input.

    Each scheduled event gets its own deterministic pitch (hashed, not derived
    from the input), an exponential pitch sweep, FM with a decaying modulation
    index, a separate AM burst, an attack/release envelope and a short
    high-frequency chirp accent. With no events and a silent input this
    produces nothing: the oscillator is event-gated, per maths.md and
    dsp-testing.md Test 10, not a continuously running synth.
    """
    n, channels = x.shape
    out = np.zeros((n, channels))
    if not events:
        return out

    e_x = np.abs(x).max(axis=1)

    sweep_semitones = 12.0 * profile.sweep_octaves * p.spread
    decay_time = profile.decay_lo_s + (profile.decay_hi_s - profile.decay_lo_s) * p.decay
    tau_f = max(decay_time * 0.5, 0.01)
    tau_r = max(decay_time, 0.02)
    beta0 = ALIEN_BETA0_MAX * p.toxicity
    mu = ALIEN_MU_MAX * p.exposure
    f_a = ALIEN_AM_HZ_LO + (ALIEN_AM_HZ_HI - ALIEN_AM_HZ_LO) * p.exposure

    dur = min(int(sr * max(8.0 * tau_r, 0.05)), n)
    if dur <= 0:
        return out
    t = np.arange(dur) / sr

    for event in events:
        start = max(int(event.start), 0)
        if start >= n:
            continue
        length = min(dur, n - start)
        tt = t[:length]

        r_i = 2.0 * rng.urand(p.seed, ALIEN_PITCH_STREAM, event.index) - 1.0
        d_i = ALIEN_D_MAX_SEMITONES * r_i
        f0 = profile.base_hz * np.power(2.0, d_i / 12.0)
        f_c = f0 * np.power(2.0, sweep_semitones * (1.0 - np.exp(-tt / tau_f)) / 12.0)
        f_c = np.clip(f_c, 20.0, sr * 0.45)

        beta = beta0 * np.exp(-tt / ALIEN_TAU_BETA_S)
        theta_m = np.cumsum(2.0 * np.pi * f_c / sr)
        m = np.sin(theta_m)
        theta_c = np.cumsum(2.0 * np.pi * f_c / sr)
        z_fm = np.sin(theta_c + beta * m)

        m_a = 0.5 * (1.0 + np.sin(2.0 * np.pi * f_a * tt))
        a = 1.0 + mu * m_a
        z_am = a * z_fm

        e_a = 1.0 - np.exp(-tt / ALIEN_ATTACK_S)
        e_r = np.exp(-tt / tau_r)
        env = e_a * e_r

        f_z = np.clip(f_c * np.power(2.0, ALIEN_CHIRP_SEMITONES / 12.0), 20.0, sr * 0.45)
        theta_z = np.cumsum(2.0 * np.pi * f_z / sr)
        a_z = 0.6 if event.accent else 0.3
        e_z = a_z * np.exp(-tt / ALIEN_TAU_Z_S)
        z_z = e_z * np.sin(theta_z)

        a_event = 1.3 if event.accent else 0.8
        a_i = a_event + ALIEN_INPUT_GAIN * e_x[start]

        burst = a_i * env * z_am + z_z
        pan = 0.5 * (1.0 + event.pan)
        out[start : start + length, 0] += burst * (1.0 - pan)
        out[start : start + length, 1] += burst * pan

    return out


#: One engine per REACTION. Replacing an entry is how a reaction gets its own
#: mechanism instead of CHEMICAL's ladder.
ENGINES: dict[str, Callable] = {
    "CHEMICAL": _ladder_engine,
    "RADIATION": _ladder_engine,
    "FISSION": _ladder_engine,
    "SLUDGE": _sludge_engine,
    "ALIEN": _alien_engine,
}


def process(
    x: np.ndarray, sr: int, p: Params, bpm: float, open_rest: bool = False
) -> tuple[np.ndarray, Controls]:
    profile = PROFILES[p.reaction]
    md = Meltdown(p, len(x), sr)
    events = schedule(x, sr, p, bpm, sub_event_bias=profile.sub_event_bias, md=md)
    controls = build_controls(events, len(x), sr, p, profile, md, open_rest)

    cutoff, feedback, drive = envelopes(events, len(x), sr, p, profile, md)
    engine = ENGINES[p.reaction]
    wet = engine(x, cutoff, feedback, drive, sr, profile, events, controls, p, md)

    wet = contaminate(wet, x, controls, p, profile, sr, md)
    return wet, controls, md
