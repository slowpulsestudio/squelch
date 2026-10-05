# DSP validation and output testing

The implementation is not complete when it compiles.

After implementing the maths, build a repeatable test harness that renders deterministic audio through each reaction and analyses the resulting WAV files.

The purpose is to establish:

1. the DSP is numerically stable;
2. the implementation follows the equations;
3. the controls actually affect the intended mechanisms;
4. the five reactions are structurally and sonically different;
5. deterministic rendering works;
6. sample rate and block size do not materially change the behaviour;
7. no reaction silently collapses into another reaction;
8. the generated output is safe and bounded.

Do not judge success only by "it sounds interesting".

---

# Measurement principles

These are not advice. Each one is a mistake this suite actually made, found by
deliberately reintroducing a fixed bug and watching the test pass anyway.

**1. Use a behavioural oracle, not a duplicated implementation formula.**

A test that independently reconstructs the implementation's equation can verify
arithmetic, but cannot detect a shared semantic error: the test and the code
assume the same wrong thing and the broken engine passes. Two regressions
written here to pin a normalisation reimplemented the formula themselves and
caught nothing. Read the state the engine is actually running — expose an
accessor if you have to. Test the equation separately if it is worth testing,
but never as the sole oracle.

**2. Use the measurement domain appropriate to the invariant.**

| the invariant is about | measure |
|---|---|
| audible or output behaviour | the audio render |
| persistence, memory, topology | the internal state |
| monotonicity and range | a parameter sweep |
| unintended change | the golden render |

**3. Do not use golden audio to prove a structurally weakly observable property.**

Reverting SLUDGE's snap to a cascade is a real topology change that moves the
render by under 0.01 dB — inside the golden render's own tolerance. That is not
a weakness in the golden test. It is telling you the invariant lives at the
structural level and audio is the wrong domain for it. Instrument the state.

**4. The observation window and starting conditions are part of the
measurement specification.**

They are not incidental test implementation. A finite window has to be chosen
against the time constant it is measuring — a fixed 2 s window reads a 10 s
exponential as a deviation of 0.138 against a true 0.577 — and a capture cannot
begin while the state is still being driven, or what is measured is
`state decay ⊗ input decay`. Settle, capture, fit, with both windows scaled to
the thing being measured.

**5. Fault injection is a test of the test suite.**

Getting the current build green proves the code passes. Deliberately restoring
a known bug and requiring the named regression to fail proves the suite would
notice if it stopped passing. Those are different claims and only the second is
worth much. `scripts/fault-injection.py` does it on demand.

Keep implementation regression coverage and specification coverage apart.
Fault injection establishes the first. It says nothing about a requirement
nothing has ever broken — which has therefore never been tested either. The
second needs its own map, and `SquelchValidate` prints one.

---

# Test inputs

Generate deterministic test signals.

At minimum:

* silence
* impulse
* single sine at 100 Hz
* single sine at 440 Hz
* single sine at 1 kHz
* single sine at 5 kHz
* logarithmic sine sweep
* white noise
* low-frequency sine
* harmonic-rich sawtooth
* stereo anti-phase signal
* stereo identical signal

Use fixed seeds for every test.

Use at least:

* 44.1 kHz
* 48 kHz
* 96 kHz

Use at least:

* 32 samples/block
* 64
* 128
* 256
* 512

The output should remain deterministic across block sizes.

---

# Test 1 — build and plugin validity

Before audio tests:

* build Debug;
* build Release;
* instantiate the plugin;
* initialise at each supported sample rate;
* process audio with each supported block size;
* change every parameter through its legal range;
* reset/reinitialise the processor;
* save and restore state if the plugin supports state persistence.

Run plugin validation where available.

A plugin validator such as `pluginval` can be used for general plugin stability and host compatibility.

A failure here blocks the DSP output tests.

---

# Test 2 — finite-output test

Process:

* silence;
* impulse;
* sine;
* noise;
* maximum legal input.

Assert for every output sample:

$$
x[n]\ne\mathrm{NaN}
$$

$$
x[n]\ne\pm\infty
$$

and:

$$
|x[n]|<X_{\max}
$$

before the intentional CLIP stage.

Report:

* maximum absolute sample;
* RMS;
* DC offset;
* number of clipped samples;
* number of NaNs;
* number of infinities.

A failure indicates instability or an implementation error.

---

# Test 3 — deterministic rendering

Render exactly the same test twice with:

* identical input;
* identical sample rate;
* identical block size;
* identical transport;
* identical seed;
* identical parameters.

Compare the resulting files sample-for-sample.

The expected difference is:

$$
\max_n |y_1[n]-y_2[n]|=0
$$

or the smallest difference justified by the host's floating-point environment.

Also test:

* same project rendered twice;
* same event sequence after stopping/restarting;
* same bar bounced independently.

The hash-based event system must reproduce the same event pattern.

Identical input, seed, parameters, sample rate and processing configuration must reproduce the same deterministic result.

Host block size must not alter sample-domain reaction state. Any permitted difference across block sizes must originate only from explicitly documented control-rate processing, never from the reaction mechanism itself.

---

# Test 4 — block-size invariance

Render the same reaction with:

* 32;
* 64;
* 128;
* 256;
* 512

sample blocks.

The output must not change materially merely because the host chose a different block size.

Distinguish the two kinds of state:

* sample-domain reaction state must remain sample-accurate regardless of block size;
* control-domain state may update at a control interval, but only where that is explicitly documented.

The test exists to detect whether block size changes the reaction *mechanism*, not to require that every floating-point sample is identical under arbitrary control-rate scheduling.

Instead calculate:

* RMS difference;
* peak difference;
* spectral difference;
* event timing difference.

Large differences indicate that the DSP incorrectly depends on block boundaries.

---

# Test 5 — sample-rate invariance

Render at:

* 44.1 kHz;
* 48 kHz;
* 96 kHz.

Time-based behaviours must remain approximately constant in seconds.

For example, if:

$$
a=e^{-1/(\tau f_s)}
$$

then the measured decay time should remain approximately \(\tau\) seconds at every sample rate.

Measure:

* event duration;
* decay time;
* resonant tail;
* oscillator pitch;
* subharmonic pitch;
* stochastic state recovery.

A coefficient accidentally hard-coded in samples should fail this test.

---

# Test 6 — CHEMICAL validation

Use an impulse and harmonic-rich input.

Verify:

* resonant response exists;
* resonance increases with EXPOSURE;
* the resonant frequency follows SPREAD;
* DECAY changes event duration;
* TOXICITY changes nonlinear harmonic content;
* ENRICHMENT changes excitation;
* feedback remains bounded below the stability limit;
* the ladder can continue ringing after the input transient;
* a spectral hole in the input does not completely prevent the resonator from producing output.

Measure harmonic distortion at several EXPOSURE values.

For a sine input, calculate:

$$
THD=
\frac{
\sqrt{
A_2^2+A_3^2+\cdots+A_N^2
}
}{
A_1
}
$$

The harmonic content should increase as nonlinear resonance becomes stronger.

Also verify that CHEMICAL's output changes materially when EXPOSURE crosses its useful operating range.

## Stochastic register diagnostic

CHEMICAL's randomness is a discrete, event-held register. Inspect the register itself, not only the audio.

For each CHEMICAL event:

1. identify the event's \(q_i\);
2. verify \(q_i\) remains constant for the whole event interval;
3. verify that changing \(q_i\) changes the register, meaning where the sweep occurs;
4. verify that changing \(q_i\) does not remove, scale or reverse the sweep;
5. verify the same seed reproduces the same register sequence.

The test must distinguish register variation from sweep-depth variation.

Expected behaviour:

> Changing the stochastic register changes where the sweep occurs, not whether the full sweep occurs.

This guards against the incorrect formulation:

$$
2^{\Delta f q_i e^{-a/\tau}}
$$

and supports the intended one:

$$
2^{R_q q_i}\,2^{\Delta f e^{-a/\tau}}
$$

A correct implementation shows piecewise-constant \(q[n]\) between reaction events. Continuously evolving \(q[n]\) means CHEMICAL has been built with RADIATION's mechanism.

---

# Test 7 — RADIATION validation

RADIATION must demonstrate that the stochastic state is actually doing work.

Render the same input with:

* VOLATILITY = minimum;
* VOLATILITY = maximum.

The output should show substantially different short-timescale spectral movement.

Measure:

* spectral centroid variance;
* instantaneous resonant-frequency movement;
* event-to-event correlation;
* resonator decay;
* high-frequency event rate.

The stochastic state must be correlated.

Calculate the autocorrelation of the modulation state:

$$
R_q[k]
=
E[q[n]q[n-k]]
$$

A white-noise-like state with approximately zero correlation at all non-zero lags fails the intended mechanism.

Test determinism separately:

same seed → identical output.

Different seed → different stochastic trajectory.

The same seed must not merely produce the same average spectrum; it must reproduce the same rendered waveform.

Verify that low-level Geiger-like events are discrete and rounded rather than full-band white-noise bursts.

Verify that occasional high-frequency activity is sparse rather than continuously present.

## Stochastic state diagnostic

Inspect RADIATION's stochastic state directly. It must be continuous and correlated, not an event-held register.

Verify:

* the state evolves sample to sample, with no intervals of constant value spanning a whole event;
* the state exhibits temporal correlation, \(R_q[k]\neq0\) for small non-zero \(k\);
* the state is not piecewise constant between reaction events;
* the same seed reproduces the same state trajectory;
* a different seed produces a different trajectory.

Piecewise-constant state means RADIATION has been built with CHEMICAL's event-held register.

## CHEMICAL / RADIATION discrimination

Run the two state diagnostics against each other on the same seed and pattern. This matters more than comparing output spectra, because two different architectures can be EQ-matched to look similar while remaining different instruments.

Expected:

$$
\mathrm{CHEMICAL}: q[n] \text{ is piecewise constant between events}
$$

$$
\mathrm{RADIATION}: q[n] \text{ is continuously correlated}
$$

If both states have the same character, the two reactions have converged regardless of how different they sound.

---

# Test 8 — FISSION validation

FISSION must demonstrate that the branches are actually interacting.

Run three tests:

### Branch A

Disable coupling:

$$
k_c=0
$$

### Branch B

Normal coupling.

### Branch C

High but stable coupling.

The output spectrum and phase relationship must change materially between these cases.

Then disable detuning:

$$
d=0
$$

and compare with normal SPREAD.

The normal version must exhibit frequency splitting and moving interference that disappear or reduce strongly when the branches coincide.

Measure:

* spectral notches;
* notch movement;
* stereo correlation;
* left/right phase difference;
* branch beating;
* output difference between coupled and uncoupled states.

A FISSION implementation that produces almost the same output when branch coupling and detuning are disabled has failed the architectural test.

Also test:

$$
d_L=-d_R
$$

and verify that stereo movement exists because of branch relationships rather than a simple final pan.

---

# Test 9 — SLUDGE validation

SLUDGE must demonstrate genuine low-frequency generation.

SLUDGE's subharmonics derive from its own reactor frequency \(f_h\), not from the input fundamental. It performs no pitch tracking, so the test is referenced to \(f_h\).

Configure a known reactor frequency \(f_h\), enable the subharmonic paths, and measure the output spectrum.

Verify:

$$
f_{\text{measured},1}\approx\frac{f_h}{2}
$$

$$
f_{\text{measured},2}\approx\frac{f_h}{4}
$$

according to the configured subharmonic amplitudes \(w_1\) and \(w_2\).

A simple low-pass implementation cannot satisfy this test, because the generated component is present at frequencies the input never contained.

Compare:

* input spectrum;
* SLUDGE output spectrum.

The generated subharmonic must be present even when the input contains no corresponding low-frequency component.

Verify the phase domains explicitly: a /2 oscillator wrapped at \(2\pi\) instead of \(4\pi\) produces energy at \(f_h\) with a DC offset rather than at \(f_h/2\). Measuring the ratio of energy at \(f_h/2\) against \(f_h\) catches this.

Test TOXICITY.

Measure even-order harmonics:

$$
H_2,H_4,H_6,\ldots
$$

and odd-order harmonics:

$$
H_3,H_5,H_7,\ldots
$$

The asymmetric nonlinear stage should produce measurable even-order content.

Test HALF-LIFE.

Measure the persistence of the internal body state after input energy stops.

Test SNAPBACK.

Feed a short transient and measure whether the cutoff/body trajectory overshoots or recoils before returning toward its long-term state.

A simple envelope follower with no persistent internal state should fail the memory test.

---

# Test 10 — ALIEN validation

ALIEN must prove that it contains an actual source oscillator, and that the oscillator is event-gated rather than free-running.

Render:

* silence input;
* ALIEN enabled;
* a fixed event pattern.

ALIEN must produce output despite the input being silent.

Then render silence with no scheduled events. ALIEN must produce no continuous oscillator output. An instance sitting on a silent track with nothing scheduled must be silent.

$$
\text{silence + event} \rightarrow \text{oscillator burst}
$$

$$
\text{silence + no event} \rightarrow \text{no continuous output}
$$

That output should contain identifiable oscillator frequency.

Estimate pitch from zero crossings or spectral peak.

Verify:

$$
f_{\text{measured}}
\approx
f_{\text{specified}}
$$

within a reasonable tolerance.

Test FM by comparing:

$$
\beta=0
$$

against:

$$
\beta>0
$$

The latter must introduce sidebands around the carrier.

For a sinusoidal FM signal, verify the expected sideband structure around:

$$
f_c+kf_m
$$

for integer \(k\), subject to the envelope and finite event duration.

Test AM separately.

With AM disabled, the amplitude modulation sidebands should disappear or substantially reduce.

Test pitch jumps.

Two events with different deterministic pitch selections must produce different carrier frequencies without requiring different input frequencies.

Test reaction-specific timing.

With the same seed:

$$
\text{render}_1=\text{render}_2
$$

With different seeds:

$$
\text{timing}_1\ne\text{timing}_2
$$

with high probability.

---

# Test 11 — five-reaction differentiation test

This is a mandatory test.

It runs at two points in the signal path, because a shared output stage can re-converge five different architectures.

### A. Reaction domain

$$
y_R^{raw}[n]
$$

the reaction engine output, before common presentation and output processing.

### B. Product domain

$$
y_R^{final}[n] = P_R\!\left(y_R^{raw}[n]\right)
$$

the final plugin output.

> Raw-domain analysis proves architectural differentiation. Final-domain analysis verifies that the common output stage does not erase the reaction identities.

A fixed shared EQ curve and a common level target will pull distinct architectures toward a common spectrum. Passing in the raw domain and failing in the product domain means the output stage is the problem, not the reactions.

Render the same:

* input;
* pattern;
* seed;
* SPREAD;
* DECAY;
* EXPOSURE;
* TOXICITY;
* REACTIVITY;
* ENRICHMENT

through all five reactions.

Calculate for each output:

* RMS;
* crest factor;
* spectral centroid;
* spectral spread;
* spectral flatness;
* spectral rolloff;
* zero-crossing rate;
* harmonic ratios;
* stereo correlation;
* temporal envelope;
* spectral flux.

Do not require the five reactions to have arbitrary numerical separation.

Instead use this test to detect accidental convergence.

If changing REACTION while holding all controls fixed produces nearly identical outputs, investigate.

Measure both domains. The general spectral and statistical metrics above are supporting evidence; the mechanism-specific signatures below are the actual evidence.

The following mechanism-specific signatures should be detectable:

### CHEMICAL

* strong resonant peak;
* nonlinear harmonic generation;
* sustained feedback ring;
* event-held register behaviour, with \(q[n]\) piecewise constant between events.

### RADIATION

* correlated rapid spectral movement;
* continuous correlated stochastic resonator movement, with \(q[n]\) evolving sample to sample;
* short irregular micro-events.

### FISSION

* moving spectral cancellation;
* paired frequency structures from coupled-branch detuning;
* stereo branch divergence;
* material change when coupling or detuning is disabled.

### SLUDGE

* generated energy near \(f_h/2\) and \(f_h/4\);
* strong even-order nonlinear content;
* long memory.

### ALIEN

* oscillator output with no required input, under a scheduled event;
* no continuous output when no event is scheduled;
* FM sidebands;
* AM structure;
* discrete pitch/event changes.

---

# Test 12 — control sensitivity

Every user-facing control must actually affect something.

For each reaction:

1. render at minimum;
2. render at midpoint;
3. render at maximum.

Measure the output difference.

For parameter \(p\):

$$
D_p
=
\mathrm{RMS}(y_{p_{\max}}-y_{p_{\min}})
$$

If:

$$
D_p\approx0
$$

then either the control is intentionally inactive for that reaction or the implementation is broken.

RMS difference alone is insufficient for phase, stereo, pitch, spectral and event-timing controls. A control that only moves phase or stereo placement can read as inert under \(D_p\) while working correctly. Use the mechanism-appropriate measure as well.

### Spectral controls

$$
D_{spec} = \left\| PSD_{max}-PSD_{min} \right\|
$$

### Stereo controls

$$
D_{corr} = \left| \rho_{LR,max}-\rho_{LR,min} \right|
$$

plus inter-channel phase and difference-signal behaviour where applicable.

### Pitch controls

Measure spectral-peak displacement.

### Event controls

Measure event count, event rate or timing distribution.

### Temporal controls

Measure envelope, onset, decay and tail differences.

For every active control, record:

* parameter value;
* RMS;
* peak;
* spectral centroid;
* spectral flux;
* output duration;
* relevant reaction-specific metric.

Do not merely verify that the UI parameter moves.

Verify that the DSP changes.

---

# Test 13 — parameter monotonicity where appropriate

Not every control needs monotonic sonic behaviour.

But controls with a clear mathematical direction should be tested.

For example:

* higher DECAY should not consistently shorten the tail;
* higher AFTERGLOW should not consistently reduce persistence;
* higher ENRICHMENT should not consistently reduce input drive;
* higher EXPOSURE should not consistently reduce the reaction mechanism it controls.

If a parameter is intentionally non-monotonic, document that.

---

# Test 14 — reset behaviour

Process audio.

Then reset/reinitialise.

Verify that:

* internal states are cleared or restored according to specification;
* no previous event leaks into the next render unexpectedly;
* oscillator phase behaviour matches the documented rule;
* stochastic state restarts deterministically.

A fresh instance and a reset instance with the same seed must produce the same output.

---

# Test 15 — silence behaviour

Process silence through every reaction.

Expected behaviour is reaction-specific:

CHEMICAL:
may ring if feedback/event excitation is active.

RADIATION:
may generate its defined stochastic reaction events.

FISSION:
may generate branch activity if its event mechanism excites the branches.

SLUDGE:
may retain an afterglow but must decay.

ALIEN:
may generate events because it contains its own oscillator.

The important requirement is that the behaviour is intentional and bounded.

A reaction must not produce uncontrolled infinite output from silence.

---

# Test 16 — spectral-hole test

This is particularly important for SQUELCH.

Create an input with a deliberately empty spectral region.

For example:

* energy at 100 Hz;
* energy at 1 kHz;
* no energy around 400–800 Hz.

Run CHEMICAL and the reactions that contain resonant processing.

Verify that the reaction can generate activity inside the spectral hole where its architecture says it should.

This tests the central design principle that SQUELCH is an effect containing resonant/generative mechanisms rather than merely an EQ.

---

# Test 17 — golden renders

For each reaction create deterministic reference renders.

Store:

* input WAV;
* parameter state;
* seed;
* sample rate;
* block size;
* expected output hash;
* analysis JSON.

On later builds, rerender the same test.

Compare against the golden output.

Use exact comparison for deterministic DSP where practical.

If floating-point/compiler/platform differences make exact comparison inappropriate, use defined tolerances for:

* RMS;
* peak;
* spectral magnitude;
* timing;
* event count.

Never replace regression testing with subjective listening alone.

---

# Test 18 — generate an automated report

Every test run should produce:

`test-results/`

containing:

* `summary.json`
* `summary.md`
* rendered WAV files for failures
* numerical analysis
* pass/fail result
* parameter state
* sample rate
* block size
* seed
* git/build identifier

The report should identify:

* PASS
* FAIL
* WARN

for every test.

A failed mathematical invariant must not be hidden by an overall PASS.

---

# Test 19 — visual diagnostic files

For failed or suspicious tests, generate diagnostic plots where practical:

* waveform;
* spectrogram;
* magnitude spectrum;
* stereo correlation;
* parameter trajectory;
* resonant-frequency trajectory;
* stochastic-state trajectory;
* subharmonic spectrum;
* oscillator spectrum.

The goal is to let the developer determine whether the failure is:

* mathematical;
* numerical;
* implementation;
* parameter mapping;
* or simply a poor test threshold.

---

# Test 20 — no "looks OK" acceptance

Do not mark a reaction successful because:

* it produces audio;
* it sounds distorted;
* it has a resonant peak;
* it has random movement;
* the waveform looks complicated;
* the plugin compiles.

The reaction passes only when its defining mathematical mechanism can be demonstrated by an appropriate measurable test.

The final validation report must explicitly answer:

### CHEMICAL

Does it demonstrate nonlinear resonant feedback?

### RADIATION

Does it demonstrate correlated stochastic state modulation?

### FISSION

Does it demonstrate interacting branches, cancellation and cross-feedback?

### SLUDGE

Does it demonstrate generated subharmonics, asymmetric nonlinear content and long memory?

### ALIEN

Does it demonstrate an actual oscillator, FM/AM and reaction-specific timing?

---

# Definition of done

The contract has four axes and they are different claims. State them
separately; do not let one stand in for another.

* **All 32 DSP primitives agree with the prototype.** The implementation
  computes what the contract computes. This is a claim about the primitives the
  comparison harness runs, not about every stage: CHEMICAL's noise bed is a
  streaming implementation that is not in it, accepted on its delivered level.
* **All 39 behavioural checks pass.** Behavioural verification passes.
* **`SquelchValidate` reports zero FAIL and zero WARN.** A warning is a
  deferred failure, and a warn-list outlives the fault it was written for.
* **Specification coverage reports the expected clause set**, with every
  uncovered clause carrying an explicit documented reason, printed in the
  output rather than left to be inferred.
* **Fault injection demonstrates that each injected known fault is detected by
  its corresponding assertion.** This is materially stronger than the checks
  passing: it establishes that the assertions are not merely capable of
  passing, but were each shown to fail when their fault was restored.
* **Fault injection is observationally clean on exit**: restored source,
  freshly rebuilt artefacts, and a passing baseline, all three verified before
  it reports success. Restoring sources alone leaves the build directory
  holding the artefacts of the last injected fault, which is a clean
  `git status` over faulty binaries — a false state created by the validation
  harness itself, and therefore its responsibility to clear.
* **Coverage instrumentation fails closed.** If a named specification
  assertion stops executing, specification coverage must decrease rather than
  silently continue reporting the previous level.

Clause coverage is not to be reported as a nominal pass without its
qualification. The honest formulation is the one the README uses:
*implementation regression coverage and specification coverage are different
claims.* Fault injection establishes the first and says nothing about a
requirement nothing has ever broken — which has therefore never been tested
either. So the disposition to state is:

`SQUELCH validation: PASS, with N explicitly documented specification-coverage gaps.`

Underneath those axes, the implementation is considered mathematically
validated only when:

* the plugin builds;
* plugin validation passes;
* no NaNs/infinities occur;
* output remains bounded;
* deterministic rendering passes;
* block-size tests pass;
* sample-rate tests pass;
* parameter tests pass;
* golden renders pass;
* all five reactions produce their defining measurable mechanisms;
* no two reactions collapse into the same DSP architecture;
* failures produce diagnostic WAVs and analysis;
* the test suite can be rerun after every DSP change.

The developer must be able to run one command that performs the complete automated suite.

The final output of that command should clearly state:

`SQUELCH DSP VALIDATION: PASS`

or:

`SQUELCH DSP VALIDATION: FAIL`

with the failing reaction and test named explicitly.
