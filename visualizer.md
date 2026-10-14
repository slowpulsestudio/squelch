# Squelch: Visualizer

Inherits the SPS visualizer language (style, tokens, floor mesh, persistence, isometric
stage, time axis and the traceability principle) from the design system's
[Visualizers](https://github.com/slowpulsestudio/sps-juce-design-system#visualizers)
section. This file records only what is specific to Squelch. The model's quantities are
defined in [dsp-maths.md](dsp-maths.md).

## 1. Idea

Squelch is a scheduler firing events at a ladder filter, and every event is a cutoff sweep.
SPREAD sets how many octaves it covers, DECAY how long it takes, REACTIVITY how hard it
leaps, EXPOSURE how close to self-oscillation it gets. All of that is one curve per event,
and the plugin currently shows none of them.

So the display is the sweeps. **Cutoff runs up on a log axis, time runs along the beam,
and depth is the stereo placement each voice was given.** Each event is a curve starting at
its peak and falling to the held cutoff, as bright as its intensity. The grid the scheduler
is running on is ticked along the floor, so a step that did not fire — PROBABILITY below
100% — is a visible gap rather than something you have to count.

Self-oscillation is a labelled frame on the feedback: above it the filter rings on its own.
That line is the difference between SQUELCH as a filter and SQUELCH as a synthesiser, and
it is currently something you find by ear.

## 2. Axes

| Axis | Meaning | Numbered as |
| --- | --- | --- |
| Time | The last few seconds, newest at the front | Seconds back, 0 to -4s |
| Cutoff | The ladder's cutoff, log | Hz, at 20, 100, 1k and 10k |
| Depth | Stereo placement, which FALLOUT spreads | L and R |

## 3. DSP state to visual state

| DSP source | Drawn as |
| --- | --- |
| `Envelopes` per-sample `cutoffHz` | The strong trace: the sweep, one curve per event |
| `Envelopes::held` | A level frame at the cutoff the sweeps fall back to |
| `Envelopes::glide` | The curvature of the fall, which is the trace itself |
| `ScheduledEvent::start` | Where on the time axis each curve begins |
| `ScheduledEvent::intensity` and `accent` | Brightness of the curve; an accented step is brighter |
| `ScheduledEvent::tone` | How high the curve starts, which is the pitch variation |
| `ScheduledEvent::slide` | Curves joined rather than separate, so a slid step is one continuous line |
| `ScheduledEvent::pan` | Its depth |
| `Placement::voices[i].env` | The brightness of each of the up-to-16 live voices as it decays |
| `Placement::voices[i].panL` / `panR` | Where each voice sits across the depth axis |
| `Placement::voices[i].send` | Hatching behind the curve: what is going to the reverb |
| `Envelopes` per-sample `feedback`, against the self-oscillation point | A labelled frame; the part of a curve drawn above it is where the filter is ringing on its own |
| GRID with the host tempo, in GRID mode | Ticks on the floor at the step positions |
| MODE | Which ticks appear: the grid in GRID, none in FREE, the detected onsets in INPUT, scattered in RANDOM |
| PROBABILITY | A step tick that fired is filled; one that did not is hollow |
| SPREAD | The height each curve starts at above the held cutoff, in octaves |
| REACTION | The curve's shape per engine, and the readout. Each reaction's own state — SLUDGE's subharmonic phases, CHEMICAL's event register — adds its own fainter marks under the curve |
| `Envelopes` per-sample `drive` with DRIVE and TOXICITY | Thickness of the curve where it is being saturated |
| `meltdown` staged values | While MELTDOWN is held, the frame edge is lit and the affected frames move to where the meltdown put them |
| IONIZE | The frame edge is lit while it is latched |
| `voiceL` / `voiceR` enclosure filters | The box's two side walls dim with each channel's enclosure |
| Readouts | The reaction, the grid division, the held cutoff in Hz, and the live voice count |

## 4. Implementation

**Built.** `Source/SquelchDisplay.*` subclasses `sps::VisualizerPanel` and sits above the
three columns. The audio thread publishes a `DisplayState` once per block through a
seqlock (`Source/DisplayState.h`): the events that fired with the curve constants the
envelope built for them, the grid steps with whether each passed the probability gate,
the 16 placement voices, and the held and current cutoff. Each sweep is plotted from the
closed form `rest + (peak - rest) e^(-u/tau)` the envelope itself plays, not sampled at
frame rate. The display's clock is a monotonic sample count the audio thread keeps, so a
host locate does not drag it.

Not drawn yet: the per-reaction marks (SLUDGE subharmonics, CHEMICAL register), the
enclosure walls, the saturation thickness and the MELTDOWN frame move. MELTDOWN and the
voice count are readouts only.

**Paint cost.** Not measured.

## 5. Reference images

None yet.
