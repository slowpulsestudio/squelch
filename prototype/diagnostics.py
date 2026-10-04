"""dsp-testing.md's Test 19 — visual diagnostics.

The validation suite writes numbers and WAVs. Numbers say a test failed;
pictures say what KIND of failure it is, which is the thing the spec asks
for: mathematical, numerical, implementation, parameter mapping, or simply
a poor threshold. Those look different from each other and the same in a
table.

Reads `test-results/`, which `SquelchValidate` writes, and plots every
reaction that failed plus the golden renders. Run it after the suite:

    ./build/SquelchValidate
    ./.venv/bin/python -m prototype.diagnostics

Writes PNGs next to the WAVs. Needs matplotlib; if it is not installed this
says so and exits rather than failing the build, because Test 19 is
diagnostic and nothing depends on it.
"""

from __future__ import annotations

import json
import pathlib
import struct
import sys

import numpy as np

RESULTS = pathlib.Path("test-results")


def read_wav(path: pathlib.Path) -> tuple[np.ndarray, float]:
    """32-bit float stereo, which is what the suite writes.

    Parsed by hand rather than with `wave`, which refuses format tag 3
    (IEEE float) outright. The chunks are walked rather than assumed to be
    at fixed offsets, so a writer that adds a LIST or fact chunk does not
    break this.
    """
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV")

    at = 12
    channels, sr, bits, data = 2, 44100.0, 32, b""
    while at + 8 <= len(raw):
        name = raw[at : at + 4]
        size = struct.unpack("<I", raw[at + 4 : at + 8])[0]
        body = raw[at + 8 : at + 8 + size]
        if name == b"fmt ":
            _, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            sr = float(rate)
        elif name == b"data":
            data = body
        at += 8 + size + (size & 1)

    if bits != 32:
        raise ValueError(f"{path} is {bits}-bit; this expects 32-bit float")

    samples = np.frombuffer(data, dtype="<f4").astype(np.float64)
    return samples.reshape(-1, channels), sr


def correlation_over_time(x: np.ndarray, frame: int = 4096) -> tuple[np.ndarray, np.ndarray]:
    """Stereo correlation per frame. A single number for the whole render
    hides the thing worth seeing, which is whether the image is moving."""
    centres, values = [], []
    for at in range(0, len(x) - frame, frame // 2):
        block = x[at : at + frame]
        left, right = block[:, 0] - block[:, 0].mean(), block[:, 1] - block[:, 1].mean()
        denominator = np.sqrt((left**2).sum() * (right**2).sum())
        values.append(float((left * right).sum() / denominator) if denominator > 1e-18 else 0.0)
        centres.append(at + frame / 2)
    return np.asarray(centres), np.asarray(values)


def peak_trajectory(
    x: np.ndarray, sr: float, frame: int = 4096, lo_hz: float = 40.0, hi_hz: float = 8000.0
) -> tuple[np.ndarray, np.ndarray]:
    """Where the strongest component sits, frame by frame. This is the
    resonant-frequency trajectory for a resonator and the oscillator
    trajectory for ALIEN; both are the same measurement.

    Banded, because these are raw engine outputs taken before the output
    stage and several of them carry far more energy below 20 Hz than at
    their own resonance. An unbanded argmax reports the lowest bin for
    every frame and draws a flat line that looks like a pinned resonator.
    """
    centres, values = [], []
    window = np.hanning(frame)
    freqs = np.fft.rfftfreq(frame, 1.0 / sr)
    band = (freqs >= lo_hz) & (freqs <= hi_hz)
    for at in range(0, len(x) - frame, frame // 2):
        spectrum = np.abs(np.fft.rfft(x[at : at + frame, 0] * window))
        values.append(float(freqs[band][np.argmax(spectrum[band])]))
        centres.append(at + frame / 2)
    return np.asarray(centres), np.asarray(values)


def plot(path: pathlib.Path, title: str) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    x, sr = read_wav(path)
    t = np.arange(len(x)) / sr

    fig, axes = plt.subplots(3, 2, figsize=(15, 11))
    fig.suptitle(f"{title} — {path.name}", fontsize=13)

    axes[0][0].plot(t, x[:, 0], lw=0.4, color="tab:blue")
    axes[0][0].plot(t, x[:, 1], lw=0.4, color="tab:orange", alpha=0.7)
    axes[0][0].set_title("waveform")
    axes[0][0].set_xlabel("s")

    axes[0][1].specgram(x[:, 0], NFFT=2048, Fs=sr, noverlap=1024, cmap="magma")
    axes[0][1].set_title("spectrogram")
    axes[0][1].set_ylabel("Hz")
    axes[0][1].set_yscale("symlog", linthresh=100)

    # Magnitude spectrum of the steady part, past any onset transient.
    window = np.hanning(min(32768, len(x)))
    segment = x[len(x) // 4 : len(x) // 4 + len(window), 0] * window
    mag = np.abs(np.fft.rfft(segment))
    freqs = np.fft.rfftfreq(len(window), 1.0 / sr)
    axes[1][0].semilogx(freqs[1:], 20 * np.log10(np.maximum(mag[1:], 1e-12)), lw=0.6)
    axes[1][0].set_title("magnitude spectrum")
    axes[1][0].set_xlabel("Hz")
    axes[1][0].set_ylabel("dB")
    axes[1][0].grid(alpha=0.3)

    centres, corr = correlation_over_time(x)
    axes[1][1].plot(centres / sr, corr, lw=0.8)
    axes[1][1].set_ylim(-1.05, 1.05)
    axes[1][1].axhline(1.0, color="grey", lw=0.5)
    axes[1][1].axhline(0.0, color="grey", lw=0.5)
    axes[1][1].set_title("stereo correlation (1 = mono, 0 = uncorrelated)")
    axes[1][1].set_xlabel("s")

    centres, peaks = peak_trajectory(x, sr)
    axes[2][0].plot(centres / sr, peaks, lw=0.8, color="tab:red")
    axes[2][0].set_title("peak-frequency trajectory")
    axes[2][0].set_xlabel("s")
    axes[2][0].set_ylabel("Hz")
    axes[2][0].grid(alpha=0.3)

    # Envelope, on a dB scale, because a decay is a straight line there and
    # a curve anywhere else.
    envelope = np.abs(x[:, 0])
    smoothed = np.convolve(envelope, np.ones(512) / 512, mode="same")
    axes[2][1].plot(t, 20 * np.log10(np.maximum(smoothed, 1e-9)), lw=0.6)
    axes[2][1].set_title("envelope")
    axes[2][1].set_xlabel("s")
    axes[2][1].set_ylabel("dB")
    axes[2][1].grid(alpha=0.3)

    fig.tight_layout()
    out = path.with_suffix(".png")
    fig.savefig(out, dpi=110)
    plt.close(fig)
    print(f"  {out}")


def main() -> int:
    try:
        import matplotlib  # noqa: F401
    except ImportError:
        print("matplotlib is not installed, so there is nothing to plot.")
        print("  ./.venv/bin/pip install matplotlib")
        return 0

    if not RESULTS.exists():
        print("No test-results/. Run ./build/SquelchValidate first.")
        return 1

    summary = RESULTS / "summary.json"
    if summary.exists():
        report = json.loads(summary.read_text())
        failures = [t for t in report["tests"] if t["outcome"] == "FAIL"]
        warnings = [t for t in report["tests"] if t["outcome"] == "WARN"]
        print(f"{report['verdict']}: {report['passed']} passed, "
              f"{report['failed']} failed, {report['warnings']} warnings\n")
        for t in failures + warnings:
            print(f"  [{t['outcome']}] {t['test']}: {t['detail']}")
        print()

    wavs = sorted(RESULTS.glob("*.wav"))
    if not wavs:
        print("No WAVs to plot.")
        return 0

    print("Plotting:")
    for path in wavs:
        plot(path, "failure" if path.name.startswith("failure-") else "golden render")
    return 0


if __name__ == "__main__":
    sys.exit(main())
