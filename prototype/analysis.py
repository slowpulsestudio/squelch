"""Measured and visual validation for every render.

Listening alone is not evidence. Every sweep render gets level metrics and a
before/after spectrogram so a claim like "this got darker" can be checked.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from scipy.signal import spectrogram

#: Absolute level and share of total are kept separately on purpose: a share
#: metric saturates and cannot show a boost in a band that already dominates.
BANDS = [
    ("sub", 20.0, 120.0),
    ("low", 120.0, 500.0),
    ("mid", 500.0, 2000.0),
    ("high", 2000.0, 6000.0),
    ("air", 6000.0, 20000.0),
]


def _db(value: float) -> float:
    return 20.0 * np.log10(max(float(value), 1e-12))


def align(wet: np.ndarray, dry: np.ndarray, sr: int, max_shift_s: float = 0.02) -> int:
    """Find the sample offset of wet relative to dry, for latency compensation."""
    max_shift = int(max_shift_s * sr)
    a = dry.mean(axis=1)
    b = wet.mean(axis=1)
    n = min(len(a), len(b))
    best_shift, best_score = 0, -np.inf
    for shift in range(0, max_shift + 1):
        score = float(np.dot(a[: n - shift], b[shift:n]))
        if score > best_score:
            best_score, best_shift = score, shift
    return best_shift


def band_levels(x: np.ndarray, sr: int) -> tuple[dict, dict]:
    mono = x.mean(axis=1)
    spectrum = np.abs(np.fft.rfft(mono)) ** 2
    freqs = np.fft.rfftfreq(len(mono), 1.0 / sr)
    total = spectrum.sum() + 1e-18
    absolute, share = {}, {}
    for name, lo, hi in BANDS:
        mask = (freqs >= lo) & (freqs < hi)
        energy = float(spectrum[mask].sum())
        absolute[name] = round(_db(np.sqrt(energy / max(mask.sum(), 1))), 2)
        share[name] = round(100.0 * energy / total, 2)
    return absolute, share


def metrics(dry: np.ndarray, wet: np.ndarray, sr: int) -> dict:
    shift = align(wet, dry, sr)
    n = min(len(dry), len(wet) - shift)
    aligned_wet = wet[shift : shift + n]
    aligned_dry = dry[:n]

    difference = aligned_wet - aligned_dry
    dry_abs, dry_share = band_levels(aligned_dry, sr)
    wet_abs, wet_share = band_levels(aligned_wet, sr)

    return {
        "latency_samples": shift,
        "dry_peak_db": round(_db(np.max(np.abs(aligned_dry))), 2),
        "wet_peak_db": round(_db(np.max(np.abs(aligned_wet))), 2),
        "dry_rms_db": round(_db(np.sqrt(np.mean(aligned_dry**2))), 2),
        "wet_rms_db": round(_db(np.sqrt(np.mean(aligned_wet**2))), 2),
        # How loud the processing is relative to the delivered output: this is
        # what decides whether the effect is actually audible in the mix.
        "difference_rel_db": round(
            _db(np.sqrt(np.mean(difference**2))) - _db(np.sqrt(np.mean(aligned_wet**2))), 2
        ),
        "band_db_dry": dry_abs,
        "band_db_wet": wet_abs,
        "band_share_dry": dry_share,
        "band_share_wet": wet_share,
    }


def figure(dry: np.ndarray, wet: np.ndarray, sr: int, title: str, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shift = align(wet, dry, sr)
    n = min(len(dry), len(wet) - shift)
    aligned_wet = wet[shift : shift + n]
    aligned_dry = dry[:n]

    fig, axes = plt.subplots(3, 1, figsize=(11, 9))
    fig.suptitle(title, fontsize=9)

    for ax, sig, name in ((axes[0], aligned_dry, "dry"), (axes[1], aligned_wet, "processed")):
        f, t, s = spectrogram(sig.mean(axis=1), sr, nperseg=1024, noverlap=768)
        ax.pcolormesh(t, f, 10.0 * np.log10(s + 1e-12), shading="gouraud", vmin=-120, vmax=-20)
        ax.set_yscale("symlog", linthresh=200)
        ax.set_ylim(20, 20000)
        ax.set_ylabel(f"{name}\nHz")

    onset = int(np.argmax(np.abs(aligned_dry).mean(axis=1)))
    lo = max(onset - int(0.02 * sr), 0)
    hi = min(onset + int(0.08 * sr), n)
    time = np.arange(lo, hi) / sr
    axes[2].plot(time, aligned_dry[lo:hi].mean(axis=1), linewidth=0.7, label="dry")
    axes[2].plot(time, aligned_wet[lo:hi].mean(axis=1), linewidth=0.7, label="processed")
    axes[2].set_xlabel("seconds (zoomed on loudest transient)")
    axes[2].legend(fontsize=7)

    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
