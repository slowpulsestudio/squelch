"""The control-rate signals a reaction is driven by.

Shared between the reactor (which builds them) and the reaction profiles (which
read them), so neither has to import the other.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass
class Controls:
    #: Combined event envelope, 0..1, one value per control block.
    env: np.ndarray
    #: The same envelope split per channel by each event's stereo position.
    env_stereo: np.ndarray
    #: Filter cutoff in Hz, one value per control block.
    cutoff: np.ndarray
    #: Filter Q, one value per control block.
    resonance: np.ndarray
    #: Per-event start samples, for reactions that key off the attack itself.
    starts: np.ndarray
    #: Global suppression from RODS, 0..1, where 1 means fully damped.
    damping: float
