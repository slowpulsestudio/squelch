# SQUELCH — build plan

Working plan for building the five reactions as five separate effects.

The specification is [README.md](README.md), the transfer functions are in
[maths.md](maths.md), and the validation contract is
[dsp-testing.md](dsp-testing.md). This file is only the order of work.

---

## Where things stand

Measured, not estimated.

| | state |
|---|---|
| specification | all five reactions specified in maths.md |
| implementation | five distinct engines (SLUDGE, ALIEN, CHEMICAL, RADIATION, FISSION), dispatched per-reaction; no shared ladder |
| `prototype/checks.py` | 37/37 passing |
| reaction distinctness | 0/10 pairs too alike; closest FISSION/CHEMICAL at −6.1 dB |
| noise generation | fully causal: every stochastic value hashes (seed, stream, sample index), no stateful RNG left in reactor.py/reactions.py/rng.py |
| C++ port | 16/16 harness primitives agree to machine precision; all five engines ported and verified; output stage not ported; only SLUDGE wired into processBlock |
| git | ALIEN port committed, nothing pushed |

Steps 1-6 below are done. Step 7 (the C++ port) is in progress.

---

## Order of work

### 0. Housekeeping

- Decide what to do with the untracked `prompt.md`. It is a 189-line stale
  snapshot from before the rename to README.md and will drift against it.
- Commit the three documents, the retuned taps, and the distinctness check.

### 1. Per-reaction dispatch

`reactor.process` calls `filters.ladder` unconditionally. Nothing else can be
built until there is somewhere for a second engine to live.

- Introduce a per-reaction engine entry point; the ladder becomes one of five.
- Expose the raw reaction output, before the shared output stage, so Test 11
  can measure both domains. Without this every differentiation measurement is
  taken through a fixed EQ and a common level target.
- No sonic change. The render before and after this step should be identical.

### 2. SLUDGE

First new engine, because subharmonic generation is unmistakably not a filter.
If the dispatch is wrong, this is where it shows.

- Body extraction, long memory, asymmetric saturation.
- Independent `4π` and `8π` phase accumulators. A `2π` wrap here produces a
  buzz at the parent frequency with a DC offset and looks like it works.
- Verify against Test 9: energy near `f_h/2` and `f_h/4`, referenced to the
  reactor frequency, with no pitch tracking.

### 3. ALIEN

Second, because an actual oscillator is the furthest thing from the ladder and
proves the architecture can host a source rather than only a processor.

- Event-gated carrier, FM, AM, pitch jumps, reaction-specific timing.
- Test 10 both ways: silence plus a scheduled event must produce a burst;
  silence with no event must produce nothing.
- Anti-aliasing matters here. FM sidebands are unbounded.

### 4. CHEMICAL and RADIATION

Together, because they are the −26.0 dB pair and their distinction is the
subtlest of the five. Building one without the other invites convergence.

- CHEMICAL: keep the ladder, add the event-held register `R_q q_i` as a
  log-frequency offset. Never as a multiplier on sweep depth.
- RADIATION: quadrature resonator, continuous correlated state `q[n]`,
  correlation time `τ_q` independent of DECAY.
- The real check is Test 6 and Test 7's state diagnostics, not the audio.
  CHEMICAL's `q[n]` must be piecewise constant between events; RADIATION's must
  evolve sample to sample. Two architectures can be EQ-matched to sound similar
  while remaining different instruments, and the reverse is also true.

### 5. FISSION

Last, because coupled branches with `ρ(A) < 1` is the fiddliest stability
problem of the five.

- Two branches, detuning, cross-feedback, moving fractional delay.
- Test 8 is the architectural test: disabling coupling and detuning must change
  the output materially. If it does not, it is one filter wearing a costume.

### 6. Output stage review

Deferred until there is something to review.

- Run Test 11 in both domains. If the reactions separate raw and converge
  final, `voice()`'s fixed Pultec curve and `unity_match`'s common peak target
  are flattening them.
- Re-derive the drive and enrichment calibration against the new engines.

### 7. C++ port

- Extend `Source/Dsp` and the comparison harness one engine at a time.
- `prototype/checks.py` is the contract, not a guide.

---

## Definition of done for a reaction

A reaction is not finished because it produces audio, sounds distorted, or has
a resonant peak. It is finished when:

- its defining mathematical mechanism is demonstrated by a measurement that
  could have failed;
- its own test in dsp-testing.md passes;
- the distinctness check improves, and no pair it is part of remains below
  −9 dB;
- the forbidden shortcut in the maths.md audit table is demonstrably not what
  was built.

The last one is the point. Every reaction has a named lazy implementation that
would look plausible and measure as a retune, and the audit table exists
because one of them already shipped.

---

## Deferred

- MELTDOWN, IONIZE and CONTAMINATION behaviour per reaction.
- Editor work. The DSP decides what the controls need to be.
- Golden renders. Worth having once a reaction is stable, not before.
