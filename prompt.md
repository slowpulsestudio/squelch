# SQUELCH — Concept, Architecture & Parameters

> Rewritten after the first prototype was found to be built on the wrong
> filter. This supersedes the original brief, which described the intent
> correctly but left the transfer function unspecified — and the transfer
> function turned out to be the whole thing. The original is in git history.

## Core concept

SQUELCH is a rhythmic acid-house / techno audio manipulator. Incoming audio is
fed into a hazardous reactor and subjected to unstable chemical, radioactive and
nuclear reactions. It should feel like a reactor rather than a conventional
effect: something you provoke, not something you set.

The sound is soft, rounded and squelchy — vocal, liquid, self-limiting.
Not harsh, not brittle, not digital.

## The central realisation

A TB-303 is a synth. It feeds its filter a sawtooth containing every harmonic,
so a resonant peak always has something to amplify. SQUELCH is an effect, and
arbitrary input has holes in its spectrum. Sweeping a resonant peak through a
hole produces nothing.

So the filter cannot be a peak that merely shapes what arrives. It has to be
a resonator that rings on its own, excited by whatever comes in. That means
a feedback ladder running near self-oscillation, not a biquad with a high Q.

That single distinction is the difference between squelching and sounding like
a telephone.

## Architecture

$$
y(t) = F_{\text{ladder}}\Big(x(t),\; f_c(t),\; k(t),\; \delta\Big)
$$

### The ladder

Four one-pole lowpass stages with global feedback and saturation inside the
loop:

$$
g = 1 - e^{-2\pi f_c / f_s}
$$

$$
u = \tanh\big(\delta\,(x - k\,s_4)\big)\,/\,\delta
$$

$$
s_1 \mathrel{+}= g\,(u - s_1), \quad
s_2 \mathrel{+}= g\,(s_1 - s_2), \quad
s_3 \mathrel{+}= g\,(s_2 - s_3), \quad
s_4 \mathrel{+}= g\,(s_3 - s_4)
$$

$$
y = a\,s_1 + b\,s_2 + c\,s_3 + d\,s_4
$$

Self-oscillation occurs at k = 4. The usable character lives between k = 2 and
k = 3.95.

The tanh must be inside the feedback loop. Its expansion

$$
\tanh x \approx x - \frac{x^3}{3} + \frac{2x^5}{15} - \cdots
$$

generates the odd harmonics that give the resonance a voice, and because the
saturation grows along with the resonance, the filter limits its own ring. That
self-limiting is what makes it round rather than shrill. Saturation placed
after the filter does not do this.

### Output tap

The reaction chooses which poles are summed, giving different filter shapes out
of one structure:

| shape | (a, b, c, d) | character |
|---|---|---|
| 24 dB lowpass | (0, 0, 0, 1) | full, round |
| bandpass | (0, −1, 0, 1) | hollow, nasal |
| highpass | (1, −2, 0, 1) | thin, whistling |

### The cutoff envelope

Retriggered per event, decaying exponentially from its peak back to rest:

$$
f_c(t) = f_{\text{base}} \cdot 2^{\,\Delta f\, e^{-t/\tau}}
$$

Δf is in octaves, not hertz, so the sweep stays musical at any base
frequency. This is the "BWWAAOW": the filter opens at once and closes over τ.

### Accents

An accented step changes three things at once, not just level:

$$
\Delta f \rightarrow \Delta f + \Delta f_{\text{acc}}, \qquad
k \rightarrow k + \Delta k_{\text{acc}}, \qquad
A \rightarrow A + \Delta A
$$

That is why a plain pattern reads as doo-doo-WAAOW-doo.

### Slides

Between events the cutoff glides rather than jumping:

$$
f(t) = f_1 + (f_2 - f_1)\big(1 - e^{-t/\tau_{\text{slide}}}\big)
$$

A glide shorter than about 10 ms is a step, and a step in a high-k ladder is a
click. Measured: at 1.5 ms the artefacts sat 49 dB below the signal; at 35 ms,
69 dB below.

### The sequence

$$
S = [x_1, x_2, \ldots, x_N], \qquad
x_i = (\rho_i,\; A_i,\; \Delta f_i,\; s_i)
$$

Every probabilistic choice hashes (seed, stream, i) rather than drawing from a
running generator, so a bounce reproduces exactly and replaying a bar fires the
same pattern. The animation comes from pitch, accent and slide patterns of
different lengths running against each other.

## Reactions

One filter, five characters. The family resemblance is deliberate — it is what
makes the plugin sound like a thing rather than five effects in a box.

| reaction | f_base | Δf | τ | rate | k | tap |
|---|---|---|---|---|---|---|
| RADIATION | 220 Hz | 4.4 oct | 70 ms | 1/16 | 3.5 | lowpass |
| FISSION | 320 Hz | 3.2 oct | 140 ms | 1/8 | 3.8 | bandpass |
| SLUDGE | 70 Hz | 2.4 oct | 320 ms | 1/4 | 2.9 | lowpass |
| CHEMICAL | 180 Hz | 4.8 oct | 85 ms | 1/8 | 3.2 | lowpass |
| ALIEN | 400 Hz | 5.2 oct | 45 ms | 1/16 | 3.9 | highpass |

Each also carries its own accent pattern and noise bed texture.

## Constraints learned the hard way

Measured, not opinions. Every one of these was a defect in the first prototype.

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



---

# Where each one appears

$$
f_c(t) = f_{\text{base}} \cdot 2^{\,(\Delta f + \alpha\,\Delta f_{\text{acc}})\, e^{-t/\tau}}
$$

$$
u = \tanh\big(\delta\,(\eta\,x - k\,s_4)\big)\,/\,\delta
$$

$$
x_i \ \text{fires if} \ \ \mathrm{hash}(\text{seed}, i) < \rho\,(1 - \kappa)
\qquad
t_i = t_i^{\text{grid}} + \phi\,\xi_i + \sigma\,\zeta_i
$$

Voice is everything inside the filter. Structure is everything in the
sequencer. Nothing appears in both.

Bracketed glyphs are labels rather than quantities: they name a control that
does not appear as a term in the maths above.


---

# Cheatsheet

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

