"""MELTDOWN: a momentary performance gesture that takes the reactor critical.

Each parameter has its own staged envelope rather than everything snapping at
once, and the order follows the metaphor. The rods come out first because that
is what causes a meltdown; the fallout arrives last and outlives everything
else, still hanging in the air after the reaction itself has settled.

It is a performance control: never stored in a preset, never jittered by
Randomise.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import filters
from .params import Params


@dataclass(frozen=True)
class Stage:
    #: Seconds after the gate opens before this parameter starts moving.
    delay: float
    #: Seconds to travel from its knob position to its critical value.
    attack: float
    #: Seconds to fall back after the gate closes.
    release: float
    #: Where the parameter goes at full meltdown.
    target: float


#: Ordered by when each one arrives. CONTAINMENT descends from wherever the
#: Designer left it rather than being forced instantly, so the gesture reads as
#: the rods sliding out rather than a switch being thrown.
STAGES = {
    "containment": Stage(0.00, 0.10, 1.40, 0.00),
    "probability": Stage(0.02, 0.18, 0.90, 1.00),
    "spread": Stage(0.03, 0.50, 1.80, 1.00),
    "drive": Stage(0.06, 0.26, 1.20, 1.00),
    "reactivity": Stage(0.10, 0.30, 1.10, 0.85),
    "toxicity": Stage(0.20, 0.36, 1.60, 1.00),
    "exposure": Stage(0.42, 0.44, 2.10, 1.00),
    "contamination": Stage(0.66, 0.75, 3.40, 1.00),
}


class Meltdown:
    """The effective value of every staged parameter over the whole render."""

    def __init__(self, p: Params, n_samples: int, sr: int) -> None:
        self._params = p
        self._sr = sr
        self._n = n_samples
        self._blocks = filters.n_blocks(n_samples)
        self.active = p.meltdown_hold > 0.0 and p.meltdown_at >= 0.0
        self._ctrl: dict[str, np.ndarray] = {}

        if self.active:
            time = np.arange(self._blocks) * filters.BLOCK / sr
            closed = p.meltdown_at + p.meltdown_hold
            for name, stage in STAGES.items():
                self._ctrl[name] = self._stage_values(name, stage, time, closed)

    def _stage_values(
        self, name: str, stage: Stage, time: np.ndarray, closed: float
    ) -> np.ndarray:
        opened = self._params.meltdown_at + stage.delay
        rising = np.clip((time - opened) / max(stage.attack, 1e-6), 0.0, 1.0)

        # A short tap never reaches the later stages, which is the point: how
        # far the reaction gets depends on how long it is held.
        reached = float(np.clip((closed - opened) / max(stage.attack, 1e-6), 0.0, 1.0))
        falling = reached * np.exp(-np.maximum(time - closed, 0.0) / (stage.release / 3.0))

        progress = np.where(time < closed, rising, falling)
        base = getattr(self._params, name)
        return base + (stage.target - base) * progress

    def ctrl(self, name: str) -> np.ndarray:
        """Effective value per control block."""
        if name not in self._ctrl:
            return np.full(self._blocks, float(getattr(self._params, name)))
        return self._ctrl[name]

    def at(self, name: str, sample: int) -> float:
        """Effective value at one moment, for per-event decisions."""
        if name not in self._ctrl:
            return float(getattr(self._params, name))
        index = min(max(sample // filters.BLOCK, 0), self._blocks - 1)
        return float(self._ctrl[name][index])

    def samples(self, name: str) -> np.ndarray:
        """Effective value per sample, for stages that process whole buffers."""
        if name not in self._ctrl:
            return np.full(self._n, float(getattr(self._params, name)))
        return filters.to_sample_rate(self._ctrl[name], self._n)
