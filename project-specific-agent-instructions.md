# Project-specific agent instructions

## What a failure looks like

Making assumptions on design direction that simplify the implementation, without
asking first.

This is the top-priority failure mode for this project. It is a failure of task
regardless of how well the resulting code is written, measured or tested.

It has happened in these concrete forms, all of which must not recur:

- Collapsing five specified effects into one algorithm with five coefficient
  sets, because one algorithm is easier to build and tune. The brief asks for
  five processes — splitting, subharmonics, bubbling, FM/AM bursts — none of
  which are filter settings.
- Disconnecting specified behaviour during a refactor because it did not fit the
  new structure, then describing it as "orphaned code" and listing it for
  cleanup. Code is not orphaned if the brief asks for it; it is unfinished.
- Editing the brief so it agrees with the implementation. The brief constrains
  the code. If a specified feature looks unreachable, say so and ask — do not
  write the limitation into the spec as though it were a design principle.
- Declaring something done or distinct on the strength of a measurement that
  could not have detected the defect. Each reaction measured against dry only
  proves it does something; it cannot show whether two reactions are the same
  effect. Measure the thing that is actually claimed.

When a simplification looks necessary, the required action is to stop and ask,
with the cost stated plainly. Not to pick the simpler path and report success.

## Source of truth

There is no Figma file for this project.

[README.md](README.md) states what each of the five reactions has to do. The code
does not override it: if a reaction does not yet do what README.md describes, the
reaction is unfinished, not the document. Do not edit README.md to match the
implementation.

[dsp-maths.md](dsp-maths.md) holds the transfer functions, each under a heading naming the
reaction it belongs to. A reaction with no maths there is not yet specified, and
the gap is the work, not something to fill by reusing another reaction's engine.

Parameter names and ranges are in [prototype/params.py](prototype/params.py), with
the measured behaviour of every stage pinned by
[prototype/checks.py](prototype/checks.py). The C++ reads its parameter list from
[Source/Parameters.h](Source/Parameters.h), which mirrors it. Several parameters
have been renamed since the brief was written; for names, the code is current.

Canonical parameter names, as they stand:

- Reactions: RADIATION, FISSION, SLUDGE, CHEMICAL, ALIEN
- Structure: MODE, GRID, SEED
- Continuous: ENRICHMENT, FLUX, PROBABILITY, REACTIVITY, VOLATILITY, HALF-LIFE,
  DECAY, SPREAD, TOXICITY, CONTAINMENT, DRIVE, CONTAMINATION, EXPOSURE,
  COLLIMATOR, FALLOUT, AFTERGLOW, IONIZE AMOUNT
- Gestures: IONIZE (latched), MELTDOWN (momentary)
- Output: CLIP

Renamed since README.md: RANGE became SPREAD, SQUELCH became TOXICITY, RODS became
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
