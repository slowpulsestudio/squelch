"""The full SQUELCH signal path, end to end."""

from __future__ import annotations

import numpy as np

from . import output_stage, reactor
from .controls import Controls
from .params import Params
from .reactions import PROFILES


def process(
    x: np.ndarray, sr: int, p: Params, bpm: float, mix: float = 1.0
) -> tuple[np.ndarray, Controls]:
    """FEED -> reactor -> DRIVE -> COLLIMATOR -> FALLOUT -> mix -> peak safety."""
    # FEED drives the reaction only. The dry reference stays at its original
    # level so the MIX blend and the output level match do not move with it,
    # which leaves FEED changing character rather than loudness.
    wet, controls, md = reactor.process(x * p.feed_gain(), sr, p, bpm)
    y = output_stage.process(wet, x, sr, p, PROFILES[p.reaction], controls, mix=mix, md=md)
    return y, controls
