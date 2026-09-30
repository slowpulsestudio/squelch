---
mode: agent
description: Configure, build, and install this project's JUCE VST3 plugin.
---

## Step 1 — Locate the plugin's CMakeLists.txt

Find the `CMakeLists.txt` that contains a `juce_add_plugin(...)` call — search for it, it may not always be at `plugin/CMakeLists.txt`. Read it to determine:

- The CMake target name passed to `juce_add_plugin(<TargetName> ...)`
- The `PRODUCT_NAME` set on that target

Do not hardcode any target name (e.g. "Parasite") — always use the one actually found in the file.

## Step 2 — Verify build tools are installed

Check that the required tools are present:
- `xcode-select -p` — Xcode Command Line Tools
- `which cmake` — CMake

If either is missing, tell the user the exact install command needed (e.g. `xcode-select --install`, or `brew install cmake`) and stop. Do not guess or attempt a workaround.

## Step 3 — Configure

From the directory containing the `CMakeLists.txt` found in Step 1, run:

```
cmake -B build
```

Reuse an existing `build/` directory if one is already present. Only pass fresh/clean configure flags (e.g. removing `build/` first) if the user explicitly asks for a clean rebuild.

## Step 4 — Build

Run:

```
cmake --build build --target <TargetName>_VST3
```

where `<TargetName>` is the actual CMake target name found in Step 1.

If the build produces compile errors, surface them clearly to the user and stop — do not attempt speculative fixes to unfamiliar code.

## Step 5 — Confirm install

On a successful build, confirm the `.vst3` now exists under:

```
~/Library/Audio/Plug-Ins/VST3/Slow Pulse Studio/
```

Say exactly: "Time to test the VST3 in your DAW! (If it was already open, fully restart it — a rebuilt plugin can stay stale in a running host otherwise.)" — not a multi-step checklist or a longer prose explanation.

## Notes

Never commit or push anything as part of this prompt — it only configures, builds, and installs the plugin.
