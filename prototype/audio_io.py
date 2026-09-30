"""Audio file load/save for the prototype harness."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf


def load(path: str | Path) -> tuple[np.ndarray, int]:
    """Return (samples as float64 shaped (N, 2), sample rate)."""
    x, sr = sf.read(str(path), always_2d=True, dtype="float64")
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2], sr


def save(path: str | Path, x: np.ndarray, sr: int) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(path), x, sr, subtype="PCM_24")
