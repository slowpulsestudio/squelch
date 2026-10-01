"""The full SQUELCH signal path, end to end."""

from __future__ import annotations

import numpy as np

from . import output_stage, reactor
from .controls import Controls
from .params import Params
from .reactions import PROFILES


def process(
    x: np.ndarray, sr: int, p: Params, bpm: float, mix: float = 1.0, offline: bool = False
) -> tuple[np.ndarray, Controls]:
    """ENRICHMENT -> reactor -> DRIVE -> COLLIMATOR -> FALLOUT -> mix -> safety."""
    # ENRICHMENT drives the reaction only. The dry reference stays at its
    # original level so the MIX blend and the output level match do not move
    # with it, leaving ENRICHMENT to change character rather than loudness.
    wet, controls, md = reactor.process(x * p.enrichment_gain(), sr, p, bpm)
    y = output_stage.process(
        wet, x, sr, p, PROFILES[p.reaction], controls, mix=mix, md=md, offline=offline
    )
    return y, controls
