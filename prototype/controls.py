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
    #: Per-channel placement gain, held across each event. Kept separate from
    #: the amplitude envelope: folded into it, an event could only ever be a
    #: few dB louder on one side, never actually thrown across the field.
    pan_gain: np.ndarray
    #: How much of each event is sent into AFTERGLOW. Uniform unless IONIZE is
    #: scattering depth, in which case every event sits at its own distance.
    send: np.ndarray
    #: Filter cutoff in Hz, one value per control block.
    cutoff: np.ndarray
    #: Filter Q, one value per control block.
    resonance: np.ndarray
    #: Per-event start samples, for reactions that key off the attack itself.
    starts: np.ndarray
    #: Global suppression from CONTAINMENT, 0..1, where 1 means fully damped.
    damping: float
