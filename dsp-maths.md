# SQUELCH — maths

Each section says what the maths is for. Anything not under a reaction heading applies to all five.

The design brief is in [README.md](README.md). This file holds only the transfer functions, state equations and control mappings.

A reaction marked TBC is not yet specified.

A reaction's identity is a mathematical operation the others do not have.

Parameter values are not an identity.

| reaction             | operation that defines it                                  | status    |
| -------------------- | ---------------------------------------------------------- | --------- |
| sequencer (all five) | hashed event scheduling                                    | specified |
| CHEMICAL             | nonlinear resonant feedback, self-oscillating ladder       | specified |
| RADIATION            | stochastic state modulation around a resonator             | specified |
| FISSION              | coupled branches, cancellation, cross-feedback             | specified |
| SLUDGE               | subharmonic generation, asymmetric saturation, long memory | specified |
| ALIEN                | event-local FM/AM, an actual oscillator                    | specified |

---

# Shared — the sequencer

Applies to all five reactions. This decides when a reaction fires, never what it sounds like.

$$
S = [x_1, x_2, \ldots, x_N],
\qquad
x_i = (\rho_i,A_i,\Delta f_i,s_i)
$$

An event fires when:

$$
h_i =
\mathrm{hash}(s_0,\mathrm{stream},i)
$$

$$
h_i < \rho_i(1-\kappa)
$$

where \(h_i\in[0,1)\).

The event time is:

$$
t_i =
t_i^{grid}
+
\phi\,\xi_i
+
\sigma\,\zeta_i
$$

where \(\xi_i,\zeta_i\) are deterministic hash-derived values centred on zero.

Every probabilistic choice hashes `(seed, stream, i)` rather than drawing from a running generator, so a bounce reproduces exactly and replaying a bar fires the same pattern.

The animation comes from pitch, accent and slide patterns of different lengths running against each other.

The reaction may derive additional deterministic values from the event identity:

$$
r_{i,j}
=
2\,\mathrm{hash}(s_0,\mathrm{stream}_j,i)-1
$$

with:

$$
r_{i,j}\in[-1,1]
$$

These values are suitable for deterministic stochastic modulation, event variation and reaction-specific timing.

A hash value is a source of deterministic variation, not an audio-rate white-noise generator. Audio-rate stochastic states are produced by filtering or integrating these deterministic excitation values.

## State domains

Reaction state falls into three domains, and which domain a state belongs to is part of the reaction's identity.

### Sample-domain state

Updated every sample:

* feedback state
* resonator state
* oscillator phase
* nonlinear state
* sample-domain envelopes
* RADIATION's stochastic state

### Event-domain state

Updated at reaction events and held between them:

* event amplitude
* event register
* event-local pitch
* CHEMICAL's stochastic register

### Control-domain state

May update at an explicitly defined control interval:

* UI and control smoothing
* explicitly designated low-rate modulation

Host block size must not redefine the mathematical identity of a reaction. Sample-domain state remains sample-domain state even when the host supplies audio in blocks, and any control-rate approximation must be documented explicitly.

The CHEMICAL/RADIATION distinction is a domain distinction: CHEMICAL's stochastic register is event-domain state, RADIATION's stochastic state is sample-domain state.

---

# CHEMICAL — resonant feedback ladder

The maths below is CHEMICAL's engine. It is not the architecture of the plugin and the other four reactions are not built from it.

## What it has to do

* bubbling textures
* strong midrange resonance
* short filter envelopes
* high Q
* event-held stochastic register changes
* each event retains its full filter sweep while the register changes
* bubbling pitch-register jumps between reaction events
* fast bubbling activity

The ladder covers the resonance, the short envelopes and the high Q. Reaction-level modulation is implemented as deterministic, event-held register selection around the nonlinear ladder: the selected register remains constant for the event while the filter envelope performs its full sweep. CHEMICAL produces repeated filter gestures whose register changes from event to event. It does not continuously wander its filter or pitch state — that is RADIATION's mechanism.

## Why a ladder and not a biquad

A TB-303 is a synth. It feeds its filter a sawtooth containing every harmonic, so a resonant peak always has something to amplify. SQUELCH is an effect, and arbitrary input has holes in its spectrum. Sweeping a resonant peak through a hole produces nothing.

So the filter cannot be a peak that merely shapes what arrives. It has to be a resonator that rings on its own, excited by whatever comes in.

That means a feedback ladder running near self-oscillation, not a biquad with a high Q.

## The ladder

$$
y[n] =
F_{\text{ladder}}
\big(
x[n],f_c[n],k[n],\delta
\big)
$$

Four one-pole lowpass stages with global feedback and saturation inside the loop:

$$
g[n] =
1-e^{-2\pi f_c[n]/f_s}
$$

$$
u[n]
=
\frac{
\tanh
\left(
\delta\left(
\eta x[n]-k[n]s_4[n-1]
\right)
\right)
}{\delta}
$$

$$
s_1[n]
=
s_1[n-1]
+
g[n]\left(u[n]-s_1[n-1]\right)
$$

$$
s_2[n]
=
s_2[n-1]
+
g[n]\left(s_1[n]-s_2[n-1]\right)
$$

$$
s_3[n]
=
s_3[n-1]
+
g[n]\left(s_2[n]-s_3[n-1]\right)
$$

$$
s_4[n]
=
s_4[n-1]
+
g[n]\left(s_3[n]-s_4[n-1]\right)
$$

The output is a weighted combination:

$$
y[n]
=
a\,s_1[n]
+
b\,s_2[n]
+
c\,s_3[n]
+
d\,s_4[n]
$$

Self-oscillation is topology- and implementation-dependent rather than universally fixed at \(k=4\). For the actual discrete loop:

$$
L(z)=kH_{\text{ladder}}(z)
$$

and the small-signal oscillation boundary is approximately:

$$
k_{\text{crit}}
=
\frac{1}
{
\max_\omega
|H_{\text{ladder}}(e^{j\omega})|
}
$$

subject to the phase condition:

$$
\angle H_{\text{ladder}}(e^{j\omega}) = 2\pi m
$$

for some integer \(m\).

The useful CHEMICAL range is therefore expressed relative to the implementation's measured threshold:

$$
k=\alpha_k k_{\text{crit}}
$$

with the musical operating region below runaway.

\(k_{\text{crit}}\) is topology- and sample-rate-dependent. It must not be treated as a universal value of 4. The implementation must determine or cache the appropriate threshold per sample rate and relevant ladder configuration. Where \(k=4\) appears in the literature it is the continuous-time idealisation, not the discrete implementation threshold.

The tanh must be inside the feedback loop.

Its expansion:

$$
\tanh x
\approx
x-\frac{x^3}{3}
+\frac{2x^5}{15}
-\cdots
$$

generates odd harmonics that give the resonance a voice. Because saturation grows along with the resonance, the filter limits its own ring.

Saturation placed after the filter does not do this.

## Output tap

| shape         | \((a,b,c,d)\)  | character       |
| ------------- | -------------- | --------------- |
| 24 dB lowpass | \((0,0,0,1)\)  | full, round     |
| bandpass      | \((0,-1,0,1)\) | hollow, nasal   |
| highpass      | \((1,-2,0,1)\) | thin, whistling |

The weights determine which poles contribute to the output.
## The stochastic register

CHEMICAL's randomness is discrete, per-event and zero-order held. For event \(i\):

$$
q_i = 2\,\mathrm{hash}(s_0,\mathrm{chemical},i)-1
$$

with:

$$
q_i\in[-1,1]
$$

The value is held for the whole event interval:

$$
q[n]=q_i, \qquad t_i\le n<t_{i+1}
$$

The register spread is:

$$
R_q = \text{CHEMICAL register spread, in octaves}
$$

\(R_q\) is an internal CHEMICAL parameter. It is not the shared VOLATILITY control \(\sigma\), and it is not exposed as a plugin control unless an explicit mapping is defined elsewhere.

The register enters the cutoff as an offset in log-frequency, never as a multiplier on the sweep depth:

$$
f_c[n]
=
f_{\text{base}}
\,2^{R_q q_i}
\,2^{\Delta f e^{-a_i/\tau}}
$$

equivalently:

$$
f_c[n]
=
f_{\text{base}}
\,2^{R_q q_i + \Delta f e^{-a_i/\tau}}
$$

where \(a_i\) is time since event \(i\).

The stochastic register changes where the event occurs in frequency. It does not scale, suppress or reverse the magnitude of the event's sweep. The form:

$$
f_c[n] = f_{\text{base}} 2^{\Delta f q_i e^{-a_i/\tau}}
$$

is incorrect: it makes \(q_i\) control sweep depth, so negative registers sweep downward and registers near zero barely move at all.

## The cutoff envelope

Retriggered per event, with the register offset held constant for the event:

$$
f_c(t)
=
f_{\text{base}}
\cdot
2^{R_q q_i}
\cdot
2^{\Delta f e^{-t/\tau}}
$$

With accent:

$$
f_c(t)
=
f_{\text{base}}
\cdot
2^{R_q q_i}
\cdot
2^{
(\Delta f+\alpha\Delta f_{\text{acc}})
e^{-t/\tau}
}
$$

The cutoff therefore starts at:

$$
f_c(0)
=
f_{\text{base}}2^{\Delta f}
$$

and approaches:

$$
\lim_{t\rightarrow\infty}f_c(t)
=
f_{\text{base}}
$$

## Accents

An accented step changes three things:

$$
\Delta f
\rightarrow
\Delta f+\Delta f_{\text{acc}}
$$

$$
k
\rightarrow
k+\Delta k_{\text{acc}}
$$

$$
A
\rightarrow
A+\Delta A
$$

## Slides

Between events the cutoff glides:

$$
f(t)
=
f_1
+
(f_2-f_1)
\left(
1-e^{-t/\tau_{\text{slide}}}
\right)
$$

The slide constant remains separate from anti-click smoothing.

## With ENRICHMENT in the loop

$$
u[n]
=
\frac{
\tanh
\left(
\delta
\left(
\eta x[n]-k s_4[n-1]
\right)
\right)
}{\delta}
$$

---

# RADIATION — stochastic resonator

## What it has to do

* rapid filter movement
* double-time activity
* rubbery squeaks
* short waxy wind-up/down pulses
* low-level rounded Geiger-counter-like ticks
* high resonance
* short percussive events
* rapid micro-variation
* unstable pitch and filter excursions
* occasional high-frequency activity

## The mechanism

RADIATION is a resonator whose internal frequency, excitation and energy are continuously perturbed by correlated stochastic states.

The stochastic modulation is the identity.

It is not white noise through a filter.

It is not independent random cutoff changes.

Each stochastic state has memory:

$$
q[n+1]
=
a_q q[n]
+
b_q r[n]
$$

where:

$$
a_q=e^{-1/(\tau_q f_s)}
$$

and:

$$
b_q=
\sqrt{1-a_q^2}
$$

with deterministic hash-derived:

$$
r[n]\in[-1,1]
$$

The state therefore changes continuously rather than jumping independently every sample.

\(\tau_q\) is the correlation time of the RADIATION stochastic state. It is an internal reaction-state parameter and is distinct from the shared DECAY control \(\tau\).

RADIATION's stochastic state evolves continuously between reaction events. It is not a per-event register and is not held constant until the next event. That event-held mechanism belongs to CHEMICAL, and the two must not converge:

$$
\boxed{\mathrm{CHEMICAL} = \text{discrete event-held stochastic register}}
$$

$$
\boxed{\mathrm{RADIATION} = \text{continuous correlated stochastic state}}
$$

A second, faster state provides the characteristic radioactive micro-variation:

$$
m[n+1]
=
a_m m[n]
+
b_m r_m[n]
$$

with:

$$
\tau_m < \tau_q
$$

so \(m\) moves faster than \(q\).

The slower state produces the broad rubbery movement; the faster state produces the microscopic instability.

## Resonant frequency

The base frequency is:

$$
f_r[n]
=
f_{\text{base}}
\cdot
2^{
\frac{
\Delta f
\left(
q[n]+\beta_m m[n]
\right)
}{12}
}
$$

where \(q\) is the slow stochastic state and \(m\) is the rapid state.

The frequency excursion is bounded:

$$
q_b[n]
=
\tanh(q[n])
$$

$$
m_b[n]
=
\tanh(m[n])
$$

so:

$$
f_r[n]
=
f_{\text{base}}
\cdot
2^{
\Delta f
\left(
q_b[n]+\beta_m m_b[n]
\right)/12
}
$$

This prevents a rare stochastic excursion from producing an unbounded frequency.

## Resonator state

RADIATION uses a second-order resonator rather than CHEMICAL's four-stage ladder.

Define:

$$
\omega_r[n]
=
\frac{2\pi f_r[n]}{f_s}
$$

and damping:

$$
r[n]
=
e^{-\pi B[n]/f_s}
$$

where \(B[n]\) is the instantaneous bandwidth.

The resonator uses the coupled quadrature form rather than a direct-form recurrence. RADIATION modulates \(\omega_r\) and \(r\) rapidly and continuously by design, and a direct form does not carry amplitude independently of frequency, so fast coefficient modulation produces amplitude pumping and transient instability.

$$
c[n]=\cos(\omega_r[n])
$$

$$
s[n]=\sin(\omega_r[n])
$$

$$
v_{re}[n]
=
r[n]
\left(
c[n]v_{re}[n-1]
-
s[n]v_{im}[n-1]
\right)
+
e[n]
$$

$$
v_{im}[n]
=
r[n]
\left(
s[n]v_{re}[n-1]
+
c[n]v_{im}[n-1]
\right)
$$

equivalently:

$$
\mathbf v[n]
=
r[n]R(\omega_r[n])\mathbf v[n-1]
+
\mathbf e[n],
\qquad
R(\omega)=
\begin{bmatrix}
\cos\omega&-\sin\omega\\
\sin\omega&\cos\omega
\end{bmatrix}
$$

with:

$$
0\le r[n]<1
$$

after modulation, where \(e[n]\) is the reaction excitation.

The resonator therefore has its own memory and can ring after the incoming signal has momentarily disappeared.

## Stochastic resonance

The excitation contains three components:

$$
e[n]
=
\eta x[n]
+
\epsilon_p p[n]
+
\epsilon_g g[n]
$$

where:

* \(\eta x[n]\) is the incoming signal,
* \(p[n]\) is a short percussive event pulse,
* \(g[n]\) is a low-level rounded stochastic tick source.

The event pulse uses:

$$
p[n]
=
A_p
e^{-n/\tau_p}
$$

for \(n\ge0\).

The rounded tick source is itself filtered:

$$
g_0[n]
=
r_g[n]
$$

$$
g[n]
=
a_g g[n-1]
+
(1-a_g)g_0[n]
$$

with:

$$
a_g=e^{-1/(\tau_g f_s)}
$$

The tick therefore has a short rounded body rather than a single-sample click.

## Waxy wind-up/down

A separate event-local state controls short frequency excursions:

$$
w[n+1]
=
a_w w[n]
+
(1-a_w)w_{\infty}
$$

with event-specific:

$$
w_{\infty}
=
r_w
$$

and:

$$
a_w=e^{-1/(\tau_w f_s)}
$$

The resonant frequency becomes:

$$
f_r[n]
=
f_{\text{base}}
2^{
\left(
\Delta f q_b[n]
+
\Delta f_w w[n]
+
\Delta f_m m_b[n]
\right)/12
}
$$

This produces the short waxy rise/fall independently of the longer stochastic state.

## Rapid activity

The reaction runs a secondary event stream at twice the base subdivision:

$$
\Gamma_R = 2\Gamma
$$

The same deterministic hash mechanism is used:

$$
h_{i,R}
=
\mathrm{hash}(s_0,\mathrm{radiation},i)
$$

and:

$$
h_{i,R}<\rho_R
$$

The double-time activity therefore comes from additional short excitation events, not from simply turning the same event louder.

## Occasional high-frequency activity

A deterministic rare-event state is:

$$
r_h
=
\mathrm{hash}(s_0,\mathrm{radiationHF},i)
$$

A high-frequency event occurs when:

$$
r_h < p_{HF}
$$

For that event:

$$
f_{HF}
=
f_{\text{base}}
2^{\Delta f_{HF}}
$$

and:

$$
e_{HF}[n]
=
A_{HF}
e^{-n/\tau_{HF}}
\sin(\theta_{HF}[n])
$$

with:

$$
\theta_{HF}[n+1]
=
\theta_{HF}[n]
+
\frac{2\pi f_{HF}}{f_s}
$$

The oscillator is only active during the event.

## Output

The resonator output is the real part of the quadrature state:

$$
y_R[n]=v_{re}[n]
$$

The high-frequency event is mixed in:

$$
y[n]
=
G_R y_R[n]
+
G_{HF}e_{HF}[n]
$$

with:

$$
G_R+G_{HF}\le1
$$

before the shared output stage.

## Controls

| control               | mathematical term                 | effect                            | recommended range         |
| --------------------- | --------------------------------- | --------------------------------- | ------------------------- |
| SPREAD \(\Delta f\)   | frequency exponent                | stochastic pitch/filter excursion | ±2 to ±24 semitones       |
| DECAY \(\tau\)        | resonator/event decay             | persistence                       | short to medium           |
| EXPOSURE \(k\)        | resonator feedback/radius         | resonance intensity               | bounded below instability |
| TOXICITY \(\delta\)   | excitation saturation             | rounding/compression              | low to high               |
| REACTIVITY \(\alpha\) | \(\beta_m,\epsilon_p\)            | micro-event intensity             | 0–1                       |
| VOLATILITY \(\sigma\) | \(\tau_q,\tau_m\) and state depth | stochastic speed                  | low to high               |
| HALF-LIFE \(\lambda\) | \(a_q\)                           | state persistence                 | short to long             |
| ENRICHMENT \(\eta\)   | input excitation                  | source drive                      | ±18 dB                    |
| CONTAMINATION \(\nu\) | tick/noise gain                   | radioactive texture               | 0–1                       |
| FALLOUT \(\chi\)      | post-event stochastic gain        | settling dispersion               | 0–1                       |
| AFTERGLOW \(T_{60}\)  | tail state                        | residual resonance                | short to long             |

The EXPOSURE control must be mapped to a stable resonator-radius range rather than an arbitrary unbounded coefficient.

## Stability

The resonator requires:

$$
0\le r[n]<1
$$

at every sample.

The stochastic frequency must remain below Nyquist:

$$
0<f_r[n]<0.45f_s
$$

The actual bound should be clamped before coefficient calculation.

---

# FISSION — coupled branch system

## What it has to do

* phaser dissonance
* phase cancellation
* frequency splitting
* detune flanging
* feedback
* stereo separation wobbles
* metallic movement
* unstable harmonic relationships

## The mechanism

FISSION takes one incoming structure and splits it into multiple coupled branches.

The identity is the relationship between the branches.

Each branch has its own state, frequency and phase. The branches then interact through cross-feedback and are recombined with a moving relative phase.

A single filter with a different coefficient cannot reproduce this architecture.

## Branch frequencies

The two principal branches are centred around the same base frequency:

$$
f_1[n]
=
f_{\text{base}}
2^{d[n]/12}
$$

$$
f_2[n]
=
f_{\text{base}}
2^{-d[n]/12}
$$

where \(d[n]\) is the instantaneous detuning in semitones.

The detuning itself moves:

$$
d[n]
=
\Delta f
\left(
d_0+d_m m[n]
\right)
$$

where \(m[n]\) is a slow deterministic modulation state:

$$
m[n+1]
=
a_m m[n]
+
(1-a_m)r_m[n]
$$

and:

$$
a_m=e^{-1/(\tau_m f_s)}
$$

This makes the frequency split continuously diverge and reconverge.

## Branch resonators

Each branch uses a second-order resonator:

$$
v_1[n]
=
2r_1\cos(\omega_1[n])v_1[n-1]
-
r_1^2v_1[n-2]
+
u_1[n]
$$

$$
v_2[n]
=
2r_2\cos(\omega_2[n])v_2[n-1]
-
r_2^2v_2[n-2]
+
u_2[n]
$$

with:

$$
\omega_i[n]
=
\frac{2\pi f_i[n]}{f_s}
$$

and:

$$
r_i=e^{-\pi B_i/f_s}
$$

## Cross-feedback

The branch inputs are:

$$
u_1[n]
=
\eta x[n]
-
k_c v_2[n-1]
$$

$$
u_2[n]
=
\eta x[n]
-
k_c v_1[n-1]
$$

The branches therefore do not merely receive separate copies of the input.

Each branch modifies the other.

The coupling coefficient is:

$$
k_c
=
k_{c,\min}
+
E_k\,k_{\text{norm}}
$$

where \(E_k\) is derived from EXPOSURE.

The coupled system must satisfy a bounded feedback condition. In matrix form:

$$
\mathbf v[n]
=
\mathbf A[n]\mathbf v[n-1]
+
\mathbf Bx[n]
$$

The implementation must maintain:

$$
\rho(\mathbf A)<1
$$

where \(\rho\) is the spectral radius.

If the requested EXPOSURE exceeds the stable coupling region, the coefficient is clamped.

## Phase cancellation

The branches are recombined with relative phase:

$$
y[n]
=
w_1v_1[n]
+
w_2v_2[n]e^{j\phi[n]}
$$

For a real implementation, the equivalent phase relationship is produced by a short all-pass or delay relationship.

Using a fractional delay \(D[n]\):

$$
y[n]
=
w_1v_1[n]
+
w_2v_2[n-D[n]]
$$

The frequency-domain relationship contains:

$$
H(\omega)
=
w_1
+
w_2e^{-j\omega D}
$$

so cancellation occurs where:

$$
w_1
+
w_2e^{-j\omega D}
\approx0
$$

For equal branch weights:

$$
w_1=w_2
$$

deep cancellation occurs near:

$$
\omega D=(2m+1)\pi
$$

As \(D\) moves, the cancellation frequencies move.

That is the mathematical source of the phaser/flanger character.

## Moving branch delay

The delay is:

$$
D[n]
=
D_0
+
D_m m_D[n]
$$

with:

$$
D[n]\ge0
$$

and interpolation required for fractional sample positions.

The modulation state is:

$$
m_D[n+1]
=
a_Dm_D[n]
+
(1-a_D)r_D[n]
$$

This creates moving notches rather than static comb filtering.

## Stereo divergence

The left and right branch detunings use opposite signs:

$$
d_L[n]=d[n]
$$

$$
d_R[n]=-d[n]
$$

and the relative delay is also mirrored:

$$
D_L[n]
=
D_0+D_m m_D[n]
$$

$$
D_R[n]
=
D_0-D_m m_D[n]
$$

Therefore the stereo image moves because the branch relationships themselves diverge.

This is not a final pan.

## Metallic movement

The branch difference is:

$$
y_D[n]
=
v_1[n]-v_2[n]
$$

The branch sum is:

$$
y_S[n]
=
v_1[n]+v_2[n]
$$

The output mix is:

$$
y[n]
=
(1-\mu)y_S[n]
+
\mu y_D[n]
$$

where \(\mu\) is derived from REACTIVITY.

The difference component emphasises the dissonant relationship between branches.

## Controls

| control               | mathematical term         | effect                      | recommended range    |
| --------------------- | ------------------------- | --------------------------- | -------------------- |
| SPREAD \(\Delta f\)   | \(d[n]\)                  | branch frequency separation | 0–24 semitones       |
| DECAY \(\tau\)        | \(r_1,r_2\)               | branch persistence          | short to long        |
| EXPOSURE \(k\)        | \(k_c\)                   | cross-feedback              | stable coupled range |
| TOXICITY \(\delta\)   | branch saturation         | nonlinear metallic density  | low to high          |
| REACTIVITY \(\alpha\) | \(\mu\), modulation depth | branch divergence           | 0–1                  |
| FLUX \(\phi\)         | \(D_m\)                   | moving cancellation         | 0–1                  |
| VOLATILITY \(\sigma\) | \(m_D,m\) rate            | movement speed              | low to high          |
| HALF-LIFE \(\lambda\) | \(a_m,a_D\)               | branch-memory persistence   | short to long        |
| ENRICHMENT \(\eta\)   | branch input              | excitation                  | ±18 dB               |
| CONTAMINATION \(\nu\) | branch noise              | metallic texture            | 0–1                  |
| COLLIMATOR \(\theta\) | stereo branch mix         | narrows divergence          | 0–1                  |
| FALLOUT \(\chi\)      | branch-difference tail    | dispersal                   | 0–1                  |
| AFTERGLOW \(T_{60}\)  | branch decay              | lingering split             | short to long        |

## Stability

The coupled branch system must satisfy:

$$
\rho(\mathbf A)<1
$$

The delay must remain inside the allocated delay buffer:

$$
0\le D[n]\le D_{\max}
$$

The detuned frequencies must remain below Nyquist.

All fractional delays require interpolation with bounded gain.

---

# SLUDGE — nonlinear subharmonic reactor

## What it has to do

* thick low-mid resonance
* low-pass filtering
* viscous snap back
* lurching through molasses
* soft rounded saturation
* slow filter movement
* long resonant tails
* subharmonic and body enhancement
* heavy low-frequency pressure

## The mechanism

SLUDGE is not a low-pass filter with a slow envelope.

Its identity is:

1. generation of low-frequency/subharmonic energy,
2. asymmetric nonlinear processing,
3. long-timescale internal memory.

A low-pass filter can remove high-frequency energy but cannot create new energy below the lowest meaningful input component.

SLUDGE therefore contains an explicit subharmonic generator.

## Input body extraction

First derive a smooth measure of the incoming/reactor energy:

$$
e[n]
=
|x[n]|
$$

Then low-pass it:

$$
q[n]
=
q[n-1]
+
g_q(e[n]-q[n-1])
$$

with:

$$
g_q
=
1-e^{-2\pi f_q/f_s}
$$

This is the slow body state.

A second state provides faster snap response:

$$
r[n]
=
r[n-1]
+
g_r(q[n]-r[n-1])
$$

with:

$$
g_r
=
1-e^{-2\pi f_r/f_s}
$$

and:

$$
f_r>f_q
$$

The difference:

$$
s_{\text{snap}}[n]
=
q[n]-r[n]
$$

produces the viscous snap component.

## Long memory

The main reactor memory is:

$$
M[n]
=
a_M M[n-1]
+
(1-a_M)q[n]
$$

where:

$$
a_M
=
e^{-1/(\tau_M f_s)}
$$

and:

$$
\tau_M
\propto
\lambda
$$

This state remains after the immediate input changes.

The reaction therefore does not simply follow the incoming envelope.

## Slow cutoff movement

The low-pass cutoff follows the memory state:

$$
f_c[n]
=
f_{\text{base}}
2^{
\Delta f\,M_b[n]
}
$$

where:

$$
M_b[n]=\tanh(M[n])
$$

The cutoff therefore moves slowly as the reactor accumulates energy.

This is the source of the molasses-like movement.

## Asymmetric saturation

Unlike CHEMICAL's symmetric tanh, SLUDGE uses an asymmetric nonlinearity.

Define:

$$
z[n]
=
\eta x[n]
+
\beta_M M[n]
+
\beta_h h[n]
$$

Then:

$$
S(z)
=
\begin{cases}
\tanh(\delta_+ z), & z\ge0\\[4pt]
\gamma\,\tanh(\delta_- z), & z<0
\end{cases}
$$

where:

$$
\gamma\ne1
$$

creates positive/negative asymmetry.

The asymmetry introduces even-order components.

For small input:

$$
S(z)
\approx
c_1z+c_2z^2+c_3z^3+\cdots
$$

where:

$$
c_2\ne0
$$

when the transfer function is asymmetric.

This gives SLUDGE a thicker harmonic body than CHEMICAL's predominantly odd-order saturation.

## Subharmonic generation

The fundamental reactor frequency is derived from the body state:

$$
f_h[n]
=
f_{\text{reactor}}
2^{\Delta_h M_b[n]}
$$

\(f_{\text{reactor}}\) is SLUDGE's own reactor/body frequency. It is not assumed to equal the input fundamental, and SLUDGE does not perform pitch tracking.

The generated subharmonics are:

$$
f_{h1}[n]=\frac{f_h[n]}{2}
\qquad
f_{h2}[n]=\frac{f_h[n]}{4}
$$

Each division uses its own phase accumulator, wrapped in the phase domain that preserves its period.

For the /2 component:

$$
\theta_1[n+1]
=
\operatorname{wrap}_{4\pi}
\left(
\theta_1[n]
+
\frac{2\pi f_h[n]}{f_s}
\right)
$$

$$
h_1[n]
=
\sin\left(\frac{\theta_1[n]}{2}\right)
$$

For the /4 component:

$$
\theta_2[n+1]
=
\operatorname{wrap}_{8\pi}
\left(
\theta_2[n]
+
\frac{2\pi f_h[n]}{f_s}
\right)
$$

$$
h_2[n]
=
\sin\left(\frac{\theta_2[n]}{4}\right)
$$

The /2 oscillator requires a \(4\pi\) phase domain and the /4 oscillator requires an \(8\pi\) phase domain, relative to the parent increment. Wrapping either derived phase at \(2\pi\) destroys the intended subharmonic periodicity: the result is a rectified-looking waveform at the parent rate with a DC component, not an octave down.

The validation relationship is therefore:

$$
f_{\text{measured},1}\approx\frac{f_h}{2}
\qquad
f_{\text{measured},2}\approx\frac{f_h}{4}
$$
$$

The generated component is:

$$
h[n]
=
A_h[n]
\left(
w_1h_1[n]
+
w_2h_2[n]
\right)
$$

with amplitude controlled by reactor energy:

$$
A_h[n]
=
A_{h0}
+
\beta_h\tanh(q[n])
$$

The subharmonic is therefore generated from the reactor state rather than being simply an EQ boost.

## Phase continuity

The oscillator phases persist while the reactor remains active.

When the reactor is completely inactive, phase may be retained rather than reset:

$$
\theta_1[n_0^+]=\theta_1[n_0^-]
\qquad
\theta_2[n_0^+]=\theta_2[n_0^-]
$$

This prevents repeated phase resets from producing clicks. Each accumulator is retained in its own \(4\pi\) or \(8\pi\) domain.

## Resonant low-pass stage

The asymmetric nonlinear signal passes through a low-pass stage whose cutoff is determined by the slow memory:

$$
g_c[n]
=
1-e^{-2\pi f_c[n]/f_s}
$$

$$
s[n]
=
s[n-1]
+
g_c[n]
\left(
S(z[n])-s[n-1]
\right)
$$

The subharmonic is then mixed with the filtered body:

$$
y_0[n]
=
(1-\mu_h)s[n]
+
\mu_h h[n]
$$

## Viscous snapback

The faster state provides a temporary reverse movement:

$$
f_{\text{snap}}[n]
=
f_c[n]
2^{\Delta_{\text{snap}}s_{\text{snap}}[n]}
$$

\(s_{\text{snap}}[n]\) is \(q[n]-r[n]\), already defined above. It needs no
separate decaying state: because \(r\) tracks faster than \(q\)
(\(f_r>f_q\)), a change in input level makes \(r\) move first and \(q\)
follow, so the difference rises and falls on its own as \(q\) catches back up
to \(r\). An independent exponential state here would have no excitation term
feeding it and would simply decay to zero regardless of input, which is why
that formulation has been dropped in favour of using \(q[n]-r[n]\) directly.

This makes the cutoff briefly recoil before settling back into the slow memory state.

## Long resonant tail

The final output contains a persistent tail state:

$$
T[n]
=
a_TT[n-1]
+
(1-a_T)y_0[n]
$$

where:

$$
a_T
=
e^{-1/(\tau_Tf_s)}
$$

and:

$$
\tau_T
\propto
T_{60}
$$

The tail gain is reduced as the reaction settles:

$$
G_T[n]
=
\chi\,e^{-t/T_{60}}
$$

giving:

$$
y[n]
=
y_0[n]
+
G_T[n]T[n]
$$

## Controls

| control               | mathematical term            | effect                          | recommended range   |
| --------------------- | ---------------------------- | ------------------------------- | ------------------- |
| SPREAD \(\Delta f\)   | \(f_c,f_h\) excursion        | spectral/body movement          | ±2 to ±18 semitones |
| DECAY \(\tau\)        | event/subharmonic envelope   | event persistence               | short to long       |
| EXPOSURE \(k\)        | \(\beta_M,\beta_h\)          | reactor excitation/memory depth | 0–1                 |
| TOXICITY \(\delta\)   | \(\delta_+,\delta_-\)        | asymmetric saturation           | low to high         |
| REACTIVITY \(\alpha\) | snap/subharmonic modulation  | lurch intensity                 | 0–1                 |
| HALF-LIFE \(\lambda\) | \(\tau_M\)                   | long memory                     | short to very long  |
| ENRICHMENT \(\eta\)   | input drive                  | source energy                   | ±18 dB              |
| CONTAMINATION \(\nu\) | nonlinear/body contamination | thickness                       | 0–1                 |
| FALLOUT \(\chi\)      | \(G_T\)                      | tail dispersion                 | 0–1                 |
| AFTERGLOW \(T_{60}\)  | \(\tau_T\)                   | tail duration                   | short to long       |

## Stability

The asymmetric saturation must remain bounded:

$$
|S(z)|\le1
$$

The subharmonic oscillator must satisfy:

$$
|A_h[n]|\le A_{h,\max}
$$

The low-pass coefficients must satisfy:

$$
0<g_c,g_q,g_r<1
$$

The tail state must decay:

$$
0<a_T<1
$$

The subharmonic generator must not become an uncontrolled DC accumulator.

---

# ALIEN — event-local oscillator reactor

## What it has to do

* short zaps
* pitch sweeps
* resonant chirps
* FM and AM bursts
* rapid pitch jumps
* strange high-frequency accents
* unpredictable event timing

## The mechanism

ALIEN is the one reaction that contains an actual source oscillator.

It does not require incoming audio to create its primary sound.

Its identity is:

* event-local oscillator
* FM
* AM
* discrete pitch transitions
* reaction-specific deterministic timing

The input can influence the oscillator, but the oscillator remains an independent source.

## Event frequency

Each event receives a deterministic pitch selection:

$$
r_i
=
2\,
\mathrm{hash}(s_0,\mathrm{alienPitch},i)-1
$$

Map it to a semitone jump:

$$
d_i
=
D_{\max}r_i
$$

and:

$$
f_{0,i}
=
f_{\text{base}}
2^{d_i/12}
$$

The frequency may jump immediately at event onset.

A pitch sweep uses:

$$
f_c[n]
=
f_{0}
2^{
\Delta_{\text{sweep}}
(1-e^{-n/\tau_f})
/12
}
$$

This produces an exponential pitch movement rather than a linear Hz sweep.

## Carrier oscillator

ALIEN contains an actual oscillator, but oscillator output is event-gated. A scheduled reaction event may excite the oscillator even when the input is silent. In the absence of a reaction event, ALIEN does not continuously free-run.

$$
\text{silence + event} \rightarrow \text{ALIEN oscillator burst}
$$

$$
\text{silence + no event} \rightarrow \text{no continuous ALIEN oscillator output}
$$

The event amplitude combines the scheduled event with the input energy measure \(E_x\):

$$
A_i = A_{\text{event},i} + g_x E_x(t_i)
$$

so an event fires from silence, and a loud input drives it harder.

The oscillator phase is:

$$
\theta_c[n+1]
=
\theta_c[n]
+
\frac{2\pi f_c[n]}{f_s}
$$

The carrier waveform is:

$$
c[n]
=
\sin(\theta_c[n])
$$

## FM oscillator

The modulation phase is:

$$
\theta_m[n+1]
=
\theta_m[n]
+
\frac{2\pi f_m[n]}{f_s}
$$

The modulator is:

$$
m[n]
=
A_m[n]\sin(\theta_m[n])
$$

The FM phase becomes:

$$
\theta_{\text{FM}}[n]
=
\theta_c[n]
+
\beta[n]m[n]
$$

and:

$$
z_{\text{FM}}[n]
=
\sin(\theta_{\text{FM}}[n])
$$

where \(\beta\) is modulation index.

## FM decay

The modulation index decays within each event:

$$
\beta[n]
=
\beta_0
e^{-n/\tau_\beta}
$$

This gives a bright initial zap that collapses toward the carrier.

## AM burst

AM uses a separate modulation state:

$$
a[n]
=
1+
\mu[n]
m_a[n]
$$

where:

$$
m_a[n]
=
\frac{1}{2}
\left(
1+\sin(\theta_a[n])
\right)
$$

and:

$$
\theta_a[n+1]
=
\theta_a[n]
+
\frac{2\pi f_a}{f_s}
$$

The AM output is:

$$
z_{\text{AM}}[n]
=
a[n]z_{\text{FM}}[n]
$$

FM therefore controls instantaneous phase/frequency while AM independently controls amplitude.

## Event envelope

Each oscillator event has an attack/release envelope.

For the short attack:

$$
E_a[n]
=
1-e^{-n/\tau_a}
$$

For release:

$$
E_r[n]
=
e^{-n/\tau_r}
$$

Combined:

$$
E[n]
=
E_a[n]E_r[n]
$$

The oscillator output becomes:

$$
y_e[n]
=
A_e E[n]z_{\text{AM}}[n]
$$

This produces a finite zap rather than a continuously running synthesizer.

## Resonant chirp emphasis

A short high-frequency resonant emphasis is derived from the oscillator frequency:

$$
f_z[n]
=
f_c[n]2^{\Delta z/12}
$$

and:

$$
z_z[n]
=
E_z[n]\sin(\theta_z[n])
$$

with:

$$
\theta_z[n+1]
=
\theta_z[n]
+
\frac{2\pi f_z[n]}{f_s}
$$

and:

$$
E_z[n]
=
A_z e^{-n/\tau_z}
$$

This provides the high-frequency accent without requiring the main oscillator to remain at an excessively high carrier frequency.

## Reaction-specific timing

ALIEN augments the shared sequencer with a deterministic hazard state.

Define:

$$
\lambda_A[n]
=
\lambda_0
\left(
1+
\alpha_\lambda q[n]
\right)
$$

where:

$$
q[n+1]
=
a_q q[n]
+
(1-a_q)r_q[n]
$$

The event probability over a discrete interval is:

$$
P_i
=
1-e^{-\lambda_A[i]\Delta t}
$$

A deterministic hash decides the event:

$$
h_i
=
\mathrm{hash}(s_0,\mathrm{alienTiming},i)
$$

and:

$$
h_i<P_i
$$

fires the event.

The hazard can therefore cluster events after a high-reactivity event and then decay:

$$
q[n]
\rightarrow
q[n]e^{-1/(\tau_qf_s)}
$$

This creates unpredictable timing while remaining exactly reproducible.

## Input interaction

The incoming audio may excite the oscillator's amplitude:

$$
A_e[n]
=
A_0
+
\beta_x|x[n]|
$$

but the oscillator remains active without input.

This is the defining architectural difference from the other reactions.

## Stereo

The oscillator phase may be offset between channels:

$$
\theta_L[n]=\theta[n]
$$

$$
\theta_R[n]
=
\theta[n]+\phi_s
$$

The stereo offset is derived from IONIZE and COLLIMATOR:

$$
\phi_s
=
\pi\iota(1-\theta)
$$

The output is:

$$
y_L[n]
=
E[n]A_e[n]
\sin(\theta_L[n]+\beta m[n])
$$

$$
y_R[n]
=
E[n]A_e[n]
\sin(\theta_R[n]+\beta m[n])
$$

## Controls

| control               | mathematical term           | effect                      | recommended range    |
| --------------------- | --------------------------- | --------------------------- | -------------------- |
| SPREAD \(\Delta f\)   | carrier pitch / sweep       | pitch range                 | ±24 semitones        |
| DECAY \(\tau\)        | \(E_r,\tau_\beta\)          | event duration              | very short to medium |
| EXPOSURE \(k\)        | \(A_e,\beta_0\)             | oscillator intensity        | 0–1                  |
| TOXICITY \(\delta\)   | FM index / nonlinear output | spectral aggression         | low to high          |
| REACTIVITY \(\alpha\) | \(\beta,\lambda_A\)         | FM/timing instability       | 0–1                  |
| PROBABILITY \(\rho\)  | base hazard                 | event density               | 0–1                  |
| FLUX \(\phi\)         | event timing deviation      | temporal displacement       | 0–1                  |
| VOLATILITY \(\sigma\) | hazard variation            | timing unpredictability     | 0–1                  |
| HALF-LIFE \(\lambda\) | hazard recovery             | clustered-event persistence | short to long        |
| ENRICHMENT \(\eta\)   | oscillator/input amplitude  | source energy               | ±18 dB               |
| CONTAMINATION \(\nu\) | secondary accent/noise      | texture                     | 0–1                  |
| COLLIMATOR \(\theta\) | stereo phase                | stereo width                | 0–1                  |
| FALLOUT \(\chi\)      | post-event gain             | residual dispersal          | 0–1                  |
| AFTERGLOW \(T_{60}\)  | post-event envelope         | lingering tail              | short to long        |

## Stability

The phase accumulator must remain finite by wrapping:

$$
\theta
\leftarrow
\theta\bmod 2\pi
$$

This applies to ALIEN's carrier and modulator phases, which are not derived by division. SLUDGE's subharmonic accumulators are wrapped at \(4\pi\) and \(8\pi\) instead, because wrapping a divided phase at \(2\pi\) destroys its period.

Carrier and modulator frequencies must satisfy:

$$
0<f<f_s/2
$$

Practical anti-alias limits should be lower than Nyquist when FM index is large.

The oscillator amplitude must satisfy:

$$
|A_e[n]E[n]|\le A_{\max}
$$

FM modulation must be bounded:

$$
|\beta[n]m[n]|\le\beta_{\max}
$$

The hazard probability must satisfy:

$$
0\le P_i\le1
$$

---

# Notation

Voice is everything inside the reaction. Structure is everything in the sequencer. Nothing appears in both.

Bracketed glyphs are labels rather than quantities: they name a control that does not appear as a term in an equation.

## Structure — what fires, and when

* \((\mathcal R)\) REACTION — which of the five
* \((\mathcal M)\) MODE — GRID / RANDOM / FREE / INPUT
* \((\Gamma)\) GRID — 1/1 … 1/64
* \((s_0)\) SEED — which pattern the hash produces
* \(\rho\) PROBABILITY — chance a step fires
* \(\phi\) FLUX — timing jitter either side of the grid
* \(\sigma\) VOLATILITY — scatter in time and stereo
* \(\lambda\) HALF-LIFE — how far one event carries into the next
* \(\kappa\) CONTAINMENT — closes the vessel, thinning events out

## Voice — the shape of each event

* \(\Delta f\) SPREAD — how far the cutoff/pitch moves
* \(\tau\) DECAY — how long the event takes to settle
* \(k\) EXPOSURE — reaction-specific excitation/coupling/feedback intensity
* \(\delta\) TOXICITY — reaction-specific nonlinear/spectral intensity
* \(\alpha\) REACTIVITY — depth of reaction-specific perturbation

The same control therefore does not necessarily appear as the same equation in every reaction. Its semantic role is shared; its mathematical implementation is reaction-specific.

## Colour — what surrounds it

* \(\eta\) ENRICHMENT — how hard the source hits the reaction, ±18 dB
* \(\nu\) CONTAMINATION — noise bed, textured by the REACTION
* \((\theta)\) COLLIMATOR — narrows the stereo field
* \((\chi)\) FALLOUT — disperses the reaction as it settles
* \((T_{60})\) AFTERGLOW — how long it keeps glowing

## Gestures and output

* \((\iota)\) IONIZE — latched: scatters each event in stereo, spectrum and depth
* \((\Omega)\) MELTDOWN — momentary: staged runaway
* \((\lceil\rceil)\) CLIP — hard ceiling instead of the limiter, latency padded to match

---

# Reaction architecture audit

| reaction  | unique mathematical operation                                                                 | shared mechanisms allowed                                    | forbidden shortcut                                                                              |
| --------- | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------ | ----------------------------------------------------------------------------------------------- |
| CHEMICAL  | nonlinear resonant feedback ladder + discrete event-held stochastic register                  | sequencer, envelopes, deterministic modulation, output stage | replacing ladder resonance with a static biquad; continuous correlated stochastic modulation    |
| RADIATION | correlated stochastic state modulation around a quadrature resonator                          | resonant excitation, envelopes, shared sequencer             | CHEMICAL ladder with random cutoff values; discrete event-held stochastic register              |
| FISSION   | multiple coupled branches, moving phase cancellation and cross-feedback                       | resonators, deterministic modulation, stereo processing      | one filter with a bandpass tap; stereo/detune modulation of a single path                       |
| SLUDGE    | generated subharmonics, asymmetric nonlinearity and long memory                                | low-pass processing, envelopes, deterministic modulation     | low-pass-only processing or symmetric CHEMICAL saturation                                       |
| ALIEN     | event-gated oscillator with FM/AM and reaction-specific timing                                | envelopes, sequencer, stereo processing                      | filtering the input and calling it an oscillator; resonator pitch modulation in place of a source |

The CHEMICAL and RADIATION rows are deliberately mutually exclusive. Each reaction's stochastic mechanism is the other's forbidden shortcut, so the two cannot converge on a single implementation.

These are five different DSP architectures.

Shared controls parameterise them; they do not define their identity.

CHEMICAL owns the ladder.

RADIATION owns correlated stochastic resonator modulation.

FISSION owns branch splitting and interaction.

SLUDGE owns nonlinear low-frequency generation and memory.

ALIEN owns source generation through an actual oscillator.

---

# Implementation invariants

The following properties must hold across all reactions.

## Determinism

For identical:

* input
* sample rate
* transport position
* seed
* controls
* plugin state

the rendered output must be identical.

All stochastic decisions therefore derive from deterministic hash streams.

## Bounded internal states

Any state that can accumulate without bound must have either:

* a mathematically bounded recurrence,
* a stable decay term,
* or an explicit safety bound.

## Sample-rate independence

Any time-based coefficient must derive from seconds and \(f_s\), for example:

$$
a=e^{-1/(\tau f_s)}
$$

Do not hard-code a coefficient whose behaviour changes with sample rate.

## Event smoothing

Parameter discontinuities that would create clicks must be smoothed independently of musical envelopes.

The anti-click smoothing constant must not replace:

* slide time,
* reaction decay,
* stochastic state half-life,
* oscillator envelope,
* long-term memory.

These are different physical behaviours.

## Anti-aliasing

Any generated oscillator, subharmonic, FM signal or rapidly changing nonlinear process must be evaluated with an appropriate anti-aliasing strategy in the implementation.

At minimum, carrier frequencies and modulation excursions must be bounded.

For strongly nonlinear generated content, oversampling or a band-limited oscillator/waveshaper should be used where required.

## Output safety

Every reaction must remain bounded before the shared output stage.

The final shared output stage may apply the configured CLIP or limiter behaviour, but individual reaction engines must not rely on clipping as their normal operating mechanism.

---

# MELTDOWN

MELTDOWN is a gesture, not a sixth reaction.

It temporarily pushes the currently selected reaction toward its instability boundary.

The gesture modifies the reaction's existing internal mechanism rather than replacing it.

For a generic bounded reaction parameter \(p\):

$$
p_{\Omega}(t)
=
p_0
+
\Omega(t)
\left(
p_{\max}-p_0
\right)
$$

where:

$$
0\le\Omega(t)\le1
$$

The exact \(p\) affected is reaction-specific:

* CHEMICAL → feedback proximity to \(k_{\text{crit}}\)
* RADIATION → stochastic state depth and resonator excitation
* FISSION → branch coupling
* SLUDGE → memory/subharmonic drive
* ALIEN → oscillator/FM/timing intensity

MELTDOWN therefore amplifies the identity of the selected reaction instead of introducing another processing architecture.

---

# IONIZE

IONIZE is a latched spatial/spectral scattering operation applied after the reaction has generated its core sound.

It does not define a reaction.

A deterministic per-event scatter value is generated:

$$
r_i
=
2\,
\mathrm{hash}(s_0,\mathrm{ionize},i)-1
$$

Stereo displacement:

$$
p_i
=
\iota r_i
$$

Spectral displacement:

$$
d_i
=
\iota\Delta f r_i
$$

Depth/gain displacement:

$$
g_i
=
1+\iota r_i
$$

The scattering remains deterministic for a fixed seed.

---

# CONTAMINATION

CONTAMINATION provides a reaction-specific contamination bed.

The bed is not allowed to define the reaction.

Let:

$$
n[n]
$$

be a bounded noise source.

The reaction-specific texture is:

$$
c[n]
=
F_{\mathcal R}(n[n])
$$

where \(F_{\mathcal R}\) is determined by the selected reaction.

Then:

$$
y_{\text{mix}}[n]
=
y_{\mathcal R}[n]
+
\nu c[n]
$$

The contamination level therefore surrounds the reaction rather than replacing its defining mechanism.

---

# FALLOUT / AFTERGLOW

When an event ends, the reaction may continue through its internal state.

The post-event gain is:

$$
G_{\text{fallout}}(t)
=
\chi e^{-t/\tau_F}
$$

and the afterglow persistence is:

$$
\tau_F
\propto
T_{60}
$$

The important distinction is that AFTERGLOW extends the reaction's existing state.

It must not turn every reaction into the same generic reverb or delay.

---

# Final architectural rule

The plugin contains five reaction engines.

They share:

* event scheduling,
* deterministic randomness,
* controls,
* output handling,
* transport,
* stereo/output infrastructure.

They do **not** share a single DSP core.

The defining mathematical operations are:

$$
\boxed{
\begin{aligned}
\text{CHEMICAL} &:\quad
\text{nonlinear resonant feedback ladder}\\
\text{RADIATION} &:\quad
\text{correlated stochastic resonator modulation}\\
\text{FISSION} &:\quad
\text{coupled branches + cancellation + cross-feedback}\\
\text{SLUDGE} &:\quad
\text{subharmonic generation + asymmetric nonlinearity + memory}\\
\text{ALIEN} &:\quad
\text{event oscillator + FM/AM + stochastic timing}
\end{aligned}
}
$$

Changing SPREAD, DECAY, EXPOSURE or TOXICITY must never be sufficient to turn one reaction into another.

The mathematical architecture is the identity.
