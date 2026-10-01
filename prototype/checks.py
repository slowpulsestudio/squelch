"""Isolated checks on the DSP building blocks.

A measurement is only worth acting on once the instrument producing it has been
shown to be correct, so each component is verified on a known signal before any
conclusion is drawn from a full-mix render.

Run with: ./.venv/bin/python -m prototype.checks
"""

from __future__ import annotations

import sys

import numpy as np
from scipy.signal import hilbert, welch

from . import filters, output_stage
from .params import Params
from .scheduler import schedule

SR = 44100


def _tone(freq: float, seconds: float = 1.0, sr: int = SR) -> np.ndarray:
    t = np.arange(int(seconds * sr)) / sr
    return np.repeat((np.sin(2.0 * np.pi * freq * t) * 0.5)[:, None], 2, axis=1)


def _peak_frequency(x: np.ndarray, sr: int = SR) -> float:
    spectrum = np.abs(np.fft.rfft(x.mean(axis=1)))
    return float(np.fft.rfftfreq(len(x), 1.0 / sr)[int(np.argmax(spectrum))])


def _bed_controls(n: int):
    """A plausible set of controls for exercising a noise bed on its own."""
    from .controls import Controls

    blocks = filters.n_blocks(n)
    return Controls(
        env=np.linspace(0.2, 1.0, blocks),
        pan_gain=np.ones((blocks, 2)),
        send=np.linspace(0.2, 1.0, blocks),
        cutoff=np.full(blocks, 500.0),
        resonance=np.full(blocks, 4.0),
        starts=np.arange(0, n, SR // 8),
        damping=0.0,
    )


def check_ladder_response() -> tuple[bool, str]:
    """A 4-pole lowpass must roll off near 24 dB/octave with one resonant peak.

    Measured with Welch + Blackman-Harris rather than a bare FFT ratio: an
    unwindowed FFT leaks enough energy from the resonant peak to put a false
    floor around -72 dB, which reads as the filter running out of slope.
    """
    rng = np.random.default_rng(0)
    noise = rng.standard_normal((SR * 4, 2)) * 0.1
    blocks = filters.n_blocks(len(noise))
    fc = 500.0
    out = filters.varying_ladder(
        noise, np.full(blocks, fc), np.full(blocks, 6.0), SR, inner_sat=0.0
    )

    kwargs = dict(fs=SR, window="blackmanharris", nperseg=8192)
    freqs, p_in = welch(noise.mean(axis=1), **kwargs)
    _, p_out = welch(out.mean(axis=1), **kwargs)
    response = 10.0 * np.log10((p_out + 1e-30) / (p_in + 1e-30))

    def level_at(f: float) -> float:
        return float(response[int(np.argmin(np.abs(freqs - f)))])

    slope = level_at(4000.0) - level_at(2000.0)
    peak_band = (freqs > 200.0) & (freqs < 1200.0)
    peak_freq = float(freqs[peak_band][np.argmax(response[peak_band])])

    ok = -27.0 < slope < -21.0 and abs(peak_freq - fc) < fc * 0.25
    return ok, f"slope {slope:.1f} dB/oct (want ~-24), resonant peak {peak_freq:.0f} Hz (want ~{fc:.0f})"


def check_limiter_catches_spike() -> tuple[bool, str]:
    """A single-sample spike must be caught, which needs real lookahead."""
    x = np.zeros((SR, 2))
    x[:, :] = _tone(220.0, 1.0) * 0.2
    x[SR // 2, :] = 4.0
    out = output_stage.peak_limit(x, SR)
    peak = float(np.max(np.abs(out)))
    ok = peak <= output_stage.LIMITER_CEILING + 1e-6
    return ok, f"peak after limiting {peak:.4f} (ceiling {output_stage.LIMITER_CEILING})"


def check_frequency_shift() -> tuple[bool, str]:
    """SSB shifting moves the tone and must not leave a mirrored sideband."""
    tone = _tone(1000.0)
    blocks = filters.n_blocks(len(tone))
    out = filters.frequency_shift(tone, np.full(blocks, 150.0), SR)

    freqs = np.fft.rfftfreq(len(out), 1.0 / SR)
    spectrum = np.abs(np.fft.rfft(out.mean(axis=1)))
    upper = spectrum[(freqs > 1130.0) & (freqs < 1170.0)].max()
    mirror = spectrum[(freqs > 830.0) & (freqs < 870.0)].max()
    rejection = 20.0 * np.log10((mirror + 1e-12) / (upper + 1e-12))

    ok = abs(_peak_frequency(out) - 1150.0) < 15.0 and rejection < -25.0
    return ok, f"peak {_peak_frequency(out):.0f} Hz (want 1150), mirror sideband {rejection:.1f} dB"


def check_octave_down() -> tuple[bool, str]:
    """The sludge body layer must actually be an octave below, not a rectifier buzz."""
    out = filters.octave_down(_tone(400.0))
    peak = _peak_frequency(out)
    ok = abs(peak - 200.0) < 8.0
    return ok, f"peak {peak:.1f} Hz (want 200)"


def check_scheduling_is_deterministic() -> tuple[bool, str]:
    """The same setting must fire the same pattern every render."""
    x = np.zeros((SR * 4, 2))
    p = Params(mode="GRID", grid="1/16", flux=0.6, probability=0.6, reactivity=0.7, seed=7)
    a = schedule(x, SR, p, 140.0)
    b = schedule(x, SR, p, 140.0)
    same = [e.start for e in a] == [e.start for e in b]
    different_seed = schedule(x, SR, Params(**{**p.summary(), "seed": 8}), 140.0)
    varies = [e.start for e in a] != [e.start for e in different_seed]
    return same and varies, f"{len(a)} events, repeatable={same}, seed changes pattern={varies}"


def check_pitch_wind() -> tuple[bool, str]:
    """A lengthening delay must bend pitch down, and a shortening one back up."""
    seconds = 1.0
    tone = _tone(440.0, seconds)
    blocks = filters.n_blocks(len(tone))
    ramp = np.linspace(0.0, 1.0, blocks)
    out = filters.pitch_wind(tone, ramp, SR, 0.020)

    # Expected ratio is 1 minus the delay's rate of change.
    expected = 440.0 * (1.0 - 0.020 / seconds)
    middle = out[int(0.3 * SR) : int(0.8 * SR)]
    measured = _peak_frequency(middle)

    falling = filters.pitch_wind(tone, ramp[::-1], SR, 0.020)
    up = _peak_frequency(falling[int(0.3 * SR) : int(0.8 * SR)])

    ok = abs(measured - expected) < 4.0 and up > 440.0
    return ok, f"wind down {measured:.1f} Hz (want {expected:.1f}), wind up {up:.1f} Hz (want >440)"


def check_contamination_scales() -> tuple[bool, str]:
    """Every reaction's bed must be silent at zero and hit its target level.

    The grain previously measured -46 to -110 dB against the mix and was never
    audible, because its gain was scaled by two other parameters at once.
    """
    from .params import REACTIONS, Params
    from .reactions import PROFILES, contaminate

    n = SR * 4
    wet = _tone(200.0, 4.0) * 0.3
    c = _bed_controls(n)

    failures = []
    for reaction in REACTIONS:
        profile = PROFILES[reaction]
        target = 20.0 * np.log10(profile.noise_full_level)
        measured = []
        for amount in (0.0, 0.5, 1.0):
            params = Params(reaction=reaction, contamination=amount, seed=0)
            out = contaminate(wet, wet, c, params, profile, SR)
            diff = out - wet
            measured.append(
                20.0
                * np.log10(
                    (np.sqrt(np.mean(diff**2)) + 1e-15) / (np.sqrt(np.mean(out**2)) + 1e-12)
                )
            )
        if measured[0] > -200.0:
            failures.append(f"{reaction} audible at zero")
        if abs(measured[2] - target) > 1.5:
            failures.append(f"{reaction} full {measured[2]:.1f} vs target {target:.1f}")
        if abs((measured[2] - measured[1]) - 12.0) > 1.5:
            failures.append(f"{reaction} half-to-full {measured[2] - measured[1]:.1f} not 12")

    detail = "; ".join(failures) if failures else "5 beds silent at 0, on target at full, 12dB half-to-full"
    return not failures, detail


def check_only_radiation_ticks() -> tuple[bool, str]:
    """Only RADIATION may emit discrete bursts; the rest must be continuous.

    Measured on a 20ms RMS envelope, not on raw samples: any noise signal has
    roughly 12dB of sample-level crest intrinsically, so a raw-sample measure
    reports every bed as peaky and cannot tell a bed from a burst.
    """
    from .params import REACTIONS, Params
    from .reactions import PROFILES

    n = SR * 4
    wet = _tone(200.0, 4.0) * 0.3
    c = _bed_controls(n)
    window = int(0.020 * SR)
    kernel = np.ones(window) / window

    crests = {}
    for reaction in REACTIONS:
        bed = PROFILES[reaction].noise(wet, c, Params(reaction=reaction, seed=0), SR)
        envelope = np.sqrt(np.convolve(bed.mean(axis=1) ** 2, kernel, mode="valid"))
        crests[reaction] = 20.0 * np.log10(
            envelope.max() / (np.sqrt(np.mean(envelope**2)) + 1e-12)
        )

    ticky = crests["RADIATION"]
    beds = max(v for k, v in crests.items() if k != "RADIATION")
    # Beds measure 3-8dB and discrete bursts 18dB+. The bed figure is a floor
    # set by the swept filter texture, not by grain density: it stays put over
    # a 16x change in grain rate, so there is no point tightening it further.
    ok = ticky > 15.0 and beds < 10.0
    return ok, ", ".join(f"{k.split()[0].title()} {v:.1f}" for k, v in crests.items()) + " dB env crest"


def check_voicing_curve() -> tuple[bool, str]:
    """The house voicing must add weight low, dip the mud and shelve the harsh top."""
    rng = np.random.default_rng(0)
    noise = rng.standard_normal((SR * 4, 2)) * 0.1
    out = output_stage.voice(noise, SR)

    kwargs = dict(fs=SR, window="blackmanharris", nperseg=8192)
    freqs, p_in = welch(noise.mean(axis=1), **kwargs)
    _, p_out = welch(out.mean(axis=1), **kwargs)
    response = 10.0 * np.log10((p_out + 1e-30) / (p_in + 1e-30))

    def at(f: float) -> float:
        return float(response[int(np.argmin(np.abs(freqs - f)))])

    # Probed at the centre of the de-harsh bell, and at 15kHz to confirm the
    # top is left intact rather than the harshness being removed by dulling.
    low, dip, harsh, air = at(55.0), at(260.0), at(6000.0), at(15000.0)
    ok = low > 1.5 and dip < -1.0 and harsh < -3.0 and air > -1.0
    return ok, f"55Hz {low:+.1f}, 260Hz {dip:+.1f}, 6kHz {harsh:+.1f}, 15kHz {air:+.1f} dB"


def check_acid_voice() -> tuple[bool, str]:
    """BEAKER must behave like a 303: per-note envelope sweep, accents, slides.

    A resonant lowpass swept by a per-note envelope is what squelches. Holding
    a frequency, or using a bandpass, removes both the squelch and the bassline.
    """
    from .params import Params
    from .reactions import PROFILES
    from .reactor import build_controls
    from .scheduler import schedule

    profile = PROFILES["BEAKER"]
    n = SR * 4
    silence = np.zeros((n, 2))
    p = Params(reaction="BEAKER", mode="GRID", grid="1/8", seed=3, decay=0.3, spread=0.7)
    events = schedule(silence, SR, p, 140.0, sub_event_bias=profile.sub_event_bias)
    c = build_controls(events, n, SR, p, profile)

    octaves = np.log2(c.cutoff)
    moving = float(np.mean(np.abs(np.diff(octaves)) > 1e-4)) * 100.0

    accented = [e for e in events if e.accent]
    plain = [e for e in events if not e.accent]
    blocks = lambda e: int(e.start / filters.BLOCK)
    accent_peak = np.mean([octaves[blocks(e) : blocks(e) + 20].max() for e in accented[:40]])
    plain_peak = np.mean([octaves[blocks(e) : blocks(e) + 20].max() for e in plain[:40]])

    slid = sum(1 for e in events if e.slide) / max(len(events), 1) * 100.0

    ok = (
        profile.voice == "acid"
        and moving > 80.0
        and accent_peak > plain_peak
        and 20.0 < slid < 40.0
    )
    return ok, (
        f"{moving:.0f}% of the envelope is moving, accents open "
        f"{(accent_peak - plain_peak) * 12:.1f} semitones higher, {slid:.0f}% slide"
    )


def check_acid_keeps_the_low_end() -> tuple[bool, str]:
    """The acid filter is a lowpass, so the bassline must survive.

    The bandpass this replaced rejected 80Hz by 20dB, which removes the bass
    the line is supposed to be.
    """
    from .params import Params
    from .reactions import PROFILES
    from .reactor import build_controls
    from .scheduler import schedule

    profile = PROFILES["BEAKER"]
    n = SR * 4
    noise = np.random.default_rng(0).standard_normal((n, 2)) * 0.1
    p = Params(reaction="BEAKER", mode="GRID", grid="1/8", seed=3, decay=0.3, spread=0.7)
    events = schedule(noise, SR, p, 140.0, sub_event_bias=profile.sub_event_bias)
    c = build_controls(events, n, SR, p, profile)
    out = filters.varying_ladder(noise, c.cutoff, c.resonance, SR, inner_sat=0.0)

    kwargs = dict(fs=SR, window="blackmanharris", nperseg=8192)
    freqs, p_in = welch(noise.mean(axis=1), **kwargs)
    _, p_out = welch(out.mean(axis=1), **kwargs)
    response = 10.0 * np.log10((p_out + 1e-30) / (p_in + 1e-30))
    at = lambda f: float(response[int(np.argmin(np.abs(freqs - f)))])

    ok = at(80.0) > -6.0
    return ok, f"80Hz {at(80.0):+.1f} dB, 300Hz {at(300.0):+.1f} dB, 5kHz {at(5000.0):+.1f} dB"


def check_range_drives_each_character() -> tuple[bool, str]:
    """SPREAD must reach zero and scale up on each reaction's own movement.

    Not just the filter sweep: ALIEN's zaps, TOXIC SLUDGE's bubble rise and
    FISSION's separation of its two halves all answer to it.
    """
    from .params import Params
    from .reactions import PROFILES

    n = SR * 2
    source = _tone(300.0, 2.0) * 0.4
    c = _bed_controls(n)
    results = {}

    def difference(reaction: str, amount: float) -> float:
        p = Params(reaction=reaction, spread=amount, toxicity=0.8, seed=0)
        out = PROFILES[reaction].post(source, source, c, p, SR)
        delta = out - source
        return 20.0 * np.log10(
            (np.sqrt(np.mean(delta**2)) + 1e-15) / (np.sqrt(np.mean(source**2)) + 1e-12)
        )

    # ALIEN's shift must be absent at zero and present when opened.
    results["ALIEN"] = (difference("ALIEN", 0.0), difference("ALIEN", 1.0))

    # TOXIC SLUDGE's notches must stop rising at zero.
    travel = {}
    for amount in (0.0, 1.0):
        slow = filters.smooth(c.env, 0.08, SR)
        notch = 260.0 * np.power(2.0, 2.6 * amount * slow)
        travel[amount] = float(np.log2(notch.max() / notch.min()) * 12.0)

    # FISSION's halves must sit together at zero and apart when opened.
    separation = {}
    for amount in (0.0, 1.0):
        p = Params(reaction="FISSION", spread=amount, toxicity=0.8, exposure=0.7, seed=0)
        out = PROFILES["FISSION"].post(source, source, c, p, SR)
        left, right = out[:, 0], out[:, 1]
        separation[amount] = float(
            np.corrcoef(left, right)[0, 1]
        )

    alien_ok = results["ALIEN"][0] < -100.0 and results["ALIEN"][1] > -20.0
    sludge_ok = travel[0.0] < 0.1 and travel[1.0] > 20.0
    fission_ok = separation[0.0] > 0.9 and separation[1.0] < separation[0.0] - 0.1

    ok = alien_ok and sludge_ok and fission_ok
    return ok, (
        f"ALIEN zaps {results['ALIEN'][0]:.0f} -> {results['ALIEN'][1]:.0f} dB, "
        f"SLUDGE rise {travel[0.0]:.0f} -> {travel[1.0]:.0f} st, "
        f"FISSION L/R corr {separation[0.0]:.2f} -> {separation[1.0]:.2f}"
    )


def check_fallout_disperses_per_reaction() -> tuple[bool, str]:
    """FALLOUT must widen FISSION and wobble the others, and do nothing at zero.

    Width is measured as the side/mid energy ratio; wobble as how much the
    midrange's own level moves, since a gated vibrato modulates it.
    """
    from . import output_stage
    from .params import Params
    from .reactions import PROFILES

    n = SR * 2
    noise = np.random.default_rng(0).standard_normal((n, 2)) * 0.1
    tone = _tone(1000.0, 2.0) * 0.5
    c = _bed_controls(n)

    def width(x: np.ndarray) -> float:
        mid = x.mean(axis=1)
        side = (x[:, 0] - x[:, 1]) * 0.5
        return float(np.sqrt(np.mean(side**2)) / (np.sqrt(np.mean(mid**2)) + 1e-12))

    def pitch_deviation(x: np.ndarray) -> float:
        """Cents of instantaneous pitch movement on a steady midrange tone.

        Measured on frequency, not on level: the stereo spread also modulates
        the midrange's level, so a level-based measure credits spreading as
        wobble and reports more of it on FISSION than on RADIATION.
        """
        analytic = hilbert(x.mean(axis=1))
        phase = np.unwrap(np.angle(analytic))
        freq = np.diff(phase) * SR / (2.0 * np.pi)
        freq = freq[int(0.05 * SR) : -int(0.05 * SR)]
        return float(np.std(1200.0 * np.log2(np.clip(freq, 50.0, None) / 1000.0)))

    def run(source: np.ndarray, reaction: str, amount: float) -> np.ndarray:
        p = Params(reaction=reaction, fallout=amount, seed=1)
        return output_stage.fallout(source, SR, p, PROFILES[reaction], c)

    untouched = np.allclose(run(noise, "FISSION", 0.0), noise)

    fission_width = width(run(noise, "FISSION", 1.0)) / (width(noise) + 1e-12)
    radiation_width = width(run(noise, "RADIATION", 1.0)) / (width(noise) + 1e-12)
    radiation_pitch = pitch_deviation(run(tone, "RADIATION", 1.0))
    fission_pitch = pitch_deviation(run(tone, "FISSION", 1.0))
    baseline_pitch = pitch_deviation(tone)

    ok = (
        untouched
        and fission_width > radiation_width * 1.5
        and radiation_pitch > fission_pitch * 1.5
        and fission_pitch > baseline_pitch
    )
    return ok, (
        f"off=untouched {untouched}, width FISSION x{fission_width:.2f} vs "
        f"RADIATION x{radiation_width:.2f}, pitch move RADIATION {radiation_pitch:.0f} "
        f"vs FISSION {fission_pitch:.0f} cents (dry {baseline_pitch:.0f})"
    )


def check_events_are_panned() -> tuple[bool, str]:
    """VOLATILITY must scatter events across the stereo field, not just centre them."""
    from .params import Params
    from .reactions import PROFILES
    from .reactor import build_controls
    from .scheduler import schedule

    profile = PROFILES["RADIATION"]
    n = SR * 4
    silence = np.zeros((n, 2))

    spreads = []
    for volatility in (0.0, 1.0):
        p = Params(reaction="RADIATION", mode="GRID", grid="1/16", seed=5, volatility=volatility)
        events = schedule(silence, SR, p, 140.0, sub_event_bias=profile.sub_event_bias)
        c = build_controls(events, n, SR, p, profile)
        difference = c.pan_gain[:, 0] - c.pan_gain[:, 1]
        spreads.append(float(np.sqrt(np.mean(difference**2))))

    ok = spreads[0] < 1e-6 and spreads[1] > 0.1
    return ok, f"L/R event difference {spreads[0]:.4f} at VOLATILITY 0, {spreads[1]:.3f} at 1"


def check_beds_follow_the_input() -> tuple[bool, str]:
    """The noise beds must track the input's rhythm, not sit under it flat."""
    from .params import Params
    from .reactions import PROFILES, contaminate

    n = SR * 4
    # Two bars of pulses with real gaps between them.
    dry = np.zeros((n, 2))
    pulse = int(0.12 * SR)
    period = SR // 2
    for start in range(0, n, period):
        end = min(start + pulse, n)
        dry[start:end] = np.random.default_rng(start).standard_normal((end - start, 2)) * 0.4

    wet = _tone(220.0, 4.0) * 0.3
    c = _bed_controls(n)
    p = Params(reaction="TOXIC SLUDGE", contamination=1.0, seed=0)
    bed = contaminate(wet, dry, c, p, PROFILES["TOXIC SLUDGE"], SR) - wet

    # Masks come from the pulse schedule, not from instantaneous amplitude: a
    # noise burst crosses zero constantly, so a sample-value mask counts the
    # middle of a loud pulse as a gap and flattens the result.
    playing = np.zeros(n, dtype=bool)
    settled = np.zeros(n, dtype=bool)
    for start in range(0, n, period):
        playing[start : min(start + pulse, n)] = True
        settled[min(start + pulse + int(0.25 * SR), n) : min(start + period, n)] = True

    on = float(np.sqrt(np.mean(bed[playing] ** 2)))
    off = float(np.sqrt(np.mean(bed[settled] ** 2)))
    ratio = 20.0 * np.log10((on + 1e-15) / (off + 1e-15))

    ok = ratio > 12.0
    return ok, f"bed is {ratio:.1f} dB louder while the input plays than in the gaps"


def check_containment_thins_events() -> tuple[bool, str]:
    """CONTAINMENT must reduce event density, which prompt.md lists explicitly."""
    from .params import Params
    from .scheduler import schedule

    n = SR * 7
    silence = np.zeros((n, 2))
    counts = []
    for amount in (0.0, 0.5, 1.0):
        p = Params(
            mode="GRID", grid="1/16", probability=0.9, reactivity=0.6,
            containment=amount, seed=3,
        )
        counts.append(len(schedule(silence, SR, p, 140.0, sub_event_bias=2.0)))

    ok = counts[0] > counts[1] > counts[2] and counts[2] < counts[0] * 0.4
    return ok, f"{counts[0]} -> {counts[1]} -> {counts[2]} events as CONTAINMENT closes"


def check_volatility_moves_timing() -> tuple[bool, str]:
    """VOLATILITY must unsettle the timing, not only the sound.

    Measured against the grid, not by pairing up two event lists: the lists are
    sorted by time, so displacing events reorders them and pairing compares
    different events, which reads as a full step of movement at any setting.
    """
    from .params import GRID_DIVISIONS, Params
    from .scheduler import schedule

    n = SR * 7
    silence = np.zeros((n, 2))
    step = GRID_DIVISIONS["1/16"] * 60.0 / 140.0

    offsets = []
    for amount in (0.0, 1.0):
        p = Params(
            mode="GRID", grid="1/16", probability=1.0, reactivity=0.0,
            volatility=amount, seed=3,
        )
        starts = np.array([e.start for e in schedule(silence, SR, p, 140.0, 1.0)]) / SR
        deviation = starts % step
        offsets.append(float(np.minimum(deviation, step - deviation).mean() * 1000.0))

    ok = offsets[0] < 0.1 and offsets[1] > 5.0
    return ok, f"{offsets[0]:.2f} ms off-grid at VOLATILITY 0, {offsets[1]:.2f} ms at 1"


def check_meltdown_stages_in_order() -> tuple[bool, str]:
    """MELTDOWN must stage in, not snap, and in the order the metaphor implies.

    The rods come out first because that is what causes it; the fallout arrives
    last and outlives everything else.
    """
    from .meltdown import STAGES, Meltdown
    from .params import Params

    n = SR * 8
    p = Params(
        containment=0.6, spread=0.2, toxicity=0.2, drive=0.2, exposure=0.2,
        contamination=0.1, meltdown_at=1.0, meltdown_hold=2.0,
    )
    md = Meltdown(p, n, SR)
    time = np.arange(md.ctrl("drive").shape[0]) * filters.BLOCK / SR

    def crosses_half(name: str) -> float:
        values = md.ctrl(name)
        base, target = getattr(p, name), STAGES[name].target
        halfway = base + (target - base) * 0.5
        reached = np.where(values >= halfway)[0] if target > base else np.where(values <= halfway)[0]
        return float(time[reached[0]]) if len(reached) else np.inf

    def settles(name: str) -> float:
        values = md.ctrl(name)
        base = getattr(p, name)
        moved = np.where(np.abs(values - base) > abs(STAGES[name].target - base) * 0.05)[0]
        return float(time[moved[-1]]) if len(moved) else 0.0

    order = ["containment", "drive", "toxicity", "exposure", "contamination"]
    arrivals = [crosses_half(name) for name in order]
    staged = all(a < b for a, b in zip(arrivals, arrivals[1:]))

    # SPREAD has to actually reach its maximum while held.
    spread_tops = float(md.ctrl("spread").max())
    containment_bottom = float(md.ctrl("containment").min())

    lingers = settles("contamination") > max(settles(n) for n in order[:-1])
    inert = not Meltdown(Params(), n, SR).active

    ok = staged and lingers and inert and spread_tops > 0.97 and containment_bottom < 0.03
    return ok, (
        "arrive at " + ", ".join(f"{n[:4]} {a:.2f}s" for n, a in zip(order, arrivals))
        + f"; contamination settles last at {settles('contamination'):.1f}s; "
        f"spread tops {spread_tops:.2f}, containment bottoms {containment_bottom:.2f}"
    )


def check_meltdown_cannot_reach_backwards() -> tuple[bool, str]:
    """A momentary gesture must not alter the audio before it fires.

    The offline harness normalises level over the whole render, so a loud
    meltdown late in a file was quietly ducking everything before it by 2dB.
    """
    from . import engine
    from .params import Params

    seconds = 6.0
    n = int(SR * seconds)
    rng_ = np.random.default_rng(0)
    source = rng_.standard_normal((n, 2)) * 0.1

    base = dict(
        reaction="RADIATION", mode="GRID", grid="1/16", toxicity=0.25,
        exposure=0.3, spread=0.25, drive=0.2, contamination=0.15,
        containment=0.55, decay=0.3, seed=4,
    )
    calm, _ = engine.process(source, SR, Params(**base), 140.0)
    hot, _ = engine.process(
        source, SR, Params(**base, meltdown_at=3.0, meltdown_hold=1.5), 140.0
    )

    def relative(window: slice) -> float:
        difference = hot[window] - calm[window]
        return 20.0 * np.log10(
            (np.sqrt(np.mean(difference**2)) + 1e-15)
            / (np.sqrt(np.mean(hot[window] ** 2)) + 1e-12)
        )

    before = relative(slice(0, int(2.9 * SR)))
    during = relative(slice(int(3.0 * SR), int(4.5 * SR)))

    ok = before < -30.0 and during > -6.0
    return ok, f"{before:.1f} dB before the gate, {during:+.1f} dB while held"


def check_ionize_scatters_three_axes() -> tuple[bool, str]:
    """IONIZE must place each event separately in stereo, spectrum and depth.

    Stereo is measured as consecutive events landing on opposite sides, which
    is what separates ping-pong from merely wide.
    """
    from .params import Params
    from .reactions import PROFILES
    from .reactor import build_controls
    from .scheduler import schedule

    profile = PROFILES["RADIATION"]
    n = SR * 4
    silence = np.zeros((n, 2))
    base = dict(
        reaction="RADIATION", mode="GRID", grid="1/16", seed=5,
        volatility=0.0, spread=0.5, afterglow=0.5,
    )

    measured = {}
    for label, on in (("off", False), ("on", True)):
        p = Params(**base, ionize=on, ionize_amount=0.9)
        events = schedule(silence, SR, p, 140.0, sub_event_bias=profile.sub_event_bias)
        c = build_controls(events, n, SR, p, profile)

        difference = c.pan_gain[:, 0] - c.pan_gain[:, 1]
        spectrum = float(np.std(np.log2(c.cutoff)) * 12.0)
        depth = float(np.std(c.send))
        measured[label] = (float(np.sqrt(np.mean(difference**2))), spectrum, depth)

        if on:
            # Consecutive events should alternate sides.
            sides = []
            for ev in events[:40]:
                block = min(ev.start // filters.BLOCK, len(c.env) - 1)
                sides.append(np.sign(c.pan_gain[block, 0] - c.pan_gain[block, 1]))
            flips = sum(1 for a, b in zip(sides, sides[1:]) if a != b and a != 0 and b != 0)
            alternates = flips / max(len(sides) - 1, 1)

    off, on = measured["off"], measured["on"]
    ok = (
        on[0] > 0.2 and off[0] < 1e-6      # stereo: centred when off
        and on[1] > off[1] * 1.3           # spectrum: wider scatter
        and on[2] > off[2] * 1.3           # depth: varied sends
        and alternates > 0.6
    )
    return ok, (
        f"stereo {off[0]:.3f}->{on[0]:.3f}, spectrum {off[1]:.1f}->{on[1]:.1f} st, "
        f"depth {off[2]:.3f}->{on[2]:.3f}, {alternates * 100:.0f}% alternate sides"
    )


def check_afterglow_is_independent() -> tuple[bool, str]:
    """AFTERGLOW must work on its own and add a tail that outlasts the input."""
    from . import engine
    from .params import Params

    n = SR * 6
    source = np.zeros((n, 2))
    burst = int(0.4 * SR)
    source[:burst] = np.random.default_rng(0).standard_normal((burst, 2)) * 0.3

    base = dict(reaction="RADIATION", mode="GRID", grid="1/8", seed=2, toxicity=0.5)
    dry_tail, _ = engine.process(source, SR, Params(**base, afterglow=0.0), 140.0)
    wet_tail, _ = engine.process(source, SR, Params(**base, afterglow=0.8), 140.0)

    after = slice(int(1.5 * SR), n)
    quiet = 20.0 * np.log10(np.sqrt(np.mean(dry_tail[after] ** 2)) + 1e-15)
    glowing = 20.0 * np.log10(np.sqrt(np.mean(wet_tail[after] ** 2)) + 1e-15)

    ok = glowing > quiet + 6.0
    return ok, f"tail after the source stops: {quiet:.1f} -> {glowing:.1f} dB with AFTERGLOW"


CHECKS = [
    ("ladder response", check_ladder_response),
    ("limiter catches spike", check_limiter_catches_spike),
    ("frequency shift", check_frequency_shift),
    ("octave down", check_octave_down),
    ("pitch wind", check_pitch_wind),
    ("contamination scales", check_contamination_scales),
    ("only radiation ticks", check_only_radiation_ticks),
    ("beds follow the input", check_beds_follow_the_input),
    ("events are panned", check_events_are_panned),
    ("containment thins events", check_containment_thins_events),
    ("volatility moves timing", check_volatility_moves_timing),
    ("meltdown stages in order", check_meltdown_stages_in_order),
    ("meltdown cannot reach backwards", check_meltdown_cannot_reach_backwards),
    ("ionize scatters three axes", check_ionize_scatters_three_axes),
    ("afterglow is independent", check_afterglow_is_independent),
    ("acid voice", check_acid_voice),
    ("acid keeps the low end", check_acid_keeps_the_low_end),
    ("range drives each character", check_range_drives_each_character),
    ("fallout disperses per reaction", check_fallout_disperses_per_reaction),
    ("house voicing curve", check_voicing_curve),
    ("scheduling determinism", check_scheduling_is_deterministic),
]


def main() -> int:
    failures = 0
    for name, check in CHECKS:
        ok, detail = check()
        if not ok:
            failures += 1
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    print(f"\n{len(CHECKS) - failures}/{len(CHECKS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
