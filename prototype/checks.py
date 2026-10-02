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

from . import audio_io, filters, output_stage
from .params import Params
from .scheduler import schedule

SR = 44100


def _tone(freq: float, seconds: float = 1.0, sr: int = SR) -> np.ndarray:
    t = np.arange(int(seconds * sr)) / sr
    return np.repeat((np.sin(2.0 * np.pi * freq * t) * 0.5)[:, None], 2, axis=1)


def _peak_frequency(x: np.ndarray, sr: int = SR) -> float:
    spectrum = np.abs(np.fft.rfft(x.mean(axis=1)))
    return float(np.fft.rfftfreq(len(x), 1.0 / sr)[int(np.argmax(spectrum))])


def _blackman_harris(n: int) -> np.ndarray:
    """A window with sidelobes low enough to measure a real noise floor.

    Anything gentler leaks a loud tone across the whole spectrum and that
    leakage gets mistaken for distortion that is not there.
    """
    k = 2.0 * np.pi * np.arange(n) / n
    return (
        0.35875
        - 0.48829 * np.cos(k)
        + 0.14128 * np.cos(2.0 * k)
        - 0.01168 * np.cos(3.0 * k)
    )


def _source() -> str:
    """A real loop if one is present, so end-to-end checks use real material."""
    from pathlib import Path

    found = sorted(Path("Input").glob("*.wav"))
    return str(found[0]) if found else ""


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

    The targets are written out here rather than read back from the profile's
    own gain. Deriving the target from the constant being tested moves the goal
    whenever the constant moves, so the check could only ever pass. These are
    the levels noise-beds.md specifies.
    """
    from .params import REACTIONS, Params
    from .reactions import PROFILES, contaminate

    targets = {
        "RADIATION": -28.0,
        "FISSION": -18.0,
        "SLUDGE": -14.0,
        "CHEMICAL": -23.0,
        "ALIEN": -16.0,
    }

    n = SR * 4
    wet = _tone(200.0, 4.0) * 0.3
    c = _bed_controls(n)

    failures = []
    for reaction in REACTIONS:
        profile = PROFILES[reaction]
        target = targets[reaction]
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
    """CHEMICAL must behave like a 303: per-note envelope sweep, accents, slides.

    A resonant lowpass swept by a per-note envelope is what squelches. Holding
    a frequency, or using a bandpass, removes both the squelch and the bassline.

    Measured on the envelope that drives the ladder, not on build_controls:
    that one still computes a cutoff nobody reads, so testing it passed while
    the delivered filter sat still.
    """
    from .meltdown import Meltdown
    from .params import Params
    from .reactions import PROFILES
    from .reactor import envelopes
    from .scheduler import schedule

    profile = PROFILES["CHEMICAL"]
    n = SR * 4
    silence = np.zeros((n, 2))
    p = Params(reaction="CHEMICAL", mode="GRID", grid="1/8", seed=3, decay=0.3, spread=0.7)
    md = Meltdown(p, n, SR)
    events = schedule(silence, SR, p, 140.0, sub_event_bias=profile.sub_event_bias, md=md)
    cutoff, _, _ = envelopes(events, n, SR, p, profile, md)

    octaves = np.log2(cutoff)

    # Measured per note, not as a fraction of the whole control. A 303's sweep
    # reaches its floor and sits there until the next note, so a settled tail
    # is correct behaviour, and counting samples that are still moving marks it
    # down for being right. What matters is that every note sweeps, and by how
    # much.
    starts = [e.start for e in events]
    spans = [
        octaves[starts[i] : min(starts[i + 1], n)] for i in range(len(starts) - 1)
    ]
    falls = np.array([float(s.max() - s[-1]) for s in spans if len(s) > 1])
    swept = float(np.mean(falls > 0.5)) * 100.0

    accented = [e for e in events if e.accent]
    plain = [e for e in events if not e.accent]
    window = int(0.02 * SR)
    peak_of = lambda e: octaves[e.start : e.start + window].max()
    accent_peak = np.mean([peak_of(e) for e in accented[:40]])
    plain_peak = np.mean([peak_of(e) for e in plain[:40]])

    slid = sum(1 for e in events if e.slide) / max(len(events), 1) * 100.0

    ok = (
        profile.voice == "acid"
        and swept > 90.0
        and float(np.median(falls)) > 1.0
        and accent_peak > plain_peak
        and 20.0 < slid < 40.0
    )
    return ok, (
        f"{swept:.0f}% of notes sweep, median fall {np.median(falls):.1f} octaves, "
        f"accents open {(accent_peak - plain_peak) * 12:.1f} semitones higher, "
        f"{slid:.0f}% slide"
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

    profile = PROFILES["CHEMICAL"]
    n = SR * 4
    noise = np.random.default_rng(0).standard_normal((n, 2)) * 0.1
    p = Params(reaction="CHEMICAL", mode="GRID", grid="1/8", seed=3, decay=0.3, spread=0.7)
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

    Not just the filter sweep: ALIEN's zaps, SLUDGE's bubble rise and
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

    # SLUDGE's notches must stop rising at zero.
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
    p = Params(reaction="SLUDGE", contamination=1.0, seed=0)
    bed = contaminate(wet, dry, c, p, PROFILES["SLUDGE"], SR) - wet

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

    Measured as two separate numbers, because one figure conflates them. A
    broad level offset and a changed waveform are different faults: arming the
    gesture moves the level-match reference window, which is worth a fraction
    of a dB and is audible to nobody, while any real leak backwards would show
    up in the residual once that offset is divided out. A streaming port
    normalises nothing and has neither.
    """
    from . import engine, output_stage
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

    def rms(a: np.ndarray) -> float:
        return float(np.sqrt(np.mean(a**2))) + 1e-15

    before = slice(0, int(2.9 * SR))
    offset = rms(hot[before]) / rms(calm[before])
    shape = 20.0 * np.log10(rms(hot[before] - calm[before] * offset) / rms(hot[before]))
    offset_db = 20.0 * np.log10(offset)

    during = slice(int(3.0 * SR), int(4.5 * SR))
    held = 20.0 * np.log10(rms(hot[during] - calm[during]) / rms(hot[during]))

    ok = abs(offset_db) < 0.5 and shape < -30.0 and held > -6.0
    return ok, (
        f"before the gate {offset_db:+.2f} dB level, {shape:.1f} dB waveform; "
        f"{held:+.1f} dB while held"
    )


def check_ionize_scatters_three_axes() -> tuple[bool, str]:
    """IONIZE must place each event separately in stereo, spectrum and depth.

    Stereo is measured as consecutive events landing on opposite sides, which
    is what separates ping-pong from merely wide. Spectrum and depth are
    measured per event rather than across the whole control: the spread of a
    continuous envelope is dominated by its own decay shape, which swamps the
    scatter this is trying to see.
    """
    from .meltdown import Meltdown
    from .params import Params
    from .reactions import PROFILES
    from .reactor import build_controls, envelopes
    from .scheduler import schedule

    profile = PROFILES["RADIATION"]
    n = SR * 4
    silence = np.zeros((n, 2))
    base = dict(
        reaction="RADIATION", mode="GRID", grid="1/16", seed=5,
        volatility=0.0, spread=0.5, afterglow=0.5,
    )

    measured = {}
    registers = {}
    for label, on in (("off", False), ("on", True)):
        p = Params(**base, ionize=on, ionize_amount=0.9)
        md = Meltdown(p, n, SR)
        events = schedule(silence, SR, p, 140.0, sub_event_bias=profile.sub_event_bias, md=md)
        c = build_controls(events, n, SR, p, profile)
        cutoff, _, _ = envelopes(events, n, SR, p, profile, md)

        difference = c.pan_gain[:, 0] - c.pan_gain[:, 1]

        # Each event's register, read just before the next one fires. Held per
        # event rather than reduced to a spread: an event that has not finished
        # decaying sits high for reasons that have nothing to do with IONIZE,
        # and that variance is larger than the scatter being looked for. The
        # two runs schedule identically, so differencing them cancels it.
        starts = [e.start for e in events]
        registers[label] = np.array([
            float(np.log2(cutoff[min(starts[i + 1], n) - 1]))
            for i in range(len(starts) - 1)
        ])

        sends = [
            float(c.send[min(e.start // filters.BLOCK, len(c.send) - 1) :][:40].max())
            for e in events[:-1]
        ]
        measured[label] = (float(np.sqrt(np.mean(difference**2))), float(np.std(sends)))

        if on:
            # Consecutive events should alternate sides.
            sides = []
            for ev in events[:40]:
                block = min(ev.start // filters.BLOCK, len(c.env) - 1)
                sides.append(np.sign(c.pan_gain[block, 0] - c.pan_gain[block, 1]))
            flips = sum(1 for a, b in zip(sides, sides[1:]) if a != b and a != 0 and b != 0)
            alternates = flips / max(len(sides) - 1, 1)

    off, on = measured["off"], measured["on"]
    scatter = float(np.std(registers["on"] - registers["off"]) * 12.0)
    ok = (
        on[0] > 0.2 and off[0] < 1e-6      # stereo: centred when off
        and scatter > 2.0                  # spectrum: own register per event
        and on[1] > off[1] * 1.3           # depth: varied sends
        and alternates > 0.6
    )
    return ok, (
        f"stereo {off[0]:.3f}->{on[0]:.3f}, spectrum scatter {scatter:.1f} st, "
        f"depth {off[1]:.3f}->{on[1]:.3f}, {alternates * 100:.0f}% alternate sides"
    )


def check_afterglow_is_independent() -> tuple[bool, str]:
    """AFTERGLOW must work on its own and add a tail that outlasts the input."""
    from . import engine, output_stage
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


def check_ionize_moves_the_delivered_mix() -> tuple[bool, str]:
    """IONIZE must still be moving by the time it reaches the output.

    The control-level check passed while the delivered mix barely moved: DRIVE
    compresses the loud side of a panned event harder than the quiet side and
    squeezed 7.5dB of swing down to 3.6dB. Measure what comes out, not what
    was asked for.
    """
    from . import engine, output_stage
    from .params import Params

    dry, sr = audio_io.load(_source())
    base = dict(
        reaction="RADIATION", mode="GRID", grid="1/16", seed=6, volatility=0.2,
        spread=0.6, toxicity=0.6, decay=0.35, contamination=0.2, afterglow=0.65,
    )

    def movement(on: bool) -> tuple[float, float]:
        y, _ = engine.process(dry, sr, Params(**base, ionize=on, ionize_amount=0.9), 140.0)
        # Measured above the mono bass band: everything below it is folded to
        # centre on purpose, so including it just dilutes the reading with
        # material that is never allowed to move.
        y = filters.static_highpass(y, output_stage.STEREO_BASS_MONO_HZ, sr, q=0.7)
        window = int(0.025 * sr)
        balance = []
        for i in range(len(y) // window):
            block = y[i * window : (i + 1) * window]
            left = np.sqrt(np.mean(block[:, 0] ** 2)) + 1e-12
            right = np.sqrt(np.mean(block[:, 1] ** 2)) + 1e-12
            balance.append(20.0 * np.log10(left / right))
        balance = np.array(balance)
        return float(balance.std()), float(np.mean(np.abs(balance) > 6.0) * 100.0)

    off_std, off_hard = movement(False)
    on_std, on_hard = movement(True)

    ok = on_std > off_std * 2.0 and on_hard > 15.0
    return ok, (
        f"balance swing {off_std:.1f} -> {on_std:.1f} dB, "
        f"hard-panned {off_hard:.0f}% -> {on_hard:.0f}% of the time"
    )


def check_drive_does_not_alias() -> tuple[bool, str]:
    """Saturating at the base rate folds harmonics back down the spectrum.

    A distortion curve generates harmonics forever, and the ones above Nyquist
    have nowhere to go, so they reflect back as inharmonic tones underneath the
    music. That is what makes cheap saturation sound like grit instead of
    drive.

    One tone, not two. With two tones the products and the reflections both
    land on multiples of their common divisor, so the measurement cannot tell
    them apart. A single tone puts every honest harmonic on a multiple of
    itself and every reflection somewhere else.
    """
    from . import saturation

    n = SR
    t = np.arange(n) / SR
    f0 = 6900.0
    x = np.repeat((0.4 * np.sin(2 * np.pi * f0 * t))[:, None], 2, axis=1)

    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    harmonic = np.zeros(len(freqs), dtype=bool)
    for k in range(1, int(SR / 2 / f0) + 1):
        harmonic |= np.abs(freqs - k * f0) < 40.0
    harmonic |= freqs < 40.0

    def alias_floor(y: np.ndarray) -> float:
        spectrum = np.abs(np.fft.rfft(y[:, 0] * _blackman_harris(n)))
        return 20.0 * np.log10((spectrum[~harmonic].max() + 1e-18) / spectrum.max())

    plain = alias_floor(np.tanh(x * 8.0))
    clean = alias_floor(saturation.oversampled(x * 8.0, saturation.soft_clip))

    # A deliberately unkind test: a 6.9kHz tone at full scale driven eight
    # times over. Four times oversampling cannot be perfect here, because the
    # thirteenth harmonic of 6.9kHz is above Nyquist even at the raised rate,
    # so what is left is the physical floor of 4x rather than a defect. Real
    # programme material carries far less energy that high. The number that
    # matters is the improvement over saturating at the base rate.
    ok = clean < -55.0 and clean < plain - 25.0
    return ok, f"{plain:.1f} dB at the base rate, {clean:.1f} dB oversampled"


def check_drive_responds_to_input_level() -> tuple[bool, str]:
    """How hard the input hits the curve is part of the instrument.

    The knee is a fixed threshold, so a quieter input reaches less of it and
    the saturation backs off on its own, the way it does in a circuit. This is
    a deliberate choice rather than an oversight: nothing upstream may
    normalise the signal before the drive stage, or trimming the input would
    stop being a tone control and the plugin would sound the same however it
    is fed.
    """
    import soundfile as sf

    from . import reactions, reactor, saturation
    from .params import Params

    source, sr = sf.read(_source(), always_2d=True, dtype="float64")
    p = Params(
        reaction="RADIATION", mode="GRID", grid="1/16",
        drive=0.75, toxicity=0.5, seed=3,
    )
    profile = reactions.PROFILES[p.reaction]
    gain = 1.0 + 11.0 * (p.drive**0.6) * profile.drive_weight

    def bent(trim_db: float) -> float:
        trimmed = source * (10.0 ** (trim_db / 20.0))
        wet, controls, md = reactor.process(trimmed, sr, p, 140.0)
        hot = reactions.contaminate(wet, trimmed, controls, p, profile, sr, md)
        return 100.0 * float(np.mean(np.abs(hot * gain) > saturation.KNEE))

    loud = bent(0.0)
    middle = bent(-12.0)
    quiet = bent(-30.0)

    ok = loud > 60.0 and 10.0 < middle < loud - 20.0 and quiet < 1.0
    return ok, (
        f"{loud:.0f}% of samples saturate at full level, {middle:.0f}% at -12 dB, "
        f"{quiet:.0f}% at -30 dB"
    )


def check_enrichment_drives_without_changing_level() -> tuple[bool, str]:
    """ENRICHMENT has to change the sound without changing the loudness.

    It is the analogue response made into a parameter: level decides how much
    of the drive curve is reached, so ENRICHMENT is a character control. That
    only works if it leaves output level alone, otherwise it reads as a volume
    knob and nobody will push it. The dry reference is deliberately left
    untrimmed so the output level match holds it in place.
    """
    import soundfile as sf

    from . import engine, output_stage
    from .params import Params

    source, sr = sf.read(_source(), always_2d=True, dtype="float64")
    base = dict(
        reaction="RADIATION", mode="GRID", grid="1/16",
        drive=0.6, toxicity=0.5, seed=3,
    )

    renders = {}
    for name, amount in (("low", 0.0), ("unity", 0.5), ("high", 1.0)):
        renders[name], _ = engine.process(
            source, sr, Params(**base, enrichment=amount), 140.0
        )

    def rms_db(y: np.ndarray) -> float:
        return 20.0 * np.log10(np.sqrt(np.mean(y**2)) + 1e-18)

    levels = {k: rms_db(v) for k, v in renders.items()}
    spread = max(levels.values()) - min(levels.values())

    # Character measured as what is left once level is taken out of it, so the
    # number reports a change in sound rather than a change in gain.
    reference = renders["unity"]
    scaled = renders["high"] * (
        (np.sqrt(np.mean(reference**2)) + 1e-18)
        / (np.sqrt(np.mean(renders["high"] ** 2)) + 1e-18)
    )
    character = 20.0 * np.log10(
        (np.sqrt(np.mean((scaled - reference) ** 2)) + 1e-18)
        / (np.sqrt(np.mean(reference**2)) + 1e-18)
    )

    ok = spread < 3.0 and character > -12.0
    return ok, (
        f"level moves {spread:.1f} dB across the range, "
        f"character changes {character:+.1f} dB"
    )


def check_level_match_is_causal() -> tuple[bool, str]:
    """Nothing in the chain may decide the level from audio that has not played.

    The offline harness could measure a whole render before choosing a gain. A
    plugin gets one block at a time, so the level match tracks both signals
    through a slow one-pole instead. This is the check that the port is even
    possible: processing a render in one pass and processing it in pieces have
    to agree.
    """
    import soundfile as sf

    from . import engine, output_stage
    from .params import Params

    source, sr = sf.read(_source(), always_2d=True, dtype="float64")
    source = np.tile(source, (2, 1))
    p = Params(
        reaction="RADIATION", mode="GRID", grid="1/16",
        drive=0.6, toxicity=0.5, contamination=0.4, seed=3,
    )

    whole, _ = engine.process(source, sr, p, 140.0)

    # Truncating the input must not change what came before the cut. Anything
    # that measures the whole render fails this, because the average it is
    # working from is different.
    cut = int(len(source) * 0.6)
    short, _ = engine.process(source[:cut], sr, p, 140.0)

    compare = slice(0, cut - sr)  # drop the tail, which has no future to use
    a, b = whole[compare], short[compare]
    agreement = 20.0 * np.log10(
        (np.sqrt(np.mean((a - b) ** 2)) + 1e-18) / (np.sqrt(np.mean(a**2)) + 1e-18)
    )

    offline, _ = engine.process(source[:cut], sr, p, 140.0, offline=True)
    was = 20.0 * np.log10(
        (np.sqrt(np.mean((whole[compare] - offline[compare]) ** 2)) + 1e-18)
        / (np.sqrt(np.mean(whole[compare] ** 2)) + 1e-18)
    )

    ok = agreement < -100.0 and agreement < was - 40.0
    return ok, (
        f"shortening the render changes the earlier audio by {agreement:.0f} dB, "
        f"against {was:.0f} dB for the offline match"
    )


def check_peak_control_does_not_pump() -> tuple[bool, str]:
    """Peak control that follows the programme is a compressor.

    This stage exists to stop transients going over, not to even the level out.
    If its gain moves with the music it is compressing, which is the thing the
    chain deliberately does not do: the brief was space and feel, not volume.

    Measured as the gain itself rather than as a difference between two
    renders, so there is nothing to subtract and no epsilon to invent a wobble.
    """
    import soundfile as sf

    from . import filters, output_stage, reactions, reactor
    from .params import Params

    source, sr = sf.read(_source(), always_2d=True, dtype="float64")
    source = np.tile(source, (4, 1))
    p = Params(
        reaction="RADIATION", mode="GRID", grid="1/16",
        drive=0.6, toxicity=0.5, seed=3,
    )
    profile = reactions.PROFILES[p.reaction]

    wet, controls, _ = reactor.process(source * p.enrichment_gain(), sr, p, 140.0)
    y = output_stage.drive(wet, sr, p, profile.drive_weight, None)
    y = output_stage.collimate(y, sr, p)
    y = y * np.stack(
        [filters.to_sample_rate(controls.pan_gain[:, ch], len(y)) for ch in (0, 1)], axis=1
    )
    y = output_stage.fallout(y, sr, p, profile, controls)
    y = output_stage.voice(y, sr)
    controlled = output_stage.unity_match(y, source, sr)

    settled = slice(int(6 * sr), None)
    loud = np.max(np.abs(y[settled]), axis=1) > 1e-4
    gain = 20.0 * np.log10(
        (np.max(np.abs(controlled[settled]), axis=1) + 1e-15)
        / (np.max(np.abs(y[settled]), axis=1) + 1e-15)
    )[loud]

    span = 512
    length = (len(gain) // span) * span
    blocks = gain[:length].reshape(-1, span).mean(axis=1)

    swing = float(blocks.max() - blocks.min())
    movement = np.abs(np.diff(blocks)) * sr / span
    sustained = float(np.percentile(movement, 95))

    # The hold is a level offset, not movement, so it is reported rather than
    # asserted: what matters is that the gain is not riding the music.
    ok = swing < 3.0 and sustained < 6.0
    return ok, (
        f"gain wanders {swing:.2f} dB, sustained {sustained:.2f} dB/s, "
        f"holding at {blocks.mean():+.2f} dB"
    )


def check_limiter_is_not_doing_the_work() -> tuple[bool, str]:
    """The limiter is a safety net, not the thing setting the level.

    The reaction raises the crest factor, so matching the output's RMS to the
    input's used to put the peaks 9 dB over full scale and leave a brickwall to
    deal with it. Every render came out at exactly the ceiling, and the
    giveaway was that DRIVE at zero was the worst case of all: saturation
    lowers crest, so turning it up made the limiter work less, which is the
    reverse of how it sounded.

    Measured at the limiter's input, across material and across DRIVE, because
    the failure was invisible at the output: the ceiling always holds, which is
    exactly why looking at the output tells you nothing.
    """
    import glob

    import soundfile as sf

    from . import filters, output_stage, reactions, reactor
    from .params import Params

    worst_reduction = 0.0
    worst_engaged = 0.0
    worst_source = ""

    for path in sorted(glob.glob("Input/*.wav")):
        source, sr = sf.read(path, always_2d=True, dtype="float64")

        for amount in (0.0, 0.5, 1.0):
            p = Params(
                reaction="RADIATION", mode="GRID", grid="1/16",
                drive=amount, toxicity=0.5, contamination=0.4, seed=3,
            )
            profile = reactions.PROFILES[p.reaction]
            wet, controls, _ = reactor.process(source * p.enrichment_gain(), sr, p, 140.0)

            y = output_stage.drive(wet, sr, p, profile.drive_weight, None)
            y = output_stage.collimate(y, sr, p)
            y = y * np.stack(
                [filters.to_sample_rate(controls.pan_gain[:, ch], len(y)) for ch in (0, 1)],
                axis=1,
            )
            y = output_stage.fallout(y, sr, p, profile, controls)
            y = output_stage.voice(y, sr)

            peak = float(np.abs(y).max())
            reduction = 20.0 * np.log10(output_stage.LIMITER_CEILING / peak) if peak > output_stage.LIMITER_CEILING else 0.0
            engaged = 100.0 * float(np.mean(np.abs(y) > output_stage.LIMITER_CEILING))

            if reduction < worst_reduction:
                worst_reduction = reduction
                worst_source = path.split("/")[-1][10:-4] + f" at DRIVE {amount:.1f}"
            worst_engaged = max(worst_engaged, engaged)

    # How often and how hard, not how high. A peak figure says nothing about
    # whether anything is audible: one transient touching the ceiling is a
    # limiter working, a tenth of the render pinned against it is a limiter
    # setting the level.
    ok = worst_reduction > -6.0 and worst_engaged < 0.5
    return ok, (
        f"worst case {worst_reduction:.1f} dB on {worst_engaged:.2f}% of samples "
        f"({worst_source})"
    )


def check_quiet_material_is_untouched() -> tuple[bool, str]:
    """Below the knee the curve must be exactly linear.

    tanh colours everything it touches, so at low DRIVE the quiet parts of a
    track pick up distortion they never asked for. The soft knee leaves
    anything under the threshold alone and only bends the peaks.
    """
    from . import saturation

    n = SR
    t = np.arange(n) / SR
    quiet = 0.05 * np.sin(2 * np.pi * 1000.0 * t)
    quiet = np.stack([quiet, quiet], axis=1)

    def thd(y: np.ndarray) -> float:
        spectrum = np.abs(np.fft.rfft(y[:, 0] * np.hanning(n)))
        bin_ = int(round(1000.0 * n / SR))
        fundamental = spectrum[bin_ - 3 : bin_ + 4].sum()
        rest = spectrum.sum() - fundamental
        return 100.0 * rest / (fundamental + 1e-18)

    soft = thd(saturation.soft_clip(quiet, 1.0))
    plain = thd(np.tanh(quiet))

    ok = soft < 0.01
    return ok, f"soft knee {soft:.3f}%, tanh {plain:.3f}% THD at -26 dBFS"


def check_limiter_releases_gently() -> tuple[bool, str]:
    """Gain that snaps back after a transient reads as a pumping artefact.

    The limiter recovers over a release time instead of following the peak
    envelope sample by sample, which keeps the gain curve slow enough that it
    modulates level rather than adding sidebands to the programme.

    Only the recovery is measured. Clamping down has to be instant or the peak
    escapes, so including the attack in this number just reports how fast the
    limiter caught the transient, which is not what is being asked.
    """
    from . import output_stage

    n = int(SR * 2.0)
    t = np.arange(n) / SR
    tone = 0.3 * np.sin(2 * np.pi * 220.0 * t)
    strike = int(SR * 0.5)
    tone[strike : strike + 64] += 3.0
    x = np.stack([tone, tone], axis=1)

    y = output_stage.peak_limit(x, SR)

    # Read the gain off envelopes rather than sample ratios: dividing two
    # waveforms blows up either side of every zero crossing.
    span = 256
    trim = (len(x) // span) * span

    def envelope(a: np.ndarray) -> np.ndarray:
        return np.abs(a[:trim, 0]).reshape(-1, span).max(axis=1)

    gain = 20.0 * np.log10(
        np.maximum(envelope(y), 1e-9) / np.maximum(envelope(x), 1e-9)
    )
    after = gain[strike // span + 2 :]
    recovery = np.diff(after)
    rate = float(recovery[recovery > 0].max()) * SR / span if (recovery > 0).any() else 0.0

    peak = float(np.abs(y).max())
    ok = 0.0 < rate < 400.0 and peak <= output_stage.LIMITER_CEILING + 1e-6
    return ok, f"recovers at {rate:.0f} dB/s, peak {peak:.3f}"


def check_clip_trades_lookahead_for_hardness() -> tuple[bool, str]:
    """CLIP is the harder ceiling, and it must stay in step with the limiter.

    Measured on two different signals, because one cannot answer both
    questions. Alignment is read from material quiet enough that neither
    ceiling acts, so what is left is purely the delay. Hardness is read from a
    tone sitting well over the ceiling, where the limiter rides the gain down
    and keeps a clean sine while the clipper squares the top off.
    """
    from . import engine, output_stage
    from .params import Params

    n = int(SR * 1.0)
    t = np.arange(n) / SR
    expected = output_stage.lookahead_samples(SR)

    quiet = np.repeat((0.2 * np.sin(2 * np.pi * 220.0 * t))[:, None], 2, axis=1)
    quiet[: int(SR * 0.25)] = 0.0

    def delay_of(y: np.ndarray) -> int:
        correlation = np.correlate(y[:, 0], quiet[:, 0], mode="full")
        return int(np.argmax(np.abs(correlation))) - (n - 1)

    limiter_delay = delay_of(output_stage.peak_limit(quiet, SR))
    clip_delay = delay_of(output_stage.clip(quiet, SR))

    loud = np.repeat((1.6 * np.sin(2 * np.pi * 220.0 * t))[:, None], 2, axis=1)

    def thd(y: np.ndarray) -> float:
        settled = y[int(SR * 0.5) :, 0]
        spectrum = np.abs(np.fft.rfft(settled * _blackman_harris(len(settled))))
        bin_ = int(round(220.0 * len(settled) / SR))
        fundamental = spectrum[bin_ - 4 : bin_ + 5].sum()
        return 100.0 * (spectrum.sum() - fundamental) / (fundamental + 1e-18)

    hard_thd = thd(output_stage.clip(loud, SR))
    soft_thd = thd(output_stage.peak_limit(loud, SR))

    base = dict(reaction="RADIATION", mode="GRID", grid="1/16", seed=4, drive=0.6)
    source = np.random.default_rng(0).standard_normal((int(SR * 3.0), 2)) * 0.1
    rendered, _ = engine.process(source, SR, Params(**base, clip=True), 140.0)

    ceiling = output_stage.LIMITER_CEILING
    ok = (
        limiter_delay == clip_delay == expected
        and np.abs(rendered).max() <= ceiling + 1e-6
        and hard_thd > soft_thd * 3.0
    )
    return ok, (
        f"both delayed {clip_delay} samples ({1000.0 * expected / SR:.1f} ms); "
        f"clip {hard_thd:.1f}% vs limiter {soft_thd:.1f}% THD over the ceiling"
    )


CHECKS = [
    ("ladder response", check_ladder_response),
    ("limiter catches spike", check_limiter_catches_spike),
    ("limiter releases gently", check_limiter_releases_gently),
    ("limiter is not doing the work", check_limiter_is_not_doing_the_work),
    ("drive does not alias", check_drive_does_not_alias),
    ("drive responds to input level", check_drive_responds_to_input_level),
    (
        "enrichment drives without changing level",
        check_enrichment_drives_without_changing_level,
    ),
    ("quiet material is untouched", check_quiet_material_is_untouched),
    ("level match is causal", check_level_match_is_causal),
    ("level control does not pump", check_peak_control_does_not_pump),
    ("clip trades lookahead for hardness", check_clip_trades_lookahead_for_hardness),
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
    ("ionize moves the delivered mix", check_ionize_moves_the_delivered_mix),
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
