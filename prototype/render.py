"""Render one SQUELCH setting, for ad-hoc A/B checks.

Output always lands in a named subfolder of Output/, never in its root.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from . import analysis, audio_io, engine
from .params import CONTINUOUS, GRID_NAMES, MODES, REACTIONS, Params


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("Input/AT_BT_140_Synth_01_A.wav"))
    parser.add_argument("--name", required=True, help="subfolder under Output/")
    parser.add_argument("--bpm", type=float, default=140.0)
    parser.add_argument("--mix", type=float, default=1.0)
    parser.add_argument("--reaction", choices=REACTIONS, default="RADIATION")
    parser.add_argument("--mode", choices=MODES, default="GRID")
    parser.add_argument("--grid", choices=GRID_NAMES, default="1/16")
    parser.add_argument("--seed", type=int, default=0)
    for name in CONTINUOUS:
        parser.add_argument(f"--{name.replace('_', '-')}", type=float, default=None)
    args = parser.parse_args()

    overrides = {n: getattr(args, n) for n in CONTINUOUS if getattr(args, n) is not None}
    p = Params(
        reaction=args.reaction, mode=args.mode, grid=args.grid, seed=args.seed, **overrides
    )

    dry, sr = audio_io.load(args.source)
    wet, _ = engine.process(dry, sr, p, args.bpm, mix=args.mix)

    out_dir = Path("Output") / args.name
    audio_io.save(out_dir / f"{args.name}.wav", wet, sr)
    audio_io.save(out_dir / f"{args.name}_dry.wav", dry, sr)

    measured = analysis.metrics(dry, wet, sr)
    analysis.figure(dry, wet, sr, args.name, out_dir / f"{args.name}.png")
    (out_dir / f"{args.name}.json").write_text(
        json.dumps({"params": p.summary(), "metrics": measured}, indent=2)
    )
    print(json.dumps(measured, indent=2))
    print(f"\n-> {out_dir}")


if __name__ == "__main__":
    main()
