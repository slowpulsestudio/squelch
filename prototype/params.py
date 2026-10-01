"""The canonical SQUELCH parameters.

Names and semantics come from prompt.md, which is the authoritative spec.
Continuous parameters are normalised 0..1 here deliberately: prompt.md does not
specify real-world ranges or defaults, and those are meant to be fixed from
Designer feedback on Round 1 sweep renders rather than invented up front.
"""

from __future__ import annotations

from dataclasses import dataclass, fields

REACTIONS = ["RADIATION", "FISSION", "SLUDGE", "CHEMICAL", "ALIEN"]

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

#: ENRICHMENT spans this many dB either side of unity.
ENRICHMENT_RANGE_DB = 18.0

#: Continuous parameters, in the order prompt.md lists them.
CONTINUOUS = [
    "enrichment",
    "flux",
    "probability",
    "reactivity",
    "volatility",
    "half_life",
    "decay",
    "spread",
    "toxicity",
    "containment",
    "drive",
    "contamination",
    "exposure",
    "collimator",
    "fallout",
    "afterglow",
    "ionize_amount",
]

#: Momentary or latched performance gestures. Excluded from Randomise by
#: default. MELTDOWN is momentary so it is not stored either; IONIZE is latched,
#: so it is a state a preset can recall.
PERFORMATIVE = [
    "meltdown",
    "ionize",
]


@dataclass
class Params:
    """A full SQUELCH setting. Continuous values are normalised 0..1."""

    reaction: str = "RADIATION"
    mode: str = "GRID"
    grid: str = "1/16"

    #: How hard the source is fed into the reactor, 0.5 being unity. The drive
    #: curve has a fixed knee, so level is part of the character rather than
    #: just gain staging, and a preset that did not recall it would not recall
    #: the sound. That is why this is a plugin parameter and not the host-side
    #: Input strip, which stays a utility trim and stays out of presets.
    enrichment: float = 0.5

    flux: float = 0.0
    probability: float = 1.0
    reactivity: float = 0.3
    volatility: float = 0.3
    half_life: float = 0.0
    decay: float = 0.3
    spread: float = 0.4
    toxicity: float = 0.5
    containment: float = 0.0
    drive: float = 0.3
    #: Noise/grain emitted by the reaction. Its texture is set by the REACTION.
    contamination: float = 0.25
    exposure: float = 0.5
    collimator: float = 0.0
    fallout: float = 0.3
    #: How long the reaction keeps glowing after it has happened.
    afterglow: float = 0.0

    #: IONIZE is latched: the toggle decides whether events scatter in stereo,
    #: spectrum and depth at all, and the amount decides how far. Depth is
    #: scattered within whatever AFTERGLOW is set to, so at zero glow it still
    #: scatters the other two.
    ionize: bool = False
    ionize_amount: float = 0.7

    #: Output-section toggle: a hard ceiling instead of the lookahead limiter.
    #: Lives with the output controls, so like them it is kept out of presets
    #: and out of Randomise.
    clip: bool = False

    #: MELTDOWN is momentary: held from meltdown_at for meltdown_hold seconds.
    #: Negative start or zero hold means it never fires.
    meltdown_at: float = -1.0
    meltdown_hold: float = 0.0

    seed: int = 0

    def enrichment_gain(self) -> float:
        """ENRICHMENT as a linear gain, 0.5 being unity."""
        return float(
            10.0 ** (((self.enrichment - 0.5) * 2.0 * ENRICHMENT_RANGE_DB) / 20.0)
        )

    def label(self) -> str:
        parts = [self.reaction.replace(" ", "-"), self.mode, self.grid.replace("/", "-")]
        parts += [f"{name[:4]}{int(round(getattr(self, name) * 100)):03d}" for name in CONTINUOUS]
        parts.append(f"seed{self.seed}")
        return "_".join(parts)

    def summary(self) -> dict:
        return {f.name: getattr(self, f.name) for f in fields(self)}
