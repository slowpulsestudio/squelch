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

from . import filters, reactor, rng, saturation
from .params import Params
from .reactions import PROFILES

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
    header's docstring) rather than compensating the group delay out.

    Skip the latency, then the cold-start region. That region is exactly the
    two 81-tap filters' combined memory, (81 + 81) / 4 = 41 samples at the
    base rate, and not a sample more: the pipeline is FIR, memoryless curve,
    FIR, so nothing in it can carry state from the start beyond that. Past it
    the two agree to machine precision regardless of what the signal is
    doing.
    """
    n = 2500
    settle = 41
    latency = 20
    t = np.arange(n) / SR
    x = 0.6 * np.sin(2.0 * np.pi * 300.0 * t) + 0.5 * np.sin(2.0 * np.pi * 5000.0 * t)
    ref = saturation.oversampled(x[:, None], saturation.soft_clip)[:, 0]
    indices = np.arange(settle, n, 20)
    return ref[indices - latency]


def _sludge_reference(p: Params) -> np.ndarray:
    """Source/Dsp/Sludge.h's SludgeEngine against reactor._sludge_engine.

    The engine's own one-pole stages all start at zero state with no lookahead,
    same as _one_pole_hz, so they need no latency of their own. The settling
    window is much longer than the oversampler's own 41 though, and for a
    different reason: the oversampler's cold-start region is finite, but it
    then feeds the final smoothing one-pole, which is IIR. That stage smears
    the 41 samples into an exponentially decaying tail with a time constant of
    1/g_c -- about 16 samples at these parameters, reaching machine precision
    by prototype index ~300. 1000 is margin: g_c falls with f_effective, so a
    darker parameter set decays slower, and the figure is not universal.

    Run at two operating points, because almost every constant in configure()
    is a lo/hi interpolation and a single point cannot tell a correct one from
    an inverted one.
    """
    n = 4000
    settle = 1000
    latency = 20
    t = np.arange(n) / SR
    x = 0.5 * np.sin(2.0 * np.pi * 110.0 * t) + 0.3 * np.sin(2.0 * np.pi * 850.0 * t)
    stereo = np.stack([x, 0.8 * np.roll(x, 3)], axis=1)

    profile = PROFILES["SLUDGE"]
    zeros = np.zeros(n)
    out = reactor._sludge_engine(
        stereo, zeros, zeros, zeros, SR, profile, [], None, p, None
    )
    indices = np.arange(settle, n, 40)
    return out[indices - latency].reshape(-1)


SLUDGE_PARAMS_A = Params(
    decay=0.4, half_life=0.6, spread=0.65, reactivity=0.5, exposure=0.7,
    toxicity=0.35, seed=1,
)

#: Deliberately on the other side of every lo/hi interpolation in SLUDGE's
#: configure(), and clear of 0, 1 and 0.5 so no term collapses or goes
#: symmetric and hides an inversion.
SLUDGE_PARAMS_B = Params(
    decay=0.85, half_life=0.15, spread=0.3, reactivity=0.9, exposure=0.2,
    toxicity=0.75, seed=1,
)


def _alien_reference() -> np.ndarray:
    """Source/Dsp/Alien.h's voice against reactor._alien_engine.

    One event, so the comparison is a single voice rather than a sum of
    overlaps: the voice maths is what is being checked, and the pool that
    sums them is ordinary addition. ALIEN has no oversampler and no acausal
    stage anywhere, so there is no latency and no settling window -- sample 0
    must already agree.
    """
    from .scheduler import Event

    n = 4000
    silence = np.zeros((n, 2))
    p = Params(reaction="ALIEN", spread=0.6, decay=0.35, toxicity=0.7,
               exposure=0.4, seed=5)
    profile = PROFILES["ALIEN"]
    event = Event(start=0, index=3, intensity=1.0, decay_scale=1.0, tone=0.5,
                  pan=0.0, shape=0.5, depth=0.0, accent=False, slide=False)
    zeros = np.zeros(n)
    out = reactor._alien_engine(silence, zeros, zeros, zeros, SR, profile,
                               [event], None, p, None)
    return out[::20].reshape(-1)


def _chemical_reference() -> np.ndarray:
    """Source/Dsp/Chemical.h against reactor._chemical_engine.

    Two events, so the zero-order hold is exercised rather than just the
    ladder: the register must change at the second event's start and not
    before. No oversampler anywhere in CHEMICAL, so sample 0 must agree.
    """
    from .scheduler import Event

    n = 4000
    t = np.arange(n) / SR
    x = np.repeat((0.3 * np.sin(2.0 * np.pi * 150.0 * t))[:, None], 2, axis=1)
    p = Params(reaction="CHEMICAL", seed=9)
    profile = PROFILES["CHEMICAL"]

    def event(start, index):
        return Event(start=start, index=index, intensity=1.0, decay_scale=1.0,
                     tone=0.5, pan=0.0, shape=0.5, depth=0.0, accent=False,
                     slide=False)

    cutoff = np.full(n, 700.0)
    feedback = np.full(n, 2.2)
    drive = np.full(n, 1.4)
    out = reactor._chemical_engine(x, cutoff, feedback, drive, SR, profile,
                                   [event(0, 2), event(2000, 7)], None, p, None)
    return out[::20].reshape(-1)


def _radiation_reference() -> np.ndarray:
    """Source/Dsp/Radiation.h against reactor._radiation_engine.

    One event, so the percussive pulse is exercised alongside the resonator
    and the hashed AR(1) states. No oversampler, so sample 0 must agree.
    """
    from .scheduler import Event

    n = 4000
    t = np.arange(n) / SR
    x = np.stack([0.3 * np.sin(2.0 * np.pi * 180.0 * t),
                  0.25 * np.sin(2.0 * np.pi * 240.0 * t)], axis=1)
    p = Params(reaction="RADIATION", volatility=0.7, spread=0.6, decay=0.45,
               exposure=0.3, seed=4)
    profile = PROFILES["RADIATION"]
    event = Event(start=0, index=1, intensity=1.0, decay_scale=1.0, tone=0.5,
                  pan=0.0, shape=0.5, depth=0.0, accent=False, slide=False)
    zeros = np.zeros(n)
    out = reactor._radiation_engine(x, zeros, zeros, zeros, SR, profile,
                                    [event], None, p, None)
    return out[::20].reshape(-1)


def _fission_reference() -> np.ndarray:
    """Source/Dsp/Fission.h against reactor._fission_engine.

    The branch pair reads v2 back through a moving fractional delay. The
    prototype does that with np.interp over the whole array and clamps the
    read index at 0, so for the first D samples it reads v2[0]; a real delay
    line reads zeros there instead. That is a boundary convention, not a
    porting error, and it is bounded by the longest delay the parameters can
    ask for: (D0 + Dm) * sr = (0.0025 + 0.0035) * 44100 = 265 samples at
    SPREAD 1. 300 is that figure rounded up, not a guess.
    """
    from .scheduler import Event

    n = 4000
    settle = 300
    t = np.arange(n) / SR
    x = np.stack([0.3 * np.sin(2.0 * np.pi * 320.0 * t),
                  0.3 * np.sin(2.0 * np.pi * 300.0 * t)], axis=1)
    p = Params(reaction="FISSION", spread=0.8, decay=0.5, exposure=0.6, seed=2)
    profile = PROFILES["FISSION"]
    zeros = np.zeros(n)
    out = reactor._fission_engine(x, zeros, zeros, zeros, SR, profile,
                                  [], None, p, None)
    return out[settle::20].reshape(-1)


def _voice_reference() -> np.ndarray:
    """Source/Dsp/OutputStage.h's Voice against output_stage.voice.

    Four cascaded biquads with fixed coefficients, so an impulse response
    pins every one of them at once and nothing settles.
    """
    from . import output_stage

    impulse = np.zeros((64, 2))
    impulse[0] = 1.0
    return output_stage.voice(impulse, SR)[:, 0]


def _limiter_reference() -> np.ndarray:
    """Source/Dsp/OutputStage.h's PeakLimiter against output_stage.peak_limit.

    The prototype centres its rolling max on the current input while delaying
    the audio by only one window, so the gain envelope leads the sample it
    scales by 2w. Measured: a spike at 1500 starts ducking the output that
    carries input 1060. A causal port must therefore delay by 2w, which puts
    its output one window behind the prototype's -- hence the offset below.

    Settling is 3w: the prototype's rolling max uses mode='nearest' for its
    first w samples where a zero-initialised ring reads zeros, and the ring
    itself takes 2w to fill.
    """
    from . import output_stage

    n = 4000
    w = output_stage.lookahead_samples(SR)
    settle = 3 * w
    t = np.arange(n) / SR
    tone = 0.3 * np.sin(2.0 * np.pi * 220.0 * t)
    tone[1500:1510] += 3.0
    x = np.stack([tone, 0.8 * tone], axis=1)
    ref = output_stage.peak_limit(x, SR)
    indices = np.arange(settle, n, 20)
    return ref[indices - w].reshape(-1)


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
        "sludge_engine": _sludge_reference(SLUDGE_PARAMS_A),
        "sludge_engine_b": _sludge_reference(SLUDGE_PARAMS_B),
        "alien_engine": _alien_reference(),
        "chemical_engine": _chemical_reference(),
        "radiation_engine": _radiation_reference(),
        "fission_engine": _fission_reference(),
        "voice": _voice_reference(),
        "peak_limiter": _limiter_reference(),
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
