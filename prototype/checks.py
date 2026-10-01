"""Isolated checks on the DSP building blocks.

A measurement is only worth acting on once the instrument producing it has been
shown to be correct, so each component is verified on a known signal before any
conclusion is drawn from a full-mix render.

Run with: ./.venv/bin/python -m prototype.checks
"""

from __future__ import annotations

import sys

import numpy as np
from scipy.signal import welch

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
    """CONTAMINATION must map to a predictable delivered level, and reach zero.

    The grain previously measured -46 to -110 dB against the mix and was never
    audible, because its gain was scaled by two other parameters at once.
    """
    from .controls import Controls
    from .params import Params
    from .reactions import PROFILES, contaminate

    n = SR * 4
    wet = _tone(200.0, 4.0) * 0.3
    profile = PROFILES["RADIATION"]
    c = Controls(
        env=np.zeros(filters.n_blocks(n)),
        cutoff=np.full(filters.n_blocks(n), 500.0),
        resonance=np.full(filters.n_blocks(n), 4.0),
        starts=np.arange(0, n, SR // 8),
        damping=0.0,
    )

    levels = []
    for amount in (0.0, 0.25, 1.0):
        out = contaminate(wet, c, Params(contamination=amount, seed=0), profile, SR)
        diff = out - wet
        levels.append(
            20.0 * np.log10((np.sqrt(np.mean(diff**2)) + 1e-15) / (np.sqrt(np.mean(out**2)) + 1e-12))
        )

    silent = levels[0] < -200.0
    grain = -40.0 < levels[1] < -28.0
    loud = -18.0 < levels[2] < -8.0
    return silent and grain and loud, (
        f"off {levels[0]:.0f} dB, quarter {levels[1]:.1f} dB (grain), full {levels[2]:.1f} dB"
    )


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


CHECKS = [
    ("ladder response", check_ladder_response),
    ("limiter catches spike", check_limiter_catches_spike),
    ("frequency shift", check_frequency_shift),
    ("octave down", check_octave_down),
    ("pitch wind", check_pitch_wind),
    ("contamination scales", check_contamination_scales),
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
