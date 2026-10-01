# Project-specific agent instructions

## Source of truth

There is no Figma file for this project.

[prompt.md](prompt.md) is the original brief and is kept **as a record only**. It is
not maintained, and several parameters have been renamed, added or removed since it
was written. Where it and the code disagree, the code is right.

The live spec is [prototype/params.py](prototype/params.py), with the measured
behaviour of every stage pinned by [prototype/checks.py](prototype/checks.py). The
C++ reads its parameter list from [Source/Parameters.h](Source/Parameters.h), which
mirrors it.

Canonical parameter names, as they stand:

- Reactions: RADIATION, FISSION, SLUDGE, CHEMICAL, ALIEN
- Structure: MODE, GRID, SEED
- Continuous: ENRICHMENT, FLUX, PROBABILITY, REACTIVITY, VOLATILITY, HALF-LIFE,
  DECAY, SPREAD, TOXICITY, CONTAINMENT, DRIVE, CONTAMINATION, EXPOSURE,
  COLLIMATOR, FALLOUT, AFTERGLOW, IONIZE AMOUNT
- Gestures: IONIZE (latched), MELTDOWN (momentary)
- Output: CLIP

Renamed since prompt.md: RANGE became SPREAD, SQUELCH became TOXICITY, RODS became
CONTAINMENT. Added since: ENRICHMENT, CONTAMINATION, AFTERGLOW, IONIZE, MELTDOWN,
CLIP. BEAKER became CHEMICAL and TOXIC SLUDGE became SLUDGE.

Distinctions worth keeping straight:

- PROBABILITY = *whether* a reaction happens; REACTIVITY = *how much* happens when
  it does.
- HALF-LIFE = persistence of a reaction's influence on *future* reactions; DECAY =
  how long the *current* reaction stays audible.
- TOXICITY is the main character macro, not a wet/dry or output gain.
- EXPOSURE replaces the conventional name RESONANCE.
- ENRICHMENT is a plugin parameter, not the Input strip. The saturation has a fixed
  threshold, so input level is part of the character; a preset that did not recall
  it would not recall the sound. The Input strip stays a utility trim and stays out
  of presets.

## Porting the prototype

The Python prototype in `prototype/` is the reference implementation. It is causal
throughout: nothing measures audio that has not played yet, which is what makes it
portable. `prototype/checks.py` is the contract the C++ has to meet, not a loose
guide — if a stage measures differently in C++, the port is wrong.

Two things in the prototype are offline conveniences and must not be copied:
`match_rms` (kept behind `--offline` purely for A/B rendering) and anything that
indexes a whole render at once. The shipping path uses `filters.running_rms`.

<!-- Add project-specific notes here — design decisions, constraints, what's
     being tested, known issues, personas, edge cases, anything the AI should
     know about this particular project that isn't covered by master-skills.md.
     This file is never overwritten by /skill-me-up. -->
