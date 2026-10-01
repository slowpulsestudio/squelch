"""Block-rate time-varying filters.

Coefficients are recomputed once per control block (BLOCK samples) and the
filter state carries across block boundaries, which is accurate enough for fast
filter envelopes while staying fast in NumPy.
"""

from __future__ import annotations

import numpy as np
from scipy.signal import hilbert, lfilter, lfilter_zi

from . import saturation

#: Samples per control block. At 32 the envelope stepped far enough per block to
#: click audibly on short high-Q events, leaving the artefact only 4.5 dB under
#: the output peak; 8 puts it 26.8 dB under, which is inaudible.
BLOCK = 8


def n_blocks(n_samples: int) -> int:
    return int(np.ceil(n_samples / BLOCK))


def to_sample_rate(ctrl: np.ndarray, n_samples: int) -> np.ndarray:
    """Expand a control-rate array to sample rate, linearly interpolated."""
    positions = np.arange(n_samples) / BLOCK
    return np.interp(positions, np.arange(len(ctrl)), ctrl)


def smooth(ctrl: np.ndarray, time_s: float, sr: int) -> np.ndarray:
    """One-pole smoothing of a control-rate array, started at its first value."""
    if time_s <= 0.0 or len(ctrl) < 2:
        return ctrl
    coeff = float(np.exp(-1.0 / max(time_s * (sr / BLOCK), 1e-6)))
    b, a = np.array([1.0 - coeff]), np.array([1.0, -coeff])
    zi = lfilter_zi(b, a) * ctrl[0]
    return lfilter(b, a, ctrl, zi=zi)[0]


def running_rms(x: np.ndarray, sr: int, time_s: float) -> np.ndarray:
    """Level as a plugin would measure it: a one-pole on power, looking back.

    Used anywhere the offline prototype would reach for the RMS of the whole
    render. It starts settled on the opening instead of ramping up from
    silence, so the first second of a render is measured as well as the rest.
    """
    coeff = float(np.exp(-1.0 / max(time_s * sr, 1.0)))
    power = np.mean(np.atleast_2d(x.T).T ** 2, axis=1)
    start = float(np.mean(power[: max(int(0.05 * sr), 1)]))
    smoothed = lfilter([1.0 - coeff], [1.0, -coeff], power, zi=np.array([coeff * start]))
    return np.sqrt(np.maximum(smoothed[0], 1e-20))



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


def _rbj_notch(fc: float, q: float, sr: int) -> tuple[np.ndarray, np.ndarray]:
    fc = float(np.clip(fc, 20.0, sr * 0.45))
    q = max(float(q), 0.3)
    w0 = 2.0 * np.pi * fc / sr
    cos_w0, sin_w0 = np.cos(w0), np.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    b = np.array([1.0, -2.0 * cos_w0, 1.0])
    a = np.array([1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha])
    return b / a[0], a / a[0]


def _shelf_terms(fc: float, gain_db: float, sr: int) -> tuple[float, float, float, float]:
    amp = np.power(10.0, gain_db / 40.0)
    w0 = 2.0 * np.pi * float(np.clip(fc, 20.0, sr * 0.45)) / sr
    alpha = np.sin(w0) / 2.0 * np.sqrt(2.0)
    return amp, np.cos(w0), alpha, 2.0 * np.sqrt(amp) * alpha


def low_shelf(x: np.ndarray, fc: float, gain_db: float, sr: int) -> np.ndarray:
    amp, cos_w0, _, beta = _shelf_terms(fc, gain_db, sr)
    b = np.array([
        amp * ((amp + 1.0) - (amp - 1.0) * cos_w0 + beta),
        2.0 * amp * ((amp - 1.0) - (amp + 1.0) * cos_w0),
        amp * ((amp + 1.0) - (amp - 1.0) * cos_w0 - beta),
    ])
    a = np.array([
        (amp + 1.0) + (amp - 1.0) * cos_w0 + beta,
        -2.0 * ((amp - 1.0) + (amp + 1.0) * cos_w0),
        (amp + 1.0) + (amp - 1.0) * cos_w0 - beta,
    ])
    return lfilter(b / a[0], a / a[0], x, axis=0)


def high_shelf(x: np.ndarray, fc: float, gain_db: float, sr: int) -> np.ndarray:
    amp, cos_w0, _, beta = _shelf_terms(fc, gain_db, sr)
    b = np.array([
        amp * ((amp + 1.0) + (amp - 1.0) * cos_w0 + beta),
        -2.0 * amp * ((amp - 1.0) + (amp + 1.0) * cos_w0),
        amp * ((amp + 1.0) + (amp - 1.0) * cos_w0 - beta),
    ])
    a = np.array([
        (amp + 1.0) - (amp - 1.0) * cos_w0 + beta,
        2.0 * ((amp - 1.0) - (amp + 1.0) * cos_w0),
        (amp + 1.0) - (amp - 1.0) * cos_w0 - beta,
    ])
    return lfilter(b / a[0], a / a[0], x, axis=0)


def peaking(x: np.ndarray, fc: float, gain_db: float, q: float, sr: int) -> np.ndarray:
    amp = np.power(10.0, gain_db / 40.0)
    w0 = 2.0 * np.pi * float(np.clip(fc, 20.0, sr * 0.45)) / sr
    cos_w0 = np.cos(w0)
    alpha = np.sin(w0) / (2.0 * max(q, 0.1))
    b = np.array([1.0 + alpha * amp, -2.0 * cos_w0, 1.0 - alpha * amp])
    a = np.array([1.0 + alpha / amp, -2.0 * cos_w0, 1.0 - alpha / amp])
    return lfilter(b / a[0], a / a[0], x, axis=0)



def _rbj_bandpass(fc: float, q: float, sr: int) -> tuple[np.ndarray, np.ndarray]:
    fc = float(np.clip(fc, 20.0, sr * 0.45))
    q = max(float(q), 0.3)
    w0 = 2.0 * np.pi * fc / sr
    cos_w0, sin_w0 = np.cos(w0), np.sin(w0)
    alpha = sin_w0 / (2.0 * q)
    b = np.array([alpha, 0.0, -alpha])
    a = np.array([1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha])
    return b / a[0], a / a[0]


def varying_bandpass(
    x: np.ndarray, fc_ctrl: np.ndarray, q: np.ndarray | float, sr: int
) -> np.ndarray:
    """A swept resonant bandpass. Q may be a constant or a control-rate array."""
    n, ch = x.shape
    q_ctrl = q if isinstance(q, np.ndarray) else np.full(len(fc_ctrl), float(q))
    y = np.zeros_like(x)
    zi = np.zeros((2, ch))
    for i in range(len(fc_ctrl)):
        seg = x[i * BLOCK : (i + 1) * BLOCK]
        if seg.shape[0] == 0:
            break
        b, a = _rbj_bandpass(fc_ctrl[i], q_ctrl[i], sr)
        seg, zi = lfilter(b, a, seg, axis=0, zi=zi)
        y[i * BLOCK : (i + 1) * BLOCK] = seg
    return y


def varying_notch(x: np.ndarray, fc_ctrl: np.ndarray, q: float, sr: int) -> np.ndarray:
    """A swept notch — resonance inverted, which reads as hollow and submerged."""
    n, ch = x.shape
    y = np.zeros_like(x)
    zi = np.zeros((2, ch))
    for i in range(len(fc_ctrl)):
        seg = x[i * BLOCK : (i + 1) * BLOCK]
        if seg.shape[0] == 0:
            break
        b, a = _rbj_notch(fc_ctrl[i], q, sr)
        seg, zi = lfilter(b, a, seg, axis=0, zi=zi)
        y[i * BLOCK : (i + 1) * BLOCK] = seg
    return y


def pitch_wind(x: np.ndarray, wind_ctrl: np.ndarray, sr: int, max_delay_s: float) -> np.ndarray:
    """Tape/turntable wind via a delay line whose length follows the control.

    A lengthening delay drops the pitch and a shortening one raises it, so an
    envelope that rises and falls gives a wind down followed by a wind back up,
    and the delay returns to zero so the effect cannot drift out of time.
    """
    n = x.shape[0]
    delay = to_sample_rate(wind_ctrl, n) * max_delay_s * sr
    read = np.clip(np.arange(n) - delay, 0.0, n - 1.0)
    index = np.arange(n)
    out = np.empty_like(x)
    for ch in range(x.shape[1]):
        out[:, ch] = np.interp(read, index, x[:, ch])
    return out



def varying_ladder(
    x: np.ndarray,
    fc_ctrl: np.ndarray,
    q_ctrl: np.ndarray,
    sr: int,
    inner_sat: np.ndarray | float = 0.0,
) -> np.ndarray:
    """Two cascaded RBJ lowpass stages with optional saturation between them.

    Resonance is applied to the second stage only. Putting Q on both stages
    squares it into a single enormous narrow peak that swamps the rest of the
    spectrum, which is not how a 4-pole ladder behaves.

    The saturation in the middle of the cascade is what gives the resonance its
    acid grit rather than a clean analytic ring.
    """
    n, ch = x.shape
    sat = (
        inner_sat
        if isinstance(inner_sat, np.ndarray)
        else np.full(len(fc_ctrl), float(inner_sat))
    )
    y = np.zeros_like(x)
    zi = [np.zeros((2, ch)) for _ in range(2)]
    for i in range(len(fc_ctrl)):
        seg = x[i * BLOCK : (i + 1) * BLOCK]
        if seg.shape[0] == 0:
            break
        b0, a0 = _rbj_lowpass(fc_ctrl[i], 0.707, sr)
        b1, a1 = _rbj_lowpass(fc_ctrl[i], q_ctrl[i], sr)
        seg, zi[0] = lfilter(b0, a0, seg, axis=0, zi=zi[0])
        if sat[i] > 0.0:
            seg = saturation.soft_clip(seg, 1.0 + sat[i] * 6.0) / (1.0 + sat[i] * 2.0)
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
