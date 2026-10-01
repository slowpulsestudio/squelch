"""Render MELTDOWN and IONIZE examples against every input file.

The round sweeps deliberately leave the performative controls alone, since they
are excluded from Randomise. This renders them instead: each gesture against
randomly sampled creative settings, so they are heard in varied contexts rather
than against one tuned backdrop.

Everything lands in one flat folder, ordered source then gesture, so it can be
listened through in sequence.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from . import analysis, audio_io, engine, rng
from .params import CONTINUOUS, GRID_NAMES, MODES, REACTIONS, Params

#: MELTDOWN fires two bars in and is held for varying lengths, so the short
#: taps that never reach the later stages can be heard against the long holds.
MELTDOWN_BAR = 2
HOLDS = (0.5, 1.1, 1.9)

GESTURES = ("IONIZE", "MELTDOWN", "BOTH")


def _creative(seed: int, index: int) -> dict:
    """Random creative settings, kept off the extremes so the gesture reads."""
    values = {}
    for i, name in enumerate(CONTINUOUS):
        values[name] = 0.15 + 0.7 * rng.urand(seed, 60, index, i)
    # A reactor already running flat out has nowhere to go when it melts down.
    values["containment"] = 0.25 + 0.45 * rng.urand(seed, 61, index)
    values["toxicity"] = 0.2 + 0.4 * rng.urand(seed, 62, index)
    values["afterglow"] = 0.35 + 0.5 * rng.urand(seed, 63, index)
    values["ionize_amount"] = 0.75 + 0.25 * rng.urand(seed, 67, index)
    return values


def run(sources: list[Path], out_dir: Path, bpm: float, per_gesture: int, seed: int) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    bar = 4.0 * 60.0 / bpm
    manifest = {"bpm": bpm, "renders": []}
    number = 0

    for source in sources:
        dry, sr = audio_io.load(source)
        tag = source.stem.replace("AT_BT_140_", "")
        audio_io.save(out_dir / f"00_{tag}_DRY.wav", dry, sr)

        for gesture in GESTURES:
            for variant in range(per_gesture):
                number += 1
                index = number
                # Reaction cycles rather than being drawn at random: with this
                # few renders a random draw leaves some reactions unheard.
                reaction = REACTIONS[(number - 1) % len(REACTIONS)]
                mode = MODES[int(rng.urand(seed, 65, index) * len(MODES))]
                grid = GRID_NAMES[int(rng.urand(seed, 66, index) * len(GRID_NAMES))]
                hold = HOLDS[variant % len(HOLDS)]

                p = Params(
                    reaction=reaction,
                    mode=mode,
                    grid=grid,
                    seed=index,
                    ionize=gesture in ("IONIZE", "BOTH"),
                    meltdown_at=MELTDOWN_BAR * bar if gesture in ("MELTDOWN", "BOTH") else -1.0,
                    meltdown_hold=hold if gesture in ("MELTDOWN", "BOTH") else 0.0,
                    **_creative(seed, index),
                )

                stem = f"{number:02d}_{tag}_{gesture}_{reaction.replace(' ', '-')}"
                wet, _ = engine.process(dry, sr, p, bpm)
                audio_io.save(out_dir / f"{stem}.wav", wet, sr)

                measured = analysis.metrics(dry, wet, sr)
                manifest["renders"].append(
                    {"file": f"{stem}.wav", "source": source.name,
                     "gesture": gesture, "params": p.summary(), "metrics": measured}
                )
                held = f"held {hold:.1f}s" if p.meltdown_hold else "latched"
                print(f"  {stem:44s} {mode:6s} {grid:5s} {held}")

    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"\n{number} renders -> {out_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("Input"))
    parser.add_argument("--out", type=Path, default=Path("Output/gestures"))
    parser.add_argument("--bpm", type=float, default=140.0)
    parser.add_argument("--per-gesture", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    sources = sorted(args.input.glob("*.wav"))
    if not sources:
        raise SystemExit(f"no .wav files in {args.input}")
    run(sources, args.out, args.bpm, args.per_gesture, args.seed)


if __name__ == "__main__":
    main()
