"""Everything after the reactor: DRIVE, COLLIMATOR, FALLOUT and peak safety."""

from __future__ import annotations

import numpy as np
from scipy.ndimage import maximum_filter1d

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

#: Where the output aims its peaks, and how slowly it gets there. Slow enough
#: that it sets a level rather than following an envelope. The rise is slower
#: than the fall but both are long: an instant attack snaps the gain down on
#: every transient, which measured 11.7 dB/s of sustained movement and is a
#: compressor by any other name. The limiter catches what gets past this.
#:
#: Slowed from 0.6/1.2 when RADIATION's DECAY was unpinned. These were set
#: when its longest tail was 16 ms; it is now 1.28 s at this check's settings,
#: so a 1.2 s tracker was following the reaction's own ring. Swept across the
#: range the result is U-shaped — 7.65 dB of swing at 0.6/1.2, 4.80 at 2.0/4.0,
#: back to 6.65 at 6.0/12.0, where the tracker no longer settles inside the
#: render at all — while sustained movement, which is the thing a compressor
#: actually does, falls the whole way. 2.0/4.0 is the floor of the first and
#: well down the second, and beats the figures from before DECAY was fixed.
PEAK_TARGET = 0.89
PEAK_TRACK_S = 4.0
PEAK_ATTACK_S = 2.0


#: How slowly the streaming level match tracks. Long on purpose: anything near
#: the speed of the programme turns a level match into a compressor.
LEVEL_MATCH_S = 1.5
#: DRIVE's makeup tracks faster than the output match. It is correcting for a
#: curve rather than for a mix, so it can follow the material more closely
#: without reading as a compressor.
DRIVE_MATCH_S = 0.4
#: Material below this is held rather than matched. An absolute threshold on
#: purpose: anything relative to the render's own average would be measuring
#: the future, which is the thing this function exists to avoid.
LEVEL_MATCH_GATE_DB = -60.0
#: How far the match is allowed to push, either way. Wide enough that ENRICHMENT
#: can run to either end of its own 18 dB and still be levelled: a tighter
#: ceiling clamps exactly when the control is doing the most. Runaway gain in
#: the gaps is the gate's job, not the clamp's.
LEVEL_MATCH_RANGE_DB = 36.0

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
    x: np.ndarray, sr: int, p: Params, weight: float = 1.0, amount_ctrl=None
) -> np.ndarray:
    """Saturation with the level change taken back out.

    DRIVE is a contamination control, not a gain control, so the RMS it adds is
    removed afterwards. The weight is the reaction's own appetite for it.

    Makeup is referenced to the knob position rather than to the realised
    output, so a MELTDOWN is allowed to get louder, which it should. Both
    levels are tracked as the plugin would track them, so the gesture cannot
    reach backwards and change the level before it fired.
    """
    amount = np.full(len(x), p.drive) if amount_ctrl is None else amount_ctrl
    if amount.max() <= 0.0:
        return x

    gain = 1.0 + 11.0 * (np.power(amount, 0.6) * weight)
    reference = 1.0 + 11.0 * (np.power(p.drive, 0.6) * weight)

    before = filters.running_rms(x, sr, DRIVE_MATCH_S)
    after = filters.running_rms(saturation.soft_clip(x, reference), sr, DRIVE_MATCH_S)
    makeup = filters.matching_gain(before, after, sr, DRIVE_MATCH_S, LEVEL_MATCH_RANGE_DB)

    y = saturation.oversampled(x * gain[:, None], saturation.soft_clip)
    return y * makeup[:, None]


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


def unity_match(y: np.ndarray, reference: np.ndarray, sr: int) -> np.ndarray:
    """Aim the peaks just under the ceiling, in both directions.

    This used to match the output's RMS to the input's, which fought the
    instrument. The ladder makes energy — that is what resonance is — so a
    level match reads the resonance as too loud and removes it. Measured
    against a peak-normalised reference it was 3 dB down overall and 6 dB down
    through the midrange, which is where the squelch lives.

    Slow enough to be a level and not an envelope. It does lift quiet material,
    unlike the peak control it replaces: a reaction that rings less is not
    meant to be quieter, it is meant to be a different sound at the same level.
    """
    magnitude = np.max(np.abs(y), axis=1)

    attack = float(np.exp(-1.0 / max(PEAK_ATTACK_S * sr, 1.0)))
    release = float(np.exp(-1.0 / max(PEAK_TRACK_S * sr, 1.0)))
    level = np.empty(len(magnitude))
    held = 0.0
    for i, value in enumerate(magnitude):
        coeff = attack if value > held else release
        held = value + (held - value) * coeff
        level[i] = held

    gain = filters.matching_gain(
        np.full(len(y), PEAK_TARGET), level, sr, PEAK_TRACK_S,
        LEVEL_MATCH_RANGE_DB, LEVEL_MATCH_GATE_DB,
    )
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
    y = drive(wet, sr, p, profile.drive_weight, md.samples("drive") if md else None)
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
    y = match_rms(y, dry, until if until > sr // 4 else None) if offline else unity_match(y, dry, sr)
    y = (1.0 - mix) * dry + mix * y
    return clip(y, sr) if p.clip else peak_limit(y, sr)
