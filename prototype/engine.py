"""The full SQUELCH signal path, end to end."""

from __future__ import annotations

import numpy as np

from . import output_stage, reactor
from .controls import Controls
from .params import Params
from .reactions import PROFILES


def process(
    x: np.ndarray,
    sr: int,
    p: Params,
    bpm: float,
    mix: float = 1.0,
    offline: bool = False,
    topology: str = "through",
) -> tuple[np.ndarray, Controls]:
    """ENRICHMENT -> reactor -> DRIVE -> COLLIMATOR -> FALLOUT -> mix -> safety.

    `topology` is a comparison switch while the shape of the instrument is
    being decided, not a shipping control:
      through   the source passes through the filter, which idles closed
      open      the filter idles open and comes down as REACTIVITY rises
      parallel  the source stays clean and the reaction is laid over it
    """
    # ENRICHMENT decides how hard the reaction is hit and is left at that. It
    # is not divided back out here: doing so hands DRIVE the same level
    # whatever ENRICHMENT is set to, which cancels most of what the control is
    # for. Measured, taking it out again doubled the audible effect, from
    # -15.7 dB to -8.4 dB, and the output stage holds the level anyway.
    wet, controls, md = reactor.process(
        x * p.enrichment_gain(), sr, p, bpm, open_rest=(topology == "open")
    )

    if topology == "parallel":
        wet = x + wet

    y = output_stage.process(
        wet, x, sr, p, PROFILES[p.reaction], controls, mix=mix, md=md, offline=offline
    )
    return y, controls
