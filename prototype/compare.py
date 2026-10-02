"""Check the C++ against this prototype, number by number.

The port is only finished when the shipping code produces the same numbers as
the reference. Listening to both and deciding they sound similar is not the
same thing: a filter with a transposed coefficient still makes a noise, and a
hash that is one bit out still schedules something.

Run the harness, then run this:

    cmake --build build --target SquelchHarness
    ./build/SquelchHarness | ./.venv/bin/python -m prototype.compare
"""

from __future__ import annotations

import json
import sys

import numpy as np
from scipy.signal import lfilter

from . import filters, rng, saturation

SR = 44100


def _impulse_response(b, a, length: int) -> np.ndarray:
    impulse = np.zeros(length)
    impulse[0] = 1.0
    return lfilter(b, a, impulse)


def _running_rms_reference() -> np.ndarray:
    n = 2000
    t = np.arange(n) / SR
    tone = np.sin(2.0 * np.pi * 220.0 * t)[:, None]
    level = filters.running_rms(np.repeat(tone, 2, axis=1), SR, 1.5)
    return level[::100]


def _oversampler_reference() -> np.ndarray:
    """Source/Dsp/Oversampler.h's causal pipeline matches scipy's acausal
    resample_poly round trip, but delayed by its measured latency (see that
    header's docstring) rather than compensating the group delay out. Skip
    latency, then a further settling region: starting from zero state, low
    frequency content takes longer than the bare latency to stop ringing
    from the cold start, so comparing too early would catch a real (if
    transient) difference rather than a bug.
    """
    n = 2500
    settle = 320
    latency = 20
    t = np.arange(n) / SR
    x = 0.6 * np.sin(2.0 * np.pi * 300.0 * t) + 0.5 * np.sin(2.0 * np.pi * 5000.0 * t)
    ref = saturation.oversampled(x[:, None], saturation.soft_clip)[:, 0]
    indices = np.arange(settle, n, 20)
    return ref[indices - latency]


def expectations() -> dict:
    """What the C++ should have produced."""
    from .filters import _rbj_highpass, _rbj_lowpass, _rbj_notch

    lo_b, lo_a = _rbj_lowpass(500.0, 2.0, SR)
    hi_b, hi_a = _rbj_highpass(500.0, 2.0, SR)
    no_b, no_a = _rbj_notch(500.0, 2.0, SR)

    impulse = np.zeros(32)
    impulse[0] = 1.0
    stereo = np.repeat(impulse[:, None], 2, axis=1)

    return {
        "urand": np.array([rng.urand(7, 3, i) for i in range(16)]),
        "uhash": np.array([float(rng.uhash(i) >> 11) for i in range(8)]),
        "lowpass": _impulse_response(lo_b, lo_a, 32),
        "highpass": _impulse_response(hi_b, hi_a, 32),
        "notch": _impulse_response(no_b, no_a, 32),
        "peaking": filters.peaking(stereo, 1000.0, -6.0, 0.9, SR)[:, 0],
        "low_shelf": filters.low_shelf(stereo, 70.0, 2.5, SR)[:, 0],
        "high_shelf": filters.high_shelf(stereo, 15000.0, 1.0, SR)[:, 0],
        "running_rms": _running_rms_reference(),
        "oversampler_soft_clip": _oversampler_reference(),
    }


def compare(measured: dict, tolerance: float = 1e-9) -> int:
    expected = expectations()
    failures = 0

    for name, want in expected.items():
        if name not in measured:
            print(f"[MISS] {name}: the harness did not report it")
            failures += 1
            continue

        got = np.asarray(measured[name], dtype=float)
        if got.shape != want.shape:
            print(f"[FAIL] {name}: {got.shape} values against {want.shape}")
            failures += 1
            continue

        error = float(np.max(np.abs(got - want)))
        scale = float(np.max(np.abs(want))) + 1e-18
        relative = error / scale

        if relative > tolerance:
            worst = int(np.argmax(np.abs(got - want)))
            print(
                f"[FAIL] {name}: off by {relative:.3e} relative, worst at index "
                f"{worst} ({got[worst]:.12g} against {want[worst]:.12g})"
            )
            failures += 1
        else:
            print(f"[PASS] {name}: matches to {relative:.1e}")

    total = len(expected)
    print(f"\n{total - failures}/{total} agree with the prototype")
    return failures


if __name__ == "__main__":
    sys.exit(1 if compare(json.load(sys.stdin)) else 0)
