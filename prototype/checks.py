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


CHECKS = [
    ("ladder response", check_ladder_response),
    ("limiter catches spike", check_limiter_catches_spike),
    ("frequency shift", check_frequency_shift),
    ("octave down", check_octave_down),
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
