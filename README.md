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

Not a specification — a record of where the code stands. Every reaction runs
the one ladder, and these are the only two values per reaction that no control
can reach. Everything else about the filter is a knob range.

| reaction | f_base | tap |
|---|---|---|
| RADIATION | 220 Hz | 24 dB lowpass |
| FISSION | 320 Hz | 3-pole negative, hollow |
| SLUDGE | 100 Hz | lowpass with a 2-pole lift |
| CHEMICAL | 180 Hz | 24 dB lowpass |
| ALIEN | 400 Hz | shallow, bright |

Each also carries its own accent pattern and noise bed texture.

Measured on identical settings, nine of the ten reaction pairs sit between
−6.3 and −11.3 dB apart, and RADIATION/CHEMICAL at −20.1 dB are the same sound.
Genuinely different processes would scatter; everything landing within 5 dB of
everything else is the signature of one algorithm with the dials moved.


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

