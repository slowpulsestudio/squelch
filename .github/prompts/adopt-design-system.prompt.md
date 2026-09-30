---
mode: agent
description: Connect this JUCE plugin to the Slow Pulse Studio design system for the first time, and rebuild its editor from design system components.
---

This is a one-time migration with visible consequences — it replaces the plugin's entire user interface. Work through it in order and stop at every checkpoint. Never skip a confirmation.

## Step 0 — Safety

Check `git status`. If there are uncommitted changes, tell the user to commit or stash them first, and stop. This prompt rewrites the editor; the user needs a clean point to return to.

Then confirm the plugin builds as it is today by running `/build-vst3`. If it does not build before you start, stop and say so — do not migrate a broken project.

## Step 1 — Check whether it is already connected

Find the CMakeLists.txt containing `juce_add_plugin(...)`. Search it for `FetchContent_Declare` with `SpsDesignSystem`.

If it is already there, say the plugin is already connected, point the user at `/system-my-design` for version updates, and stop.

## Step 2 — Gather the facts, change nothing

Read and report, without editing anything:

- The CMake target name from `juce_add_plugin(<TargetName> ...)`
- The editor's current size from `setSize(...)` in the PluginEditor constructor
- Every parameter from `createParameterLayout()`: ID, display name, type (Float / Bool / Choice / Int), min, max, default, units, skew, and the exact choice list for Choice parameters
- Any named preset list, in order
- Which parameters are input or output gain, wet/dry mix, or bypass

## Step 3 — Propose the mapping, then WAIT

Present a table mapping every parameter to the component it will become:

- Continuous parameters -> `sps::RotaryKnob`
- Choice parameters -> `sps::SwitchSelector`
- Boolean parameters -> `sps::Toggle`
- Seed or integer entry -> `sps::Adjustor`
- Value readouts -> `sps::Quartz`

Then describe the proposed layout in plain English: a `sps::PresetToolbar` across the top, the creative parameters in a grid on a `sps::ModulePanel`, and the input/output/mix controls in a separate section below a divider.

State the proposed new editor size, and say plainly if it differs from the current one.

Every parameter must appear in that table. If you cannot place one, say so rather than dropping it.

STOP HERE. Ask the user to approve the mapping and the layout before you write any code.

## Step 4 — Wire up the build

Find the newest released version:

```
git ls-remote --tags --refs https://github.com/slowpulsestudio/sps-juce-design-system.git
```

Tags look like v0.1.0. Sort by version number and take the highest. Do not guess a tag — if the command fails, stop and say why.

Add to the plugin's CMakeLists.txt, next to the existing JUCE FetchContent block:

```cmake
FetchContent_Declare(
    SpsDesignSystem
    GIT_REPOSITORY https://github.com/slowpulsestudio/sps-juce-design-system.git
    GIT_TAG <the tag you found>
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SpsDesignSystem)
```

And add `sps::DesignSystem` to that target's `target_link_libraries`.

Do not copy any design system source files into this project. Components are reached as `#include "Components/RotaryKnob.h"`.

Confirm it still builds with `/build-vst3` before touching the editor. A build failure here is a wiring problem, not a UI problem, and is much easier to fix on its own.

## Step 5 — Rebuild the editor

Rewrite the PluginEditor using the approved mapping. Rules:

- Do not touch the processor, the DSP, or `createParameterLayout()`. Parameter IDs, ranges and defaults stay exactly as they are — this is a UI change only.
- Every control is attached to its parameter through the APVTS attachment classes, so automation and host state keep working.
- `sps::Toggle` is a `juce::Button`, so boolean parameters bind with `AudioProcessorValueTreeState::ButtonAttachment` like any other button — never hand-roll a `juce::ParameterAttachment` for one.
- Every control gets a hover tooltip using the classic industry term for that parameter, per the tooltip rule in master-skills.md.
- Displayed values are rounded to whole integers unless that parameter is explicitly meant to show decimals.
- Wire `sps::PresetToolbar`: `setPresetNames`, `onPresetSelected`, `onRandomise`, and `isDirty`. Call `refreshDisplay()` whenever a tracked parameter changes.
- Randomise jitters only the creative parameters. It must never move input gain, output gain, wet/dry mix or bypass. Seed parameters ARE included.
- Delete the old `GenericAudioProcessorEditor`, any bespoke LookAndFeel, and any stock `juce::Slider` / `juce::TextButton` the design system now replaces.
- Do not write hex colours, corner radii or font sizes directly. Use the design system's components, which already carry them.

## Step 6 — Build and hand over

Run `/build-vst3`.

If it fails, show the errors and stop — do not attempt speculative fixes to unfamiliar DSP code.

On success, report what changed: how many parameters moved to which components, the old and new editor size, and anything you could not map. Then say exactly: "Time to test the VST3 in your DAW!"

## Notes

Never commit or push anything as part of this prompt.
