"""Round 1 sweep: random full parameter sets across the full plausible range.

Full sets are sampled together rather than one parameter at a time, and every
candidate lands in one flat folder so the renders can be listened through in
sequence. The ranges and defaults for the real plugin come from feedback on
these, not from a guess.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import replace
from pathlib import Path

from . import analysis, audio_io, engine, rng
from .params import CONTINUOUS, GRID_NAMES, MODES, REACTIONS, Params

#: Deliberately calm and deliberately extreme anchors, so every reaction is
#: heard at both ends of its travel and not only in the middle.
QUIET_ANCHOR = dict(
    flux=0.0, probability=0.35, reactivity=0.1, volatility=0.1, half_life=0.0,
    decay=0.15, range=0.15, squelch=0.2, rods=0.55, drive=0.1, contamination=0.2,
    exposure=0.25, collimator=0.0, fallout=0.15,
)

VIOLENT_ANCHOR = dict(
    flux=0.7, probability=1.0, reactivity=1.0, volatility=0.9, half_life=0.6,
    decay=0.8, range=1.0, squelch=1.0, rods=0.0, drive=0.85, contamination=0.8,
    exposure=1.0, collimator=0.35, fallout=0.9,
)


def _random_params(reaction: str, seed: int, index: int) -> Params:
    # The reaction is part of every draw, otherwise each reaction gets an
    # identical sequence of settings and the sweep only explores a handful of
    # distinct combinations.
    r = REACTIONS.index(reaction)
    values = {name: rng.urand(seed, 10, r, index, i) for i, name in enumerate(CONTINUOUS)}
    mode = MODES[int(rng.urand(seed, 11, r, index) * len(MODES))]
    grid = GRID_NAMES[int(rng.urand(seed, 12, r, index) * len(GRID_NAMES))]
    return Params(reaction=reaction, mode=mode, grid=grid, seed=index, **values)


def variants(reaction: str, count: int, seed: int) -> list[Params]:
    """Both extremes plus random full sets in between.

    The two anchors are identical across reactions on purpose, so the character
    of each reaction can be compared at matched settings.
    """
    base = Params(reaction=reaction, mode="GRID", grid="1/16", seed=0)
    out = [replace(base, **QUIET_ANCHOR, seed=0)]
    for i in range(1, count - 1):
        out.append(_random_params(reaction, seed, i))
    out.append(replace(base, **VIOLENT_ANCHOR, seed=count))
    return out[:count]


def run(source: Path, out_dir: Path, bpm: float, per_reaction: int, seed: int) -> None:
    dry, sr = audio_io.load(source)
    out_dir.mkdir(parents=True, exist_ok=True)
    analysis_dir = out_dir / "analysis"

    audio_io.save(out_dir / "00_DRY.wav", dry, sr)

    manifest = {"source": source.name, "bpm": bpm, "sample_rate": sr, "renders": []}
    number = 0

    for reaction in REACTIONS:
        for p in variants(reaction, per_reaction, seed):
            number += 1
            stem = f"{number:02d}_{reaction.replace(' ', '-')}"
            wet, _ = engine.process(dry, sr, p, bpm)
            audio_io.save(out_dir / f"{stem}.wav", wet, sr)

            measured = analysis.metrics(dry, wet, sr)
            analysis.figure(dry, wet, sr, f"{stem}  {p.mode} {p.grid}", analysis_dir / f"{stem}.png")

            manifest["renders"].append({"file": f"{stem}.wav", "params": p.summary(), "metrics": measured})
            print(
                f"{stem:28s} {p.mode:6s} {p.grid:5s} "
                f"effect {measured['difference_rel_db']:+6.1f} dB rel  "
                f"peak {measured['wet_peak_db']:+6.1f} dBFS"
            )

    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"\n{number} renders -> {out_dir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("Input/AT_BT_140_Synth_01_A.wav"))
    parser.add_argument("--out", type=Path, default=Path("Output/round1"))
    parser.add_argument("--bpm", type=float, default=140.0)
    parser.add_argument("--per-reaction", type=int, default=5)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    run(args.source, args.out, args.bpm, args.per_reaction, args.seed)


if __name__ == "__main__":
    main()
