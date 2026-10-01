"""Everything after the reactor: DRIVE, COLLIMATOR, FALLOUT and peak safety."""

from __future__ import annotations

import numpy as np
from scipy.ndimage import maximum_filter1d
from scipy.signal import lfilter

from . import filters, rng, saturation
from .controls import Controls
from .params import Params
from .reverb import reverb

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

#: AFTERGLOW's voicing. Damped well down so a long tail stays warm rather than
#: hissing on top of the reaction.
AFTERGLOW_DAMPING = 0.45
AFTERGLOW_LEVEL = 0.9

LIMITER_LOOKAHEAD_S = 0.005
LIMITER_CEILING = 0.97
LIMITER_RELEASE_S = 0.050

#: How slowly the streaming level match tracks. Long on purpose: anything near
#: the speed of the programme turns a level match into a compressor.
LEVEL_MATCH_S = 1.5
#: Material below this is held rather than matched. An absolute threshold on
#: purpose: anything relative to the render's own average would be measuring
#: the future, which is the thing this function exists to avoid.
LEVEL_MATCH_GATE_DB = -60.0
#: How far the match is allowed to push, either way.
LEVEL_MATCH_RANGE_DB = 18.0

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


def drive(
    x: np.ndarray, p: Params, weight: float = 1.0, amount_ctrl=None, until: int = 0
) -> np.ndarray:
    """Saturation with the level change taken back out.

    DRIVE is a contamination control, not a gain control, so the RMS it adds is
    removed afterwards. The weight is the reaction's own appetite for it.

    Makeup is referenced to the knob position and measured over the material
    before any MELTDOWN, so the gesture cannot reach backwards and change the
    level before it fired. It also means the gesture is allowed to get louder,
    which it should.
    """
    amount = np.full(len(x), p.drive) if amount_ctrl is None else amount_ctrl
    if amount.max() <= 0.0:
        return x

    window = slice(0, until) if until > len(x) // 20 else slice(None)
    gain = 1.0 + 11.0 * (np.power(amount, 0.6) * weight)
    reference = 1.0 + 11.0 * (np.power(p.drive, 0.6) * weight)
    before = np.sqrt(np.mean(x[window] ** 2)) + 1e-12
    after = np.sqrt(np.mean(saturation.soft_clip(x[window], reference) ** 2)) + 1e-12
    y = saturation.oversampled(x * gain[:, None], saturation.soft_clip)
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
    # Below STEREO_BASS_MONO_HZ stays mono unconditionally so the low end
    # stays solid on a club system. Placement lives above it.
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
    y = _stereo_spread(x, sr, p.fallout * profile.stereo_weight)
    return _mid_wobble(y, sr, p.fallout * profile.wobble_weight, c)


def match_rms(y: np.ndarray, reference: np.ndarray, until: int | None = None) -> np.ndarray:
    """Hold the output at the input's level, measuring over a causal window.

    When MELTDOWN fires, the match is taken from the material before it: a gain
    derived from the whole buffer would let the loud part quietly duck
    everything that came before it, which a momentary gesture must not do.
    """
    window = slice(0, until) if until else slice(None)
    target = np.sqrt(np.mean(reference[window] ** 2)) + 1e-12
    current = np.sqrt(np.mean(y[window] ** 2)) + 1e-12
    return y * (target / current)


def running_match(y: np.ndarray, reference: np.ndarray, sr: int) -> np.ndarray:
    """The same level match, but only ever looking backwards.

    match_rms measures the whole render before deciding on a gain, which no
    plugin can do. This tracks both levels through a slow one-pole instead and
    divides one by the other as it goes.

    The time constant is the whole design. The job is to match average level,
    not to follow the input's envelope: track too quickly and the output is
    dragged into the shape of the dry signal, which flattens the dynamics the
    reaction just created. It is slow enough to settle on a figure and stay
    there.

    Quiet passages hold the last gain rather than being matched. Dividing two
    small numbers gives a large gain for no good reason, and resetting to unity
    instead of holding biases the whole render upwards.
    """
    coeff = float(np.exp(-1.0 / max(LEVEL_MATCH_S * sr, 1.0)))
    wet = filters.running_rms(y, sr, LEVEL_MATCH_S)
    dry = filters.running_rms(reference, sr, LEVEL_MATCH_S)

    ceiling = 10.0 ** (LEVEL_MATCH_RANGE_DB / 20.0)
    gain = np.clip(dry / np.maximum(wet, 1e-12), 1.0 / ceiling, ceiling)

    gate = 10.0 ** (LEVEL_MATCH_GATE_DB / 20.0)
    sounding = wet > gate
    if not sounding.all():
        # Hold through the gaps: carry the last gain forward rather than
        # snapping to unity.
        index = np.where(sounding, np.arange(len(gain)), 0)
        gain = gain[np.maximum.accumulate(index)]

    gain = lfilter([1.0 - coeff], [1.0, -coeff], gain, zi=np.array([coeff * gain[0]]))[0]
    return y * gain[:, None]


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



def lookahead_samples(sr: int) -> int:
    """The latency the plugin reports, whichever ceiling is switched on."""
    return max(int(LIMITER_LOOKAHEAD_S * sr), 1)


def peak_limit(x: np.ndarray, sr: int) -> np.ndarray:
    """Lookahead peak limiter.

    Gain is derived from a rolling forward-looking max and applied to audio
    delayed by the same window, so single-sample transients are caught rather
    than slipping through ahead of the envelope. Gain falls instantly but
    recovers over LIMITER_RELEASE_S: without a release it snapped back at over
    4000 dB per second, which is itself a distortion.
    """
    window = lookahead_samples(sr)
    magnitude = np.max(np.abs(x), axis=1)
    padded = np.concatenate([magnitude, np.zeros(window)])
    rolling = maximum_filter1d(padded, size=2 * window + 1, mode="nearest")[:len(magnitude)]

    target = np.minimum(1.0, LIMITER_CEILING / np.maximum(rolling, 1e-9))
    coeff = float(np.exp(-1.0 / max(LIMITER_RELEASE_S * sr, 1.0)))
    gain = np.empty_like(target)
    held = 1.0
    for i, want in enumerate(target):
        held = want if want < held else want + (held - want) * coeff
        gain[i] = held

    delayed = np.zeros_like(x)
    delayed[window:] = x[: len(x) - window]
    return delayed * gain[:, None]


def clip(x: np.ndarray, sr: int) -> np.ndarray:
    """Hard ceiling, oversampled so it does not alias.

    A clipper needs no lookahead, but it is padded to the limiter's delay
    anyway. Reporting a different latency makes the host re-sync the moment
    CLIP is switched, which is audible as a pop in the middle of a take. The
    delay is matched instead, so the toggle is seamless and the two ceilings
    stay sample-aligned against each other.

    Clipping at the higher rate removes the aliasing, but the filter that comes
    back down rings, and that ring lands on top of a signal already sitting
    flat on the ceiling: measured 1.08, which is over full scale and the one
    thing a ceiling may never do. The final clamp catches that overshoot. It
    reintroduces a little aliasing, but only on the few samples that overshot,
    which is a far better trade than handing the host a clipped output.
    """
    loud = saturation.oversampled(x, lambda u: saturation.hard_clip(u, LIMITER_CEILING))
    loud = saturation.hard_clip(loud, LIMITER_CEILING)

    window = lookahead_samples(sr)
    delayed = np.zeros_like(loud)
    delayed[window:] = loud[: len(loud) - window]
    return delayed


def process(
    wet: np.ndarray,
    dry: np.ndarray,
    sr: int,
    p: Params,
    profile,
    c: Controls,
    mix: float = 1.0,
    md=None,
    offline: bool = False,
) -> np.ndarray:
    """Run the output chain, blend with dry, then protect the peaks.

    Peak safety runs after the blend rather than on the wet path alone: the
    reaction decorrelates phase against the dry signal, so the mix can peak
    higher than either part on its own.

    `offline` falls back to the whole-render level match, which is kept only so
    the two can be rendered against each other. The plugin cannot use it.
    """
    until = int(p.meltdown_at * sr) if (md is not None and md.active) else 0
    y = drive(wet, p, profile.drive_weight, md.samples("drive") if md else None, until)
    y = collimate(y, sr, p)

    # Placement runs after the saturation, not before it. A hard-panned event
    # has one loud channel and one quiet one, and tanh compresses the loud one
    # harder, so driving a placed signal squeezes most of the placement back
    # out: measured 7.5dB of balance swing down to 3.6dB.
    y = y * np.stack(
        [filters.to_sample_rate(c.pan_gain[:, ch], len(y)) for ch in (0, 1)], axis=1
    )

    if p.afterglow > 0.0:
        send = filters.to_sample_rate(c.send, len(y))[:, None]
        glow = reverb(y * send, sr, decay=p.afterglow, damping=AFTERGLOW_DAMPING)
        y = y + glow * (AFTERGLOW_LEVEL * p.afterglow)

    y = fallout(y, sr, p, profile, c)
    y = voice(y, sr)
    if offline:
        y = match_rms(y, dry, until if until > sr // 4 else None)
    else:
        y = running_match(y, dry, sr)
    y = (1.0 - mix) * dry + mix * y
    return clip(y, sr) if p.clip else peak_limit(y, sr)
