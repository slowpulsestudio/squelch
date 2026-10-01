"""The 16 canonical SQUELCH parameters.

Names and semantics come from prompt.md, which is the authoritative spec.
Continuous parameters are normalised 0..1 here deliberately: prompt.md does not
specify real-world ranges or defaults, and those are meant to be fixed from
Designer feedback on Round 1 sweep renders rather than invented up front.
"""

from __future__ import annotations

from dataclasses import dataclass, fields

REACTIONS = ["RADIATION", "FISSION", "TOXIC SLUDGE", "BEAKER", "ALIEN"]

MODES = ["GRID", "RANDOM", "FREE", "INPUT"]

# Division name -> length in beats.
GRID_DIVISIONS = {
    "1/1": 4.0,
    "1/2": 2.0,
    "1/4": 1.0,
    "1/4D": 1.5,
    "1/4T": 2.0 / 3.0,
    "1/8": 0.5,
    "1/8D": 0.75,
    "1/8T": 1.0 / 3.0,
    "1/16": 0.25,
    "1/16D": 0.375,
    "1/16T": 1.0 / 6.0,
    "1/32": 0.125,
}

GRID_NAMES = list(GRID_DIVISIONS)

#: Continuous parameters, in the order prompt.md lists them.
CONTINUOUS = [
    "flux",
    "probability",
    "reactivity",
    "volatility",
    "half_life",
    "decay",
    "range",
    "squelch",
    "containment",
    "drive",
    "contamination",
    "exposure",
    "collimator",
    "fallout",
]


@dataclass
class Params:
    """A full SQUELCH setting. Continuous values are normalised 0..1."""

    reaction: str = "RADIATION"
    mode: str = "GRID"
    grid: str = "1/16"

    flux: float = 0.0
    probability: float = 1.0
    reactivity: float = 0.3
    volatility: float = 0.3
    half_life: float = 0.0
    decay: float = 0.3
    range: float = 0.4
    squelch: float = 0.5
    containment: float = 0.0
    drive: float = 0.3
    #: Noise/grain emitted by the reaction. Its texture is set by the REACTION.
    contamination: float = 0.25
    exposure: float = 0.5
    collimator: float = 0.0
    fallout: float = 0.3

    seed: int = 0

    def label(self) -> str:
        parts = [self.reaction.replace(" ", "-"), self.mode, self.grid.replace("/", "-")]
        parts += [f"{name[:4]}{int(round(getattr(self, name) * 100)):03d}" for name in CONTINUOUS]
        parts.append(f"seed{self.seed}")
        return "_".join(parts)

    def summary(self) -> dict:
        return {f.name: getattr(self, f.name) for f in fields(self)}
