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
./.venv/bin/python -m prototype.diagnostics                       # dsp-testing.md, Test 19
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
seven block sizes. `prototype.diagnostics` turns the WAVs into six plots each,
because what kind of failure something is — mathematical, numerical,
implementation, parameter mapping, or a poor threshold — looks different in a
picture and identical in a table.

### What it currently says

99 passed, 0 failed, 0 warnings, 1 assertion not applicable.

The not-applicable one is Test 13's "higher DECAY should not shorten the tail",
asked of SLUDGE, which has no tail: it is a filter on the input and its own
reactor oscillator never decays. Counted apart from the passes, because an
assertion that never ran is not a pass.

### What was fixed

Six controls did nothing, or did the opposite of what they were named. Every
one of them matched the prototype exactly, so none was a port fault — the
suite was finding real faults in the instrument's own maths.

**RADIATION's DECAY and EXPOSURE, and FISSION's DECAY**, produced
bit-identical output, −300 dB apart, across their whole travel. `decay_time` is
a t60 — which is what a profile written as 0.025 to 0.3 seconds reads as — but
the bandwidth realising it was computed as `1/(pi*tau)`, the 1/e form, which
rings 6.9× too long. Every setting then fell under a 5 Hz bandwidth floor and a
r = 0.99 damping ceiling, which between them pinned the resonator flat.

The repair ran four layers deep, and each layer was only visible once the one
above it was fixed:

1. `2.1986/t60` for the bandwidth, with the floor and ceiling moved out to
   0.2 Hz and 0.99995 where nothing in the control range reaches them. DECAY
   now spans 25 ms to 1 s of t60, which is what RADIATION's profile says.
2. The level match was following the reaction's own ring. Its 0.6 s/1.2 s
   tracker was set when the longest tail was 16 ms; slowed to 2 s/4 s, the gain
   swing drops from 7.65 dB to 3.2 dB and sustained movement — the thing a
   compressor actually does — from 5.15 dB/s to 1.7.
3. Unpinning `r` let the energy normalisation vary where it had been a fixed
   −17 dB trim, dropping the drive stage's input 7.3 dB. A calibration gain of
   1.3 puts it back. The window is narrow and measured from both sides: below
   1.2 the drive curve is never reached, above 1.5 the limiter stops being a
   safety net.
4. Two checks were then measuring quantities the output stage deliberately
   equalises. ENRICHMENT's 4 dB "level move" is entirely crest — peaks hold to
   0.00 dB — and AFTERGLOW looked inert at 0.2 dB while actually stretching the
   render's decay from 0.70 s to 3.80 s.

**FISSION's comb sat still.** Both its modulators were normalised by DC gain,
`(1−a)·noise + a·m`, which is right for a filter that must pass a constant
unchanged and wrong for one whose output *is* the signal: at a 0.9 s time
constant `(1−a)` is 2.5e−5 while the steady-state deviation is 0.0020, a
thousandth of the ±1 the terms are written as if they span. The delay
modulation was a quarter of a sample. Energy normalisation, `sqrt(1−a²)`, takes
the frame-to-frame comb similarity from 0.981 to 0.810 and a mono input's
stereo correlation from 0.975 to 0.182.

**Three of SLUDGE's controls** were all the same mistake in different places:
a term written as if its input were normalised, when it is a raw envelope that
sits near 0.15 on real material.

- **DECAY** reaches the output through one term, the snap `q − r`.
  [maths.md](maths.md) writes `r` as a filter *of* `q` and then describes `r`
  as "moving first and `q` following" — which a cascade cannot do, since `q` is
  `r`'s input. Made siblings off the envelope, the prose becomes the behaviour.
  Then as a fraction of the body rather than a raw difference, DECAY's span
  goes from 1.0–1.9% of cutoff movement to 6.0–10.7%.
- **TOXICITY** drove the saturator's `delta` while its asymmetry `gamma` stayed
  fixed. Raising drive alone pushes both halves of the curve toward their own
  limits, so even orders saturate while odd ones keep climbing: even/odd fell
  from 4.3 to 0.1, meaning *opening* the control moved the sound towards
  symmetry. Sweeping `gamma` 0.9 → 0.1 with it gives 0.68 → 1.28, with the even
  orders themselves up seventeen times and the odd ones still rising.
- **HALF-LIFE** sets the long memory `M`, and `M_b = tanh(M)` is written as if
  `M` were normalised. It never left tanh's linear region, so the whole
  molasses movement was 0.2 of the 1.17 octaves SPREAD makes available.
  Measured against a reference level it runs at its designed depth.

Two of those were invisible until the measurement was fixed too. HALF-LIFE's
check asked what the tail's RMS did, but the memory drives the cutoff and never
touches the level — it read 0.005 dB. Read off the state directly, the way
Tests 6 and 7 read CHEMICAL's register and RADIATION's stochastic state, the
body is 21% intact half a second after the input stops at one end of the
control and 94% at the other.


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
- Never measure the one quantity a later stage normalises away. The output
  stage matches PEAKS on purpose, so an RMS check downstream of it reports
  crest-factor changes as gain changes, and a tail-level check reports a
  working reverb as inert. Ask for duration, or for peak, or for whatever the
  stage is not holding constant.
- A calibration that was never exercised is not a calibration. Every gain
  stage downstream of RADIATION had been set against a resonator that a clamp
  held at one value, so the first time the control actually moved, four
  separate stages turned out to be depending on the bug.
- When a specification's equations and its prose disagree, the prose is
  usually describing the intent and the equations the mistake. maths.md wrote
  SLUDGE's snap as a cascade and then described the two states as racing each
  other, which only a parallel pair can do.
- A bare difference or sum of envelopes is only O(1) if the envelope is
  normalised. `max|x|` sits near 0.15 on real material, so `tanh(M)` never
  left its linear region and an exponent-in-octaves applied to `q − r` moved
  the filter 2%. Three of SLUDGE's controls were weak for this one reason.
  Measure against a reference level, or take the fraction.
- Normalise a modulator by its ENERGY, `sqrt(1−a²)`, not its DC gain `(1−a)`.
  DC normalisation is for a filter that must pass a constant unchanged; a
  modulator's output *is* the signal, and at a 0.9 s time constant `(1−a)`
  makes it a thousandth of the range it is written for.
- A control named for a character should reach the thing that produces that
  character. SLUDGE's TOXICITY drove the saturator's gain while its asymmetry
  stayed fixed, so opening it moved the sound *towards* symmetry — the even
  orders saturate first and the odd ones keep climbing.
- "Not applicable" is a third outcome and worth having. Folding it into the
  passes inflates them with assertions that never ran; folding it into the
  warnings reports a working control as suspect.



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

