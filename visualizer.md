# Squelch: Visualizer

Inherits only the **design language** from the design system — the one green on `surface/display`,
1 px wireframe, no fills or gradients, phosphor persistence, and the rule that every drawn
quantity traces to a real DSP value. See
[Visualizers](https://github.com/slowpulsestudio/sps-juce-design-system#visualizers).

**The layout is this plugin's own.** It is not the shared isometric box, and it must not be.

## 1. Idea

A cloud chamber. Squelch fires radiation events through a resonant filter, so the display is the
instrument physicists used to actually see particles: a chamber in which each particle leaves a
visible track, and the track's shape tells you what the particle was.

Every event is a **bullet fired into the chamber** from the left. Its track curves as it travels,
and the curve is the filter's cutoff sweep: a steep fall is a fast decay, a long shallow arc is a
slow one. The track fades as the vapour disperses, so the chamber always shows the last few
seconds of fire without a scrolling axis.

What makes each reaction different is what the track does:

| Reaction | Track |
| --- | --- |
| RADIATION | A straight track that forks as it goes, each fork fainter |
| FISSION | The track splits in two at a point and both halves carry on |
| SLUDGE | A heavy, slow track that spirals inward as it loses energy |
| CHEMICAL | A track that scatters off invisible obstacles, changing direction at each |
| ALIEN | A track that does not behave: it loops, reverses, and crosses itself |

That is the plugin's character drawn as five genuinely different pictures, rather than five
identical curves at different heights.

## 2. What is drawn

| DSP source | Drawn as |
| --- | --- |
| Each fired event | One bullet entering the chamber |
| The envelope's cutoff sweep for that event | The curve of its track: the sweep is the trajectory |
| The event's intensity and accent | How bright and how long the track is |
| Its pan | Where in the chamber it is fired, left to right |
| SPREAD | How far the track travels before it settles |
| DECAY | How long the track takes to fade |
| The feedback against the self-oscillation point | Tracks above it ionise: they thicken and leave a halo, because the filter is ringing on its own |
| REACTION | The track behaviour above |
| The grid the scheduler runs on | Tick marks along the chamber's entry edge |
| PROBABILITY | A step that did not fire leaves its tick dark and no bullet |
| MELTDOWN | The whole chamber saturates: every track thickens at once |
| IONIZE | A standing haze in the chamber while it is latched |
| The held cutoff | The chamber's resting floor line, where tracks come to rest |
| Readouts | The reaction, the grid division, the held cutoff, and the live voice count |

## 3. Why the current one does not work

The present display plots every event as the same exponential curve on a shared grid, so all five
reactions look identical and the picture is a smear of overlapping lines. It also depends on
events being visible against an empty grid, which on a dense grid setting they are not. The
chamber gives each reaction its own shape and gives the display a resting state.

## 4. Implementation

**Not built.** `Source/SquelchDisplay.*` draws the generic version and should be replaced.

The data path is right and can stay: `Source/DisplayState.h` publishes, through a seqlock, the
events that fired with the curve constants the envelope built for them, the grid steps with
whether each passed the probability gate, the 16 placement voices, the held and current cutoff,
the reachable feedback, and the meltdown and ionize flags.

What is missing is the per-reaction track behaviour, which the display should derive from the
published reaction and the event's own constants — not invent.

**Paint cost.** Not measured.
