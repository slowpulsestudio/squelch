# Project-specific agent instructions

## Source of truth

There is no Figma file for this project. The design and behavioural spec lives in
[prompt.md](prompt.md) — the SQUELCH concept & parameter metaprompt. Treat it as the
authoritative reference for concept, sonic identity, parameter names, parameter
semantics, and the conceptual hierarchy.

Canonical parameter names are fixed by that document and must be used verbatim in
code, the APVTS parameter IDs/labels, and the UI: REACTION, MODE, GRID, FLUX,
PROBABILITY, REACTIVITY, VOLATILITY, HALF-LIFE, DECAY, RANGE, SQUELCH, RODS, DRIVE,
EXPOSURE, COLLIMATOR, FALLOUT.

Note the distinctions the spec calls out explicitly:
- PROBABILITY = *whether* a reaction happens; REACTIVITY = *how much* happens when it does.
- HALF-LIFE = persistence of a reaction's influence on *future* reactions; DECAY = how long the *current* reaction stays audible.
- SQUELCH is the main character macro, not a wet/dry or output gain.
- EXPOSURE replaces the conventional name RESONANCE.

<!-- Add project-specific notes here — design decisions, constraints, what's
     being tested, known issues, personas, edge cases, anything the AI should
     know about this particular project that isn't covered by master-skills.md.
     This file is never overwritten by /skill-me-up. -->
