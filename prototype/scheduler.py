"""When the reactor is allowed to react.

Covers MODE, GRID, FLUX, PROBABILITY and the event-count half of REACTIVITY.
Event times derive from musical position rather than a free-running counter, so
a bounce matches what was heard.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import rng
from .params import GRID_DIVISIONS, Params

#: Most sub-events a single trigger can fan out into at REACTIVITY = 1.
MAX_SUB_EVENTS = 5

#: FREE mode runs at this ratio of the grid step, deliberately not a simple
#: fraction so it drifts against the bar instead of relocking to it.
FREE_RATE_RATIO = 0.7213


@dataclass
class Event:
    start: int
    index: int
    intensity: float
    decay_scale: float
    tone: float
    pan: float


def _step_seconds(p: Params, bpm: float) -> float:
    return GRID_DIVISIONS[p.grid] * 60.0 / bpm


def _grid_times(p: Params, bpm: float, duration: float) -> list[tuple[int, float]]:
    step = _step_seconds(p, bpm)
    times = []
    k = 0
    t = 0.0
    while t < duration:
        swing = 0.5 * p.flux * step if k % 2 else 0.0
        jitter = 0.3 * p.flux * step * rng.ubipolar(p.seed, 101, k)
        times.append((k, t + swing + jitter))
        k += 1
        t = k * step
    return times


def _random_times(p: Params, bpm: float, duration: float) -> list[tuple[int, float]]:
    step = _step_seconds(p, bpm)
    times = []
    k = 0
    t = 0.0
    while t < duration:
        times.append((k, t))
        spread = 0.25 + 1.75 * rng.urand(p.seed, 102, k)
        t += step * (1.0 - p.flux + p.flux * spread * 2.0)
        k += 1
    return times


def _free_times(p: Params, bpm: float, duration: float) -> list[tuple[int, float]]:
    step = _step_seconds(p, bpm) * FREE_RATE_RATIO
    times = []
    k = 0
    while k * step < duration:
        jitter = 0.3 * p.flux * step * rng.ubipolar(p.seed, 103, k)
        times.append((k, k * step + jitter))
        k += 1
    return times


def _input_times(x: np.ndarray, sr: int, p: Params) -> list[tuple[int, float]]:
    """Onset times from a half-wave-rectified spectral-flux style envelope."""
    hop = 256
    mono = np.abs(x).max(axis=1)
    n_frames = len(mono) // hop
    envelope = np.array([mono[i * hop : (i + 1) * hop].max() for i in range(n_frames)])
    flux = np.diff(envelope, prepend=envelope[:1])
    flux[flux < 0.0] = 0.0
    if flux.max() <= 0.0:
        return []
    flux /= flux.max()
    threshold = 0.12
    min_gap = int(0.045 * sr / hop)
    times = []
    last = -min_gap
    k = 0
    for i in range(1, n_frames - 1):
        if flux[i] < threshold or flux[i] < flux[i - 1] or flux[i] < flux[i + 1]:
            continue
        if i - last < min_gap:
            continue
        last = i
        times.append((k, i * hop / sr))
        k += 1
    return times


def _base_times(x: np.ndarray, sr: int, p: Params, bpm: float) -> list[tuple[int, float]]:
    duration = len(x) / sr
    if p.mode == "GRID":
        return _grid_times(p, bpm, duration)
    if p.mode == "RANDOM":
        return _random_times(p, bpm, duration)
    if p.mode == "FREE":
        return _free_times(p, bpm, duration)
    if p.mode == "INPUT":
        return _input_times(x, sr, p)
    raise ValueError(f"unknown MODE: {p.mode}")


def schedule(x: np.ndarray, sr: int, p: Params, bpm: float, sub_event_bias: float = 1.0) -> list[Event]:
    """Produce every reaction event for this render.

    PROBABILITY gates whether a step fires at all; REACTIVITY decides how many
    sub-events it fans out into; VOLATILITY sets how far each one deviates from
    the last.
    """
    n = len(x)
    step = _step_seconds(p, bpm)
    events: list[Event] = []

    for k, t in _base_times(x, sr, p, bpm):
        if rng.urand(p.seed, 1, k) >= p.probability:
            continue

        count = 1 + int(round(p.reactivity * sub_event_bias * (MAX_SUB_EVENTS - 1)))
        for s in range(count):
            offset = (s / count) * step * (0.9 if p.mode != "INPUT" else 0.5)
            start = int((t + offset) * sr)
            # FLUX jitter can displace the first step before the start of the
            # render, which is not a position the reactor can fire at.
            if not 0 <= start < n:
                continue
            events.append(
                Event(
                    start=start,
                    index=k * MAX_SUB_EVENTS + s,
                    intensity=1.0 - 0.35 * (s / max(count - 1, 1)),
                    decay_scale=1.0 + p.volatility * rng.ubipolar(p.seed, 2, k, s),
                    tone=rng.urand(p.seed, 3, k, s),
                    pan=rng.ubipolar(p.seed, 4, k, s),
                )
            )

    events.sort(key=lambda e: e.start)
    return events
