# SQUELCH — Full Concept & Parameter Metaprompt

## Core Concept

**SQUELCH** is a rhythmic acid-house / techno audio manipulator that takes incoming audio and subjects it to unstable chemical, radioactive, and nuclear reactions.

The plugin should feel like a **hazardous sonic reactor** rather than a conventional audio effect.

Incoming sound enters the reactor and is subjected to different reactions. It can become:

* radioactive
* unstable
* volatile
* toxic
* resonant
* contaminated
* distorted
* fragmented
* amplified
* suppressed
* contained
* dispersed

The central metaphor is:

> **SQUELCH puts sound inside a hazardous reactor and lets it mutate.**

The user is not simply adjusting effects. They are controlling the behavior of a volatile sonic system.

---

# Sonic Identity

SQUELCH should be designed around:

* acid house
* acid techno
* resonant filter movement
* rubbery squelches
* bubbling midrange
* radioactive ticks
* Geiger-counter-like activity
* phaser dissonance
* unstable pitch movements
* laser-like zaps
* toxic low-frequency pressure
* distorted resonance
* short rhythmic reactions
* unpredictable but musically useful mutations

The sound should feel:

* deep
* punchy
* acidic
* dirty
* synthetic
* resonant
* rhythmic
* unstable
* physical
* club-oriented

Avoid:

* thin digital effects
* generic EDM effects
* cartoon laser sounds
* overly bright or metallic processing
* meaningless randomization
* effects that feel detached from the incoming audio
* excessive high-frequency harshness

The processed signal should retain a relationship to the input.

---

# Core Processing Philosophy

The conceptual process is:

**INPUT → REACTION → TRIGGER → PROBABILITY → REACTIVITY → VOLATILITY → EXPOSURE → DECAY → FALLOUT**

The plugin should continuously transform incoming material rather than simply apply a static effect.

Possible DSP processes include:

* input sampling
* micro-slicing
* transient detection
* event generation
* rhythmic triggering
* resonant filtering
* phasing
* pitch shifting
* pitch sweeps
* FM/AM bursts
* saturation
* distortion
* envelope generation
* stochastic modulation
* feedback
* stereo dispersion
* event accumulation
* state persistence
* spectral restriction
* nonlinear transformation

---

# PARAMETERS

## 1. REACTION

**Common audio equivalent:** Effect Style / Processing Type

The primary selector determining what type of chemical or radioactive reaction is applied.

### ☢ RADIATION

Radioactive instability.

DSP characteristics:

* rapid filter movement
* double-time activity
* rubbery squeaks
* Geiger-counter-like ticks
* high resonance
* short repeated events
* rapid micro-variation
* unstable pitch/filter excursions
* occasional high-frequency activity

Metaphor:

> The signal has become radioactive.

---

### ⚛ FISSION

Splitting and fragmentation.

DSP characteristics:

* phaser dissonance
* phase cancellation
* frequency splitting
* feedback
* stereo separation
* metallic movement
* unstable harmonic relationships

Metaphor:

> One sonic structure has split into multiple unstable components.

---

### ☣ TOXIC SLUDGE

Heavy, contaminated acid.

DSP characteristics:

* thick low-mid resonance
* low-pass filtering
* saturation
* slow filter movement
* long resonant tails
* subharmonic/body enhancement
* heavy low-frequency pressure

Metaphor:

> The signal has become chemically contaminated sludge.

---

### 🧪 BEAKER

Volatile laboratory reaction.

DSP characteristics:

* bubbling textures
* rapid randomized filter movement
* strong midrange resonance
* short filter envelopes
* high Q
* stochastic pitch movement
* fast bubbling activity

Metaphor:

> Something unstable is boiling inside the filter.

---

### 👽 ALIEN

Non-terrestrial reaction.

DSP characteristics:

* short zaps
* pitch sweeps
* resonant chirps
* FM/AM bursts
* rapid pitch jumps
* strange high-frequency accents
* unpredictable event timing

Metaphor:

> The reaction is behaving according to physics we do not understand.

---

# 2. MODE

**Common audio equivalent:** Trigger Mode

Determines how SQUELCH decides when a reaction occurs.

Possible modes:

* **GRID** — reaction follows a fixed rhythmic clock
* **RANDOM** — reaction occurs at irregular intervals
* **FREE** — autonomous timing independent of the grid
* **INPUT** — reaction triggered by detected events in the incoming audio

Metaphor:

> The reactor's operating protocol.

---

# 3. GRID

**Common audio equivalent:** Rhythmic Division

Determines the rhythmic resolution when MODE is GRID.

Possible values:

* 1/1
* 1/2
* 1/4
* 1/8
* 1/16
* 1/32
* triplets
* dotted divisions

Metaphor options:

* Dose
* Cycle
* Interval
* Pulse Rate
* Reactor Clock

Core concept:

> How frequently is the reactor allowed to react?

---

# 4. FLUX

**Common audio equivalent:** Swing / Timing Offset

Controls rhythmic displacement from the strict grid.

Low FLUX:

* rigid
* mechanical
* precise
* locked

High FLUX:

* displaced
* unstable
* swinging
* irregular
* chemically disturbed

Metaphor:

> The reactor's timing field is fluctuating.

**FLUX replaces SWING as the canonical parameter name.**

---

# 5. PROBABILITY

**Common audio equivalent:** Event Probability

Controls the likelihood that a reaction occurs at an available rhythmic or temporal opportunity.

Low PROBABILITY:

* sparse
* intermittent
* unpredictable
* many gaps

High PROBABILITY:

* frequent
* consistent
* dense
* highly active

Metaphor options:

* Chance
* Odds
* Risk
* Reaction Rate
* Incidence
* Activation

Core concept:

> What are the chances of a reaction?

Important distinction:

**PROBABILITY determines whether something happens.**

**REACTIVITY determines how much happens when it does.**

---

# 6. REACTIVITY

**Common audio equivalent:** Event Density / Activity / Complexity

Controls the amount and complexity of activity generated by each reaction.

Possible DSP behavior:

* number of generated events
* simultaneous voices
* modulation density
* filter movement
* rhythmic subdivisions
* repeated events
* granular density
* internal modulation rate

Low REACTIVITY:

* isolated reactions
* simple events
* restrained movement

High REACTIVITY:

* multiple simultaneous events
* bubbling activity
* rapid movement
* dense acid behavior

Metaphor:

> How chemically active is the system?

**REACTIVITY replaces BUSYNESS as the canonical parameter name.**

---

# 7. VOLATILITY

**Common audio equivalent:** Randomization / Mutation Depth

Controls how much each reaction differs from the previous reaction.

Possible variables:

* filter cutoff
* resonance
* pitch
* timing
* event duration
* stereo position
* modulation depth
* envelope shape
* event count

Low VOLATILITY:

* stable
* repeatable
* predictable

High VOLATILITY:

* unstable
* unpredictable
* constantly changing
* increasingly mutated

Metaphor:

> How unstable is the compound?

**VOLATILITY replaces MUTATION as the canonical parameter name.**

---

# 8. HALF-LIFE

**Common audio equivalent:** Inter-Reaction Persistence / State Memory

Controls how long characteristics of previous reactions continue influencing subsequent reactions.

Low HALF-LIFE:

* each reaction resets
* little memory
* isolated events

High HALF-LIFE:

* previous reactions influence future reactions
* mutations accumulate
* filter states persist
* pitch states persist
* resonance states persist
* the reactor develops memory
* reactions become increasingly contaminated by previous states

Metaphor:

> How long does the reaction remain radioactive?

Important distinction:

**HALF-LIFE is persistence between reactions.**

It is not simply another envelope decay control.

---

# 9. DECAY

**Common audio equivalent:** Envelope Release / Event Lifetime

Controls how quickly an individual reaction disappears.

Low DECAY:

* short
* clipped
* percussive
* aggressive

High DECAY:

* longer tails
* sustained resonance
* lingering reactions
* increased sonic smear

Metaphor options:

* Breakdown
* Dissolution
* Decomposition
* Fallout
* Disintegration

Core concept:

> How quickly does the reaction die?

Important distinction:

**HALF-LIFE = how long a reaction influences future reactions.**

**DECAY = how long the current reaction remains audible.**

---

# 10. RANGE

**Common audio equivalent:** Octave Breadth / Pitch Range

Controls how far generated reactions can travel through pitch.

Low RANGE:

* narrow
* concentrated
* focused
* stable pitch region

High RANGE:

* octave jumps
* large pitch excursions
* broad harmonic territory
* dramatic pitch mutations

Metaphor options:

* Spectrum
* Bandwidth
* Frequency Span
* Isotope Range
* Excursion

Core concept:

> How far can the reaction travel?

---

# 11. SQUELCH

**Common audio equivalent:** Resonance / Acid Intensity / Main Character Macro

The primary sonic intensity control.

Increasing SQUELCH should increase the characteristic acid behavior through combinations of:

* resonance
* filter movement
* modulation depth
* nonlinear processing
* event intensity
* harmonic emphasis
* rhythmic articulation

Metaphor options:

* Acidity
* Reactivity
* Pressure
* Toxicity
* Resonance

Core concept:

> How violently does the signal squelch?

SQUELCH should not behave simply as a wet/dry control or output gain.

It should be the primary macro for the recognizable SQUELCH character.

---

# 12. RODS

**Common audio equivalent:** Dampening / Suppression / Stabilization

Controls suppression of the reactor.

Possible DSP behavior:

* reduce resonance
* reduce modulation depth
* reduce feedback
* reduce event density
* smooth filter movement
* tame unstable pitch behavior
* reduce extreme reaction behavior

Low RODS:

* reactor running hot
* unstable
* highly reactive

High RODS:

* suppressed
* controlled
* damped
* stabilized

Metaphor:

> Insert the rods to suppress the reaction.

Canonical name:

**RODS**

The name refers to reactor control rods and should remain concise.

---

# 13. DRIVE

**Common audio equivalent:** Saturation / Nonlinear Gain

Controls nonlinear energy and harmonic contamination.

Possible DSP behavior:

* saturation
* distortion
* clipping
* harmonic generation
* low-mid thickening
* transient compression
* increased perceived density

Metaphor:

> How toxic has the signal become?

Associated vocabulary:

* Dose
* Exposure
* Contamination
* Overload
* Toxicity

Canonical parameter name:

**DRIVE**

---

# 14. EXPOSURE

**Common audio equivalent:** Resonance / Resonant Excitation

Controls how strongly the resonant system is excited.

Possible DSP behavior:

* filter Q
* resonance
* feedback
* ringing
* harmonic emphasis
* self-excitation

Low EXPOSURE:

* restrained
* controlled
* clean

High EXPOSURE:

* resonant
* ringing
* aggressive
* unstable
* highly excited

Metaphor:

> How much radiation is the system being exposed to?

Canonical parameter name:

**EXPOSURE**

This replaces the conventional parameter name **RESONANCE**.

---

# 15. COLLIMATOR

**Common audio equivalent:** Filter / Spectral Restriction

Controls the spectral region allowed through the reactor.

A collimator is used in radiation and particle systems to restrict and shape a beam. This makes it a strong metaphor for a frequency filter: the sonic energy is constrained into a controlled spectral region.

Possible DSP implementation:

* high-pass cutoff
* low-pass cutoff
* band-pass region
* spectral masking
* frequency restriction
* resonant spectral focus

Low COLLIMATOR:

* broad spectral output
* less restricted
* more uncontrolled
* more material escapes

High COLLIMATOR:

* tightly restricted
* focused
* concentrated
* narrow spectral region

Metaphor:

> Focus the reactor's output into a controlled sonic beam.

Canonical name:

**COLLIMATOR**

---

# 16. FALLOUT

**Common audio equivalent:** Stereo Spread / Spatial Dispersion

Controls how far the reaction spreads spatially.

Low FALLOUT:

* centered
* localized
* contained
* focused

High FALLOUT:

* wide stereo
* dispersed particles
* spatial movement
* environmental spread
* material extending into the stereo field

Possible DSP behavior:

* stereo width
* stereo decorrelation
* panning
* spatial diffusion
* transient spreading
* frequency-dependent stereo movement

Metaphor:

> How far has the fallout spread?

Canonical name:

**FALLOUT**

---

# Final Canonical Parameter Set

1. **REACTION**
2. **MODE**
3. **GRID**
4. **FLUX**
5. **PROBABILITY**
6. **REACTIVITY**
7. **VOLATILITY**
8. **HALF-LIFE**
9. **DECAY**
10. **RANGE**
11. **SQUELCH**
12. **RODS**
13. **DRIVE**
14. **EXPOSURE**
15. **COLLIMATOR**
16. **FALLOUT**

---

# Conceptual Hierarchy

The parameters should read as a coherent reactor system.

### WHAT is happening?

**REACTION**

### HOW is it triggered?

**MODE**

### At what rhythmic resolution?

**GRID**

### How unstable is the timing?

**FLUX**

### DOES it happen?

**PROBABILITY**

### How active is it?

**REACTIVITY**

### How unstable is each reaction?

**VOLATILITY**

### How long does its influence persist?

**HALF-LIFE**

### How quickly does the current sound disappear?

**DECAY**

### How far can it move in pitch?

**RANGE**

### How aggressively does it produce the characteristic acid sound?

**SQUELCH**

### How much is the reactor suppressed?

**RODS**

### How toxic/distorted is the signal?

**DRIVE**

### How strongly is the resonant system excited?

**EXPOSURE**

### How tightly is the spectrum restricted?

**COLLIMATOR**

### How far does the resulting material spread?

**FALLOUT**

---

# Performance Narrative

A typical SQUELCH performance should feel like a reactor moving through different levels of instability.

**INPUT**

Incoming audio enters the reactor.

↓

**REACTION**

The user selects the type of chemical/radioactive process.

↓

**MODE**

The reactor determines when it is permitted to react.

↓

**GRID**

The rhythmic operating frequency is established.

↓

**FLUX**

The timing begins to fluctuate.

↓

**PROBABILITY**

Each available reaction opportunity may or may not activate.

↓

**REACTIVITY**

Activated reactions become more or less chemically active.

↓

**VOLATILITY**

Each reaction mutates away from the previous one.

↓

**HALF-LIFE**

Previous reactions leave a persistent influence on future reactions.

↓

**DECAY**

Individual reactions eventually disappear.

↓

**RANGE**

Reactions can travel through different pitch regions.

↓

**SQUELCH**

The characteristic acid intensity increases.

↓

**RODS**

The reactor can be suppressed and stabilized.

↓

**DRIVE**

The signal becomes increasingly toxic and nonlinear.

↓

**EXPOSURE**

The resonant system becomes increasingly excited.

↓

**COLLIMATOR**

The output is spectrally focused and restricted.

↓

**FALLOUT**

The remaining sonic material disperses into the stereo field.

---

# Overall Metaphor

> **SQUELCH is a hazardous sonic reactor.**
>
> Sound enters the chamber and is subjected to a chemical or radioactive reaction.
>
> Some reactions happen by chance. Some become highly reactive. Some mutate. Some remain radioactive and influence what happens next. Some decay quickly. Others become toxic, resonant, and unstable.
>
> The reactor can be controlled with rods, its output can be focused through a collimator, and the resulting energy can spread outward as fallout.
>
> **SQUELCH is not simply processing audio. It is controlling the behavior of a volatile sonic system.**
