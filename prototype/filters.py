"""Block-rate time-varying filters.

Coefficients are recomputed once per control block (BLOCK samples) and the
filter state carries across block boundaries, which is accurate enough for fast
filter envelopes while staying fast in NumPy.
"""

from __future__ import annotations

import numpy as np
from scipy.signal import hilbert, lfilter

#: Samples per control block. 32 @ 44.1kHz gives a ~1.4kHz control rate.
BLOCK = 32


def n_blocks(n_samples: int) -> int:
    return int(np.ceil(n_samples / BLOCK))


def to_sample_rate(ctrl: np.ndarray, n_samples: int) -> np.ndarray:
    """Expand a control-rate array to sample rate, linearly interpolated."""
    positions = np.arange(n_samples) / BLOCK
    return np.interp(positions, np.arange(len(ctrl)), ctrl)


def _rbj_lowpass(fc: float, q: float, sr: int) -> tuple[np.ndarray, np.ndarray]:
    fc = float(np.clip(fc, 20.0, sr * 0.45))
    q = max(float(q), 0.3)
    w0 = 2.0 * np.pi * fc / sr
    cos_w0, sin_w0 = np.cos(w0), np.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    b = np.array([(1.0 - cos_w0) / 2.0, 1.0 - cos_w0, (1.0 - cos_w0) / 2.0])
    a = np.array([1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha])
    return b / a[0], a / a[0]


def _rbj_highpass(fc: float, q: float, sr: int) -> tuple[np.ndarray, np.ndarray]:
    fc = float(np.clip(fc, 20.0, sr * 0.45))
    q = max(float(q), 0.3)
    w0 = 2.0 * np.pi * fc / sr
    cos_w0, sin_w0 = np.cos(w0), np.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    b = np.array([(1.0 + cos_w0) / 2.0, -(1.0 + cos_w0), (1.0 + cos_w0) / 2.0])
    a = np.array([1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha])
    return b / a[0], a / a[0]


def varying_ladder(
    x: np.ndarray,
    fc_ctrl: np.ndarray,
    q_ctrl: np.ndarray,
    sr: int,
    inner_sat: float = 0.0,
) -> np.ndarray:
    """Two cascaded RBJ lowpass stages with optional saturation between them.

    Resonance is applied to the second stage only. Putting Q on both stages
    squares it into a single enormous narrow peak that swamps the rest of the
    spectrum, which is not how a 4-pole ladder behaves.

    The saturation in the middle of the cascade is what gives the resonance its
    acid grit rather than a clean analytic ring.
    """
    n, ch = x.shape
    y = np.zeros_like(x)
    zi = [np.zeros((2, ch)) for _ in range(2)]
    for i in range(len(fc_ctrl)):
        seg = x[i * BLOCK : (i + 1) * BLOCK]
        if seg.shape[0] == 0:
            break
        b0, a0 = _rbj_lowpass(fc_ctrl[i], 0.707, sr)
        b1, a1 = _rbj_lowpass(fc_ctrl[i], q_ctrl[i], sr)
        seg, zi[0] = lfilter(b0, a0, seg, axis=0, zi=zi[0])
        if inner_sat > 0.0:
            seg = np.tanh(seg * (1.0 + inner_sat * 6.0)) / (1.0 + inner_sat * 2.0)
        seg, zi[1] = lfilter(b1, a1, seg, axis=0, zi=zi[1])
        y[i * BLOCK : (i + 1) * BLOCK] = seg
    return y


def static_lowpass(x: np.ndarray, fc: float, sr: int, q: float = 0.707) -> np.ndarray:
    b, a = _rbj_lowpass(fc, q, sr)
    return lfilter(b, a, x, axis=0)


def static_highpass(x: np.ndarray, fc: float, sr: int, q: float = 0.707) -> np.ndarray:
    b, a = _rbj_highpass(fc, q, sr)
    return lfilter(b, a, x, axis=0)


def varying_allpass_chain(
    x: np.ndarray,
    fc_ctrl: np.ndarray,
    sr: int,
    stages: int = 6,
    feedback: float = 0.0,
) -> np.ndarray:
    """Phaser core: a chain of first-order allpasses with a swept corner."""
    n, ch = x.shape
    y = np.zeros_like(x)
    zi = [np.zeros((1, ch)) for _ in range(stages)]
    carry = np.zeros((1, ch))
    for i in range(len(fc_ctrl)):
        seg = x[i * BLOCK : (i + 1) * BLOCK]
        if seg.shape[0] == 0:
            break
        fc = float(np.clip(fc_ctrl[i], 20.0, sr * 0.45))
        t = np.tan(np.pi * fc / sr)
        coeff = (t - 1.0) / (t + 1.0)
        b = np.array([coeff, 1.0])
        a = np.array([1.0, coeff])
        seg = seg + feedback * carry
        for s in range(stages):
            seg, zi[s] = lfilter(b, a, seg, axis=0, zi=zi[s])
        carry = seg[-1:] if seg.shape[0] else carry
        y[i * BLOCK : (i + 1) * BLOCK] = seg
    return y


def frequency_shift(x: np.ndarray, shift_ctrl: np.ndarray, sr: int) -> np.ndarray:
    """Single-sideband frequency shift via the analytic signal.

    Used instead of ring modulation, which adds a mirrored lower sideband and
    reads as harsh/metallic rather than as a swept zap.
    """
    n = x.shape[0]
    analytic = hilbert(x, axis=0)
    shift = to_sample_rate(shift_ctrl, n)
    phase = 2.0 * np.pi * np.cumsum(shift) / sr
    rotator = np.exp(1j * phase)[:, None]
    return np.real(analytic * rotator)


def octave_down(x: np.ndarray) -> np.ndarray:
    """True octave divider by halving the analytic signal's instantaneous phase."""
    mono = x.mean(axis=1)
    analytic = hilbert(mono)
    envelope = np.abs(analytic)
    phase = np.unwrap(np.angle(analytic))
    sub = envelope * np.cos(phase * 0.5)
    return np.repeat(sub[:, None], x.shape[1], axis=1)
