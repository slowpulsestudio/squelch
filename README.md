# SQUELCH — Concept, Architecture & Parameters

> What the plugin has to do. The transfer functions are in
> [maths.md](maths.md). Where this document and the code disagree, the code is
> unfinished.

## Core concept

SQUELCH is a rhythmic acid-house / techno audio manipulator. Incoming audio is
fed into a hazardous reactor and subjected to unstable chemical, radioactive and
nuclear reactions. It should feel like a reactor rather than a conventional
effect: something you provoke, not something you set.

The sound is soft, rounded and squelchy — vocal, liquid, self-limiting.
Not harsh, not brittle, not digital.

## Processing an effect, not a synth

A TB-303 is a synth. It feeds its filter a sawtooth containing every harmonic,
so a resonant peak always has something to amplify. SQUELCH is an effect, and
arbitrary input has holes in its spectrum. Sweeping a resonant peak through a
hole produces nothing.

So anything resonant here has to ring on its own, excited by whatever comes in,
rather than shaping what happens to arrive. This applies to any reaction built
on resonance; it is not a claim that every reaction is a filter.

## Architecture

The transfer functions live in [maths.md](maths.md), each under a heading
naming the reaction it belongs to.

| reaction | mechanism | status |
|---|---|---|
| sequencer (all five) | hashed event scheduling | specified, built |
| CHEMICAL | nonlinear ladder feedback + discrete event-held stochastic register | specified |
| RADIATION | quadrature resonator + continuous correlated stochastic state | specified |
| FISSION | coupled branches, detuning, cancellation, cross-feedback | specified |
| SLUDGE | subharmonic generation, asymmetric saturation, long memory | specified |
| ALIEN | event-gated oscillator + FM/AM synthesis | specified |

All five are now specified in [maths.md](maths.md). The code still runs one
ladder with different coefficients for all five, which is why they measure as
one effect rather than five, and that is the open work.

## Reactions

> A reaction is not differentiated by parameter values. Its identity must come from a mathematical operation unavailable in the other reactions. **CHEMICAL** introduces nonlinear resonant feedback and self-oscillating ladder behaviour, and therefore owns the ladder; **RADIATION** introduces continuously correlated stochastic state modulation around a resonator; **FISSION** introduces multiple coupled paths, phase cancellation, detuning and cross-feedback through coupled branches; **SLUDGE** introduces nonlinear low-frequency/subharmonic generation, asymmetric saturation and long-timescale state memory; **ALIEN** introduces event-local FM/AM synthesis, discrete pitch jumps, reaction-specific timing and an actual oscillator. In architectural terms: **CHEMICAL can own the ladder; RADIATION can use a resonator plus stochastic modulation; FISSION can use coupled branches; SLUDGE can use nonlinear/subharmonic generation; ALIEN can contain an actual oscillator.** That is what makes the five voices genuinely different rather than five coordinate positions in the same filter. Shared controls such as SPREAD, DECAY, EXPOSURE and TOXICITY modulate these mechanisms but do not constitute the mechanisms themselves.

### CHEMICAL vs RADIATION

This explains the difference between the two that seem similar at a glance...

**CHEMICAL jumps. RADIATION moves.**

* **CHEMICAL** uses **discrete stochastic movement**: each reaction event chooses a new state, then holds it while the filter sweep plays out. The result is abrupt, bubbling changes from event to event.
* **RADIATION** uses **continuous correlated stochastic movement**: its internal state constantly wanders, smoothly shifting the resonator over time. The result is unstable, drifting, rubbery motion rather than discrete jumps.

In short:

**CHEMICAL = a new random decision at each event.**
**RADIATION = random movement between events.**

Neither may be implemented with the other's mechanism.

5 Reaction types, 5 voices. This is five effects in a box.

### RADIATION — radioactive instability

- rapid filter movement
- double-time activity
- rubbery squeaks
- Short waxy wind up/down pulses
- low level, rounded Geiger-counter-like ticks
- high resonance
- short percussive events
- rapid micro-variation
- unstable pitch and filter excursions
- occasional high-frequency activity

Instability and micro-variation across very short events, driven by a
continuously correlated stochastic state around a resonator. The state evolves
sample to sample rather than being re-chosen per event.

> The signal has become radioactive.

### FISSION — splitting and fragmentation

- phaser dissonance
- phase cancellation
- frequency splitting
- detune flanging
- feedback
- stereo separation wobbles
- metallic movement
- unstable harmonic relationships

Splitting is the mechanism. One structure becomes two that diverge, and the
dissonance, cancellation and stereo separation fall out of the relationship
between them. A bandpass is a picture of splitting, not splitting.

> One sonic structure has split into multiple unstable components.

### SLUDGE — heavy, contaminated acid

- thick low-mid resonance
- low-pass filtering
- viscous snap back
- lurching through molassis
- soft rounded saturation
- slow filter movement
- long resonant tails
- subharmonic and body enhancement
- heavy low-frequency pressure

Subharmonic generation is the part no filter can do: a lowpass only removes
what is above, it never adds weight below.

> The signal has become chemically contaminated sludge.

### CHEMICAL — volatile laboratory reaction

- bubbling textures
- event-held stochastic register changes
- each event retains its full filter sweep while the register changes
- strong midrange resonance
- short filter envelopes
- high Q
- bubbling pitch-register jumps between reaction events
- fast bubbling activity

Nonlinear ladder feedback with discrete event-held stochastic register changes.
The register is selected once per event and held, so every event performs the
same gesture somewhere else. It does not continuously wander — that is
RADIATION.

> Something unstable is boiling inside the filter.

### ALIEN — non-terrestrial reaction

- short zaps
- pitch sweeps
- resonant chirps
- FM and AM bursts
- rapid pitch jumps
- strange high-frequency accents
- unpredictable event timing

FM and AM are synthesis, not filtering. The pitch jumps and unpredictable
timing also reach back into the sequencer, so this one may not be confined to
the voice.

> The reaction is behaving according to physics we do not understand.

### What is currently implemented

Not a specification — a record of where the code stands.

Five reactions, five different architectures. This is a change from an earlier
build in which all five ran the same ladder and differed only in base frequency
and filter tap; on identical settings nine of the ten pairs then sat between
−6.3 and −11.3 dB apart and RADIATION/CHEMICAL at −20.1 dB were the same sound.
Everything landing within 5 dB of everything else is the signature of one
algorithm with the dials moved, and it was.

| reaction | mechanism | the thing that makes it itself |
|---|---|---|
| CHEMICAL | ladder with in-loop saturation, 180 Hz | an event-held register: one random value per event, constant until the next |
| RADIATION | quadrature resonator, 220 Hz | a continuous correlated state: evolves every sample, R[1] = 0.999 |
| FISSION | two coupled branches with opposed detuning, 320 Hz | branches that interfere with each other, not a pan |
| SLUDGE | oversampled asymmetric saturation, 100 Hz | subharmonics generated at f_h/2 and f_h/4 from its own reactor |
| ALIEN | event-gated self-FM oscillator, 400 Hz | an actual source: output from silence, silence with nothing scheduled |

CHEMICAL and RADIATION are the pair worth watching, because they are the same
shape of idea — a resonator driven by a random number — and the whole
difference between them is the shape of that number in time. The suite reads
both states directly rather than arguing from spectra: CHEMICAL's register is
constant for 0.999 of samples, RADIATION's state for 0.000.

The whole signal path runs in `processBlock`: scheduler, engine, envelopes,
placement, AFTERGLOW, drive, collimation, pan, stereo spread, mid wobble,
voicing, unity match, mix, limiter. The scheduler has all four modes.


## Validation

Three layers, each answering a question the others cannot.

```sh
./.venv/bin/python -m prototype.checks                            # 39 behavioural checks
./build/SquelchHarness | ./.venv/bin/python -m prototype.compare  # 27 numerical primitives
./build/SquelchValidate                                           # dsp-testing.md, Tests 2-20
./scripts/test1.sh                                                # dsp-testing.md, Test 1
```

The **prototype** in `prototype/` is the contract. It is NumPy, it is readable,
and it is what the C++ has to agree with.

The **comparison harness** proves the port computes the same numbers, to 1e-9
relative, on 27 primitives from the oversampler up to the limiter. It cannot
see a coefficient hardcoded in samples, a state that does not survive a block
boundary, or output that is not reproducible, because it only ever runs one
configuration.

The **validation suite** is what looks for those. It covers `dsp-testing.md`
Tests 2 through 20 — finite output, determinism, block-size and sample-rate
invariance, each reaction's defining mechanism, five-reaction differentiation,
control sensitivity, monotonicity, reset, silence, spectral holes, golden
renders — and writes `test-results/summary.json`, `summary.md` and WAVs.
`scripts/test1.sh` runs pluginval at strictness 10 across five sample rates and
seven block sizes.

### What it currently says

99 passed, 0 failed, 10 warnings.

A warning is a measurement that is real, repeatable, and not what the
specification asks for. All ten match the prototype exactly, so none of them is
a port fault, and all ten are decisions about how SQUELCH should sound rather
than mistakes in translating it. They are counted and printed and never hidden
behind the overall verdict.

- **Three controls are inert because a limit became the operating point.**
  RADIATION's DECAY and EXPOSURE and FISSION's DECAY produce bit-identical
  output, −300 dB apart, across their whole travel. `decay_time` is a
  resonator's 1/e time constant, so the bandwidth realising it runs 12.7 Hz
  down to 1.06 Hz for RADIATION and 4.0 Hz down to 0.53 Hz for FISSION — and
  both are floored at 5 Hz, which is above FISSION's entire range, and then
  ceilinged at r = 0.99 and 0.995. A floor of 0.2 Hz and a ceiling of 0.99995
  bracket the formula instead of replacing it and bring all three alive, but
  the tails then run to seconds and the output stage's level match wanders
  7.7 dB chasing them. The cap is load-bearing: it is hiding a level-matching
  weakness downstream, and which of the two to fix is a sound decision.
- **SLUDGE's DECAY is inert because its two followers are the wrong way
  round.** It reaches the output only through the snap `q − r`, and a snap is a
  fast tracker minus a slow one. Here `q` runs at 5 Hz and `r`, the one DECAY
  sets, at 40 down to 12 Hz — so `r` just follows `q` and the difference sits
  at the noise floor.
- **FISSION's comb does not move.** Both modulators are AR(1) states normalised
  by DC gain, `(1−a)·noise + a·m`, which at a 1.3 s time constant gives a
  steady-state deviation of 0.0017 rather than the ±1 the term is written as if
  it spans. The delay modulation is a quarter of a sample. Energy
  normalisation, `sqrt(1−a²)`, is what these want.
- **SLUDGE's TOXICITY moves towards symmetry.** The asymmetry is real and
  present throughout, but opening the control raises the odd orders faster than
  the even ones, so even/odd falls from 4.5 to 0.1. **HALF-LIFE** moves the
  post-input tail by 0.005 dB across its full range.


## Constraints learned the hard way

Measured, not opinions. Every one of these was a defect in the first prototype.

- Actually measure Reaction types against each other, not just one versus dry.
  A reaction measured only against the dry signal proves it does something; it
  cannot show whether two reactions are the same effect.
- This is really 5 Reaction effects in one box.
- At zero the plugin must pass audio through. The first build left a lowpass
  that never opened: −36 dB at 3 kHz with every control at zero. No setting may
  dull the source for no reason.
- No stage may read audio that has not played yet. Level matching, noise bed
  normalisation and sidechain followers all look backwards only, and must give
  the same answer at any host buffer size.
- Do not gate the amplitude. Multiplying the source by an event envelope
  replaces its dynamics with a uniform stream: 6.2 dB of range in the source
  became 2.8 dB at the output. The filter moves; the level should not.
- The limiter is a safety net, not a level control. If it is reducing by
  more than a couple of dB on ordinary material, something upstream is wrong.
- Controls must change the sound, not the level. A chain that
  level-compensates everything leaves every knob feeling inert.
- Measure the delivered result, not the component. A stage can be correct in
  isolation and inaudible in the mix.
- A knob range is not a character. DECAY, EXPOSURE, TOXICITY and SPREAD sweep
  per-reaction ranges, so anything expressed through them is reachable from
  another reaction and cannot be what distinguishes one.
- A limit that applies at every setting is not a limit, it is the value. Three
  controls were clamped flat and the clamp had been *tightened* at some point
  to fix exactly that problem, which only moved which value they were pinned
  to. Check what fraction of a control's travel a safety limit is in force
  over before trusting that it is one.
- A control that acts only on what another control creates cannot be measured
  from a baseline that creates none. SLUDGE's DECAY reaches the output through
  a term REACTIVITY scales, so at REACTIVITY = 0 it is inert by construction
  and a sweep from the default proves nothing.
- Measure the mechanism, not a number near it. Dividing output by input leaves
  the whole filter response, which for a lowpass peaks at DC whether it
  resonates or not; dividing by the same filter with its feedback off is what
  answers the question. A long analysis window averages a moving comb into a
  smear and reports it as shallower, not deeper. A tolerance as wide as a
  parameter's own range cannot fail.
- A test that cannot fail is worse than no test, because it reads as evidence.
  Before trusting a threshold, check what reference it is implicitly using —
  an earlier differentiation test scored a reaction against *itself* as more
  different than two genuinely distinct ones, and passed both.



---

# Cheatsheet

Glyphs and transfer functions are in [maths.md](maths.md).

### Structure — what fires, and when

- (ℛ)  REACTION — which of the five characters
- (ℳ)  MODE — GRID / RANDOM / FREE / INPUT
- (Γ)  GRID — 1/1 … 1/64
- (s₀)  SEED — which pattern the hash produces
- ρ  PROBABILITY — chance a step fires
- φ  FLUX — timing jitter either side of the grid
- σ  VOLATILITY — scatter in time and stereo
- λ  HALF-LIFE — how far one event carries into the next
- κ  CONTAINMENT — closes the vessel, thinning events out

### Voice — the shape of each event

- Δf  SPREAD — how far the cutoff sweeps, in octaves
- τ  DECAY — how long it takes to close
- k  EXPOSURE — loop feedback. At 4 the filter sings on its own
- δ  TOXICITY — saturation inside the loop
- α  REACTIVITY — depth of the accent modulation

### Colour — what surrounds it

- η  ENRICHMENT — how hard the source hits the loop, ±18 dB
- ν  CONTAMINATION — noise bed, textured by the REACTION
- (θ)  COLLIMATOR — narrows the stereo field
- (χ)  FALLOUT — disperses the reaction as it settles
- (T₆₀)  AFTERGLOW — how long it keeps glowing

### Gestures and output

- (ι)  IONIZE — latched: scatters each event in stereo, spectrum and depth
- (Ω)  MELTDOWN — momentary: staged runaway
- (⌈⌉)  CLIP — hard ceiling instead of the limiter, latency padded to match

