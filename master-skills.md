# JUCE VST3 Plugin Rules

---

**Default target configuration**
Unless the Designer explicitly asks for more, target VST3 only, macOS only, no Standalone build. `juce_add_plugin(... FORMATS VST3 ...)` — do not add `AU`, `Standalone`, or a Windows/Linux target speculatively.

**A failed response looks like:**
- Adding `Standalone`, `AU`, or other formats "for completeness" when only VST3/macOS was asked for
- Building cross-platform CMake logic for a project that only targets macOS

---

**JUCE acquisition and build**
Bring JUCE in via CMake `FetchContent`, pinned to a specific release tag — never a moving branch. Use `juce_add_plugin(... COPY_PLUGIN_AFTER_BUILD TRUE)` so a plain `cmake --build` deploys the compiled `.vst3` straight to `~/Library/Audio/Plug-Ins/VST3/`, with no manual copy step.

**A failed response looks like:**
- Pinning `FetchContent` to `main`/`develop` instead of a release tag
- Leaving out `COPY_PLUGIN_AFTER_BUILD`, forcing a manual copy step after every build
- Suggesting Projucer or a manually-downloaded JUCE install when the Designer has no existing JUCE setup

---

**Build with hidden symbol visibility**
By default, CMake/Clang exports every symbol from a plugin bundle with default visibility. For a JUCE VST3 this means hundreds of internal JUCE symbols end up globally exported per plugin (confirmed with `nm -g <plugin>.vst3/Contents/MacOS/*` — typically 300-700+ T/D symbols), when only a handful of VST3 entry points (`GetPluginFactory`, `bundleEntry`, `bundleExit`) need to be visible at all.

When a host loads more than one plugin built from the same JUCE version in one process, macOS's dynamic linker can coalesce matching default-visibility symbols across the separately-loaded bundles — including function-local statics such as JUCE's Typeface font cache and its mutex. Two independently-built plugins can end up sharing one instance of that static at runtime. Unloading one plugin then destroys the shared instance out from under the other, aborting the host (confirmed: Ableton Live, "mutex lock failed: Invalid argument", on quit or on unloading any one of the affected plugins).

Fix: every plugin's CMakeLists.txt must set, before `FetchContent_MakeAvailable(JUCE)`:

```cmake
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
```

Verify with `nm -g <plugin>.vst3/Contents/MacOS/<name> | grep -E " T | D | S " | wc -l` — should drop to roughly 3 (just the required entry points), not hundreds.

**A failed response looks like:**
- Treating a crash on host shutdown/plugin-unload as unfixable "JUCE noise" without checking exported symbol counts first
- Fixing symbol visibility in only one of two plugins loaded together — the other plugin still exporting default-visibility symbols is enough to still collide
- Assuming this only matters for plugins sharing the exact same design-system version; it collides on matching JUCE version/ABI, independent of which design system is used

---

**Design system acquisition**
Every plugin's UI is built from the shared Slow Pulse Studio design system, never from stock JUCE widgets or a bespoke LookAndFeel. Bring it in the same way JUCE itself is brought in — CMake `FetchContent`, pinned to a specific release tag, never a moving branch:

```cmake
FetchContent_Declare(
    SpsDesignSystem
    GIT_REPOSITORY https://github.com/slowpulsestudio/sps-juce-design-system.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SpsDesignSystem)
target_link_libraries(<TargetName> PRIVATE sps::DesignSystem)
```

Do not copy design system source files into the plugin's own `Source/` folder. The library carries its own fonts, icons and colours; a plugin that copies them will drift the moment the design system is updated. Every colour, radius, spacing value and font size comes from the design system's tokens rather than being written as a literal in the plugin.

Continuous parameters use `sps::RotaryKnob`. Choice parameters use `sps::SwitchSelector`. Boolean parameters use `sps::Toggle`. Numeric entry uses `sps::Adjustor`. Readouts use `sps::Quartz`. The preset toolbar is `sps::PresetToolbar`. The chassis is `sps::ModulePanel`.

A plugin that is not yet connected to the design system is migrated with `/adopt-design-system`, which is a one-time job. After that, run `/system-my-design` periodically to check whether a newer design system version is available.

**A failed response looks like:**
- Building a plugin editor from stock `juce::Slider` / `juce::TextButton` / `GenericAudioProcessorEditor` instead of the design system's components
- Copying design system source files into the plugin's own `Source/` folder instead of linking the library
- Pinning the design system's `FetchContent` to `main` instead of a release tag
- Writing a hex colour, corner radius or font size directly into a plugin when a design system token already defines it
- Writing a bespoke `LookAndFeel` to restyle stock JUCE widgets

---

**Verify exact JUCE API against the real FetchContent source, not memory**
Since JUCE is pinned via CMake `FetchContent`, the actual JUCE source is available on disk at `<build-dir>/_deps/juce-src`. When unsure of an exact method name, signature, or class (e.g. a LookAndFeel override or a Font method), grep that real source directly instead of guessing from memory — it is authoritative for the exact pinned version in use.

**A failed response looks like:**
- Guessing a JUCE method name/signature from memory and writing code against it without checking the actual fetched source when uncertain
- Assuming a newer/older JUCE API surface than the version actually pinned by FetchContent

---

**JUCE 9's juce_audio_processors_headless VST3 module can fail to compile on newer Clang/libc++ with 'shared_ptr' is not a member of 'std'**
The Steinberg VST3 SDK headers vendored inside JUCE 9's juce_audio_processors_headless module use std::shared_ptr without an explicit #include <memory>. Recent Apple Clang/libc++ releases (Xcode 16+) have stopped transitively pulling in <memory> through other standard headers, so this compiles fine on older toolchains but fails on newer ones with no fix in JUCE's newest release tag (9.0.2 as of writing — verify no newer tag has fixed it before applying this). Since the failing file lives in FetchContent-downloaded, non-persistent vendor source, don't patch it directly — force the include via a compiler flag on the plugin's own target instead: target_compile_options(<TargetName> PRIVATE -include memory)

**A failed response looks like:**
- Editing the vendored JUCE header directly in build/_deps/juce-src — it gets wiped and re-fetched on the next clean configure
- Concluding the Designer's own Source/ code or repo is broken/incomplete when the compile error is inside JUCE's own headers before any of their code is even reached
- Not checking whether a newer JUCE release tag has already fixed this before applying the -include memory workaround

---

**The -include memory workaround must be scoped to C++ translation units**
The rule above is correct but incomplete: applied unscoped, `-include memory` is also passed to JUCE's C and Objective-C sources, which fail immediately with `fatal error: 'memory' file not found` — a confusing error that looks like a broken toolchain rather than a scoping mistake. Scope it to C++ only:

```cmake
target_compile_options(<Target> PRIVATE
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-include;memory>)
```

**A failed response looks like:**
- Applying `-include memory` to the whole target instead of scoping it to `CXX,OBJCXX`
- Concluding the compiler or JUCE checkout is broken when a `.c`/`.m` file can't find a C++ header

---

**Tooltips must be applied through a composite control's children**
JUCE's tooltip window asks only the single component directly under the mouse for its tip (a `dynamic_cast<TooltipClient*>` on that one component) — it never walks up to a parent looking for one. Several `sps` design system controls are built from child pieces rather than being one component (e.g. `sps::Adjustor`'s numeric readout and its step buttons are separate children); calling `setTooltip` on the outer control only covers whatever background area isn't occupied by a child, which for some controls is almost none of the visible surface. `sps::Adjustor` specifically is not a `juce::SettableTooltipClient` at all, so it has no `setTooltip` to call in the first place — a subclass mixing in `juce::SettableTooltipClient` is needed before a tip can be set on it.

The fix is a small recursive helper: set the tooltip on the control, then walk `getChildComponent(i)` for every child and set the same tooltip wherever the child is itself a `SettableTooltipClient` (`dynamic_cast`, skip silently if not). Apply this to every control on the panel, not just `Adjustor` — knobs, faders and switches are also built from sub-parts in places.

**A failed response looks like:**
- Calling `setTooltip` once on the outer control and assuming that covers it
- Testing tooltips by hovering one spot (e.g. the control's edge) and concluding the whole control shows a tip, when the readout or buttons in the middle show nothing
- Treating a missing tooltip on a design-system component as a bug in the design system rather than in how the tooltip was applied

---

**A DSP port is verified against the prototype, not against the compiler**
When porting a tuned prototype to real-time code, build a small offline harness that renders through the shipping code path and compares it numerically against the prototype — pitch, envelope times, per-band energy, peak. "It builds" and "it sounds about right" are not evidence. Keep the harness in the repo as a separate target so the comparison can be re-run after any change to the DSP.

**A failed response looks like:**
- Reporting a port complete because it compiles and loads
- Judging a port by ear alone
- Deleting the comparison harness once it passes

---

**Pre-render per-setting work off the audio thread**
Where a prototype renders one fixed event per setting and replays it, the real-time form is a pre-rendered buffer, not per-sample synthesis. Render on a background thread into a spare buffer and publish it with a single atomic store; the audio thread only ever reads. Anything expensive and setting-dependent (large additive partial counts, filter design, normalisation passes) belongs there.

**A failed response looks like:**
- Summing dozens of partials per sample in `processBlock`
- Rebuilding a wavetable or buffer inside a parameter callback on the audio thread

---

**Host-position scheduling, not a free-running counter**
Rhythmic triggering derives from the host playhead's PPQ position, and any probabilistic choice hashes the step index with a seed rather than drawing from a running RNG. A bounced render must match what was heard, and replaying a bar must fire the same pattern.

**A failed response looks like:**
- A sample counter that drifts against the host
- A `std::random` generator whose sequence depends on how many blocks have been processed

---

**Install location: vendor subfolder, not the bare VST3 root**
Plugins install to a `Slow Pulse Studio` subfolder inside the system VST3 folder, not directly into `~/Library/Audio/Plug-Ins/VST3/`. Set `VST3_COPY_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3/Slow Pulse Studio"` on `juce_add_plugin(...)` alongside `COPY_PLUGIN_AFTER_BUILD TRUE`. This keeps every plugin from this studio grouped together in the DAW's plugin browser instead of mixed in with every other vendor's plugins.

**A failed response looks like:**
- Letting COPY_PLUGIN_AFTER_BUILD install straight to the bare VST3/ root without a vendor subfolder
- Using a different or inconsistent subfolder name across projects instead of Slow Pulse Studio

---

**COMPANY_NAME controls DAW vendor grouping, separate from install path**
`COMPANY_NAME` in `juce_add_plugin()` is the metadata a DAW's own plugin browser uses to group plugins by vendor — this is completely independent of the filesystem install subfolder (`VST3_COPY_DIR`). Setting one without the other will not fix vendor-grouping issues in the DAW.

**A failed response looks like:**
- Assuming the install-path vendor subfolder also controls how the DAW browser groups the plugin — it doesn't, `COMPANY_NAME` does
- Changing `VST3_COPY_DIR` to fix a DAW-browser vendor-grouping complaint instead of `COMPANY_NAME`

---

**A full DAW restart may be required to load a rebuilt plugin**
Overwriting an installed `.vst3` in place and removing/re-adding the plugin instance in the DAW is not always enough to load the new binary — some hosts keep the old plugin module resident in process memory across instance add/remove. If a rebuilt plugin doesn't reflect recent changes after being re-added, fully quit and relaunch the DAW before assuming the build or install step failed.

**A failed response looks like:**
- Concluding the build/install pipeline is broken because a DAW still shows old behavior after only removing/re-adding the plugin instance
- Not mentioning a full DAW restart as a troubleshooting step when a rebuilt plugin appears stale in a host

---

**Real-time audio safety in `processBlock`**
`processBlock` runs on the audio thread and must never allocate, lock, log, or do file/network I/O — any of these can cause audible dropouts/glitches in the DAW. Parameter changes must be smoothed (`juce::SmoothedValue`), never applied as a hard jump, to avoid zipper noise/clicks.

**A failed response looks like:**
- Allocating a `std::vector`, `juce::String`, or any heap object inside `processBlock`
- Calling `DBG`/`std::cout`/file I/O from `processBlock`
- Reading a raw parameter value directly into a filter coefficient every block instead of smoothing it

---

**Parameters**
Expose all user-facing parameters through a single `AudioProcessorValueTreeState`, defined once in `createParameterLayout()`. Internal/derived values (e.g. automatic makeup gain) are not user parameters and must not be added to the APVTS.

**A failed response looks like:**
- Reading raw member variables from the editor instead of going through the APVTS
- Exposing an internal implementation detail as a user-facing parameter without being asked

---

**Rotary knobs are the default control, not linear sliders**
For continuous plugin parameters, use a rotary knob (`juce::Slider::RotaryHorizontalVerticalDrag` or equivalent rotary style) as the default control. Only use a linear `juce::Slider` (horizontal or vertical bar) when the Designer explicitly asks for one, or the parameter is inherently linear/positional in a way a knob can't represent (e.g. a playhead/scrub position). Don't default to a linear slider just because it's the JUCE `Slider` default style.

**A failed response looks like:**
- Adding a horizontal/vertical bar `Slider` for a generic gain/frequency/mix-style parameter without being asked
- Leaving a `Slider` on its default linear style instead of setting a rotary style
- Using a linear slider "for now" with intent to swap to a knob later

---

**Round displayed parameter values to whole integers**
Any numeric value shown in the UI (parameter readouts, labels, tooltips) is rounded up to a whole integer by default — no decimal places — unless the Designer explicitly asks for decimal precision on a specific parameter. Round the display text only; keep the underlying parameter value at full float precision internally.

**A failed response looks like:**
- Displaying a parameter value like `-3.42 dB` or `440.0 Hz` in the UI by default
- Truncating/flooring instead of rounding
- Rounding the underlying stored/automated parameter value itself instead of only the displayed text

---

**Every control has a hover tooltip using classic industry terminology**
Every parameter control in the editor (knob, slider, button, toggle) gets a hover tooltip (`juce::Component::setTooltip` or equivalent) explaining what it does, written using the classic, industry-standard term a working audio engineer would recognize (e.g. "Attack", "Release", "Q", "Drive", "Wet/Dry") rather than an invented or marketing-style name. This applies to every parameter, not just the ones that seem non-obvious.

When a control's on-screen label is a whimsical/thematic metaphor rather than a classic industry term (e.g. "Wiggle", "New Worm"), the tooltip opens with a plain-language definition of what that metaphor means before giving the technical/industry-standard explanation — never the technical term alone with no explanation of the metaphor, and never the metaphor's definition omitted in favor of jumping straight to jargon.

**A failed response looks like:**
- Shipping a knob/slider/button with no tooltip at all
- Only adding tooltips to a subset of "confusing" parameters instead of every control
- Using a made-up or branded label in the tooltip instead of the classic industry term (e.g. "Squish" instead of "Ratio")
- Opening a metaphor-labeled control's tooltip with the technical explanation instead of defining the metaphor first

---

**Every VST3 plugin has presets and a Randomise button in a top toolbar**
Every VST3 plugin ships with a save-able preset system (a `ComboBox` populated from named presets) and a "Randomise" button that jitters the creative/tunable parameters, both placed together in a toolbar strip across the top of the editor — not buried in a submenu or absent entirely. The Randomise button sits immediately to the right of the preset `ComboBox`, not elsewhere in the toolbar. This is a baseline UX expectation for every plugin from this studio, not an opt-in feature to be asked about per-project. Use the shared `sps::PresetToolbar` component from the design system library instead of reimplementing the toolbar from scratch each time. Run `/system-my-design` periodically to check whether a newer design system version is available.

Pressing Randomise deselects any preset entirely — the toolbar shows the literal text "Random" (no italics, no trailing `*`) until the Designer explicitly picks a preset again from the dropdown or cycle buttons, at which point normal preset-name + dirty-state display resumes. `sps::PresetToolbar` has a dedicated `showUnsavedLabel()` method for this — never fake it by clearing the `ComboBox` selection directly.

Randomise only jitters preset-tunable creative parameters — it must never touch any control in an Input or Output section of the GUI (input gain/trim, output gain, dry/wet mix, bypass), per the "Preset-defining values vs. global mode toggles" rule below.

**A failed response looks like:**
- Shipping a VST3 editor with only the generic parameter list and no preset `ComboBox` or Randomise button
- Adding presets/randomise but placing them somewhere other than a top toolbar (e.g. buried at the bottom, in a separate tab/page)
- Placing the Randomise button somewhere other than immediately to the right of the preset `ComboBox`
- Treating presets or the Randomise button as a nice-to-have the Designer has to explicitly request for each new plugin
- Reimplementing the toolbar/dirty-state logic from scratch instead of using `sps::PresetToolbar`
- Leaving the previously-selected preset's name (dirty or not) displayed after Randomise instead of showing "Random"
- Jittering an Input/Output-section control (input gain, output gain, dry/wet mix, bypass) when Randomise is pressed

---

**Preset dirty-state indicator**
When the current parameter values no longer match the saved preset they were loaded from (the Designer tweaked something, or Randomise was pressed), show the preset name in the `ComboBox` in italics with a trailing `*` (e.g. `Warm Pad*`). Revert to the plain, non-italic name with no `*` the moment the values match a saved preset again (including right after saving). `sps::PresetToolbar` (see above) already implements this via its `isDirty` callback — wire it up rather than reimplementing the italics/`*` logic.

When a plugin has named presets and is opened fresh (no saved DAW state to restore), initialize the editor to the first preset in the list via the same code path used when the Designer picks a preset from the toolbar (`sps::PresetToolbar::setSelectedPreset (0)` plus loading that preset's values into the plugin's parameters) — never leave the raw `AudioParameterFloat`/etc. defaults from the parameter layout in place. The toolbar must show that first preset selected and non-dirty (no `*`) immediately on open.

That first preset must be a dedicated "Default" preset with neutral/baseline parameter values — not just whichever preset happens to be first alphabetically or by creation order — so the Designer always has a known, unmodified state to return to via the dropdown or cycle buttons.

**A failed response looks like:**
- Leaving the preset name unchanged (no italics, no `*`) after a parameter has been edited or Randomise pressed
- Leaving the italics/`*` in place after the Designer saves the current values as/over that preset
- Using a different dirty-state indicator than italics + trailing `*` (e.g. a separate icon, a color change, a modal dialog)
- Leaving the editor on raw parameter defaults on first open instead of the first named preset
- Treating an arbitrary/creative preset as the first-in-list default instead of a dedicated neutral "Default" preset

---

**Retrofit the preset/Randomise toolbar on any existing plugin missing it**
The preset+Randomise toolbar and dirty-state indicator rules above are a baseline requirement for every plugin from this studio, not just new ones. If a Designer opens an existing plugin project that predates these rules (or only partially implements them) and asks for unrelated work on it, add the missing toolbar/indicator — using `sps::PresetToolbar` — as part of that same task instead of only mentioning it's missing.

**A failed response looks like:**
- Noticing the toolbar or dirty-state indicator is missing/incomplete on an existing plugin but only mentioning it instead of adding it
- Waiting for the Designer to explicitly ask for the toolbar to be retrofitted before adding it
- Treating these rules as applying only to brand-new plugins, not existing ones opened for other work

---

**Preset-defining values vs. global mode toggles**
When a plugin has both save-able presets and boolean mode toggles that represent a general workflow preference (e.g. a hard/soft character switch, or a static-vs-dynamic processing mode), keep those toggles out of the preset-value struct/table entirely. Presets should only capture the continuous/creative parameters they're meant to tune — switching presets should never silently flip a mode switch the user deliberately set.

A seed/determinism parameter (one that seeds the plugin's own internal RNG or generative trajectory) is not a global mode toggle under this rule, even if it also has its own dedicated regenerate control (e.g. a "New Worm"/"New Pattern" button) — it's part of the creative variation Randomise exists to produce, so it stays in the preset-tunable set and Randomise must still jitter it. Only exclude parameters that are genuinely non-preset workflow settings.

Any control that lives in an Input or Output section of the GUI (e.g. input gain/trim, output gain, dry/wet mix, bypass) is always excluded from presets and from Randomise, the same as a global mode toggle — these are gain-staging/session-level settings the Designer sets for their current mix, not creative content a preset should recall or Randomise should jitter.

**A failed response looks like:**
- Bundling a general-purpose mode toggle into the same struct/table as preset-tunable values, causing preset switches to silently change it
- Forgetting to document which parameters are intentionally excluded from presets, leaving future changes to accidentally include them
- Excluding a seed/determinism parameter from Randomise because it has its own dedicated regenerate button
- Including an Input/Output-section control (input gain, output gain, dry/wet mix, bypass) in a preset's saved values or in Randomise's jitter set

---

**Input and output belong in the layout, not among the parameters**
Every plugin wears a strip down each side: input trim above its fader on the left, mix above the output fader on the right. These are not creative parameters — they never appear in the parameter grid, and Randomise never touches them (see the rule above). Every plugin has both strips; this is chassis, not a per-plugin decision. Four of five plugins were missing one before anyone checked, because nothing made their absence visible.

**A failed response looks like:**
- Placing input trim, mix, or output gain in the main parameter grid alongside creative controls
- Shipping a plugin with only one of the two strips, or neither
- Treating the strips as an optional layout choice to raise with the Designer per plugin

---

**Section membership is authored, not coded**
Each plugin declares, in one place in its own source, which of its controls belong to INPUT, PERFORMANCE and OUTPUT, and which Randomise may touch. Randomise derives its scope from that same declaration rather than a second list, so moving a control into a strip excludes it with nothing else to update. The design system supplies the strips, the components and the naming/tooltip convention — it does not define which parameters a plugin has.

Slots fill from the bottom, so a lone parameter lands on the fader rather than the readout above it. An unclaimed slot draws nothing: an empty strip is a visible reminder that a plugin is unfinished, rather than a default quietly standing in for a decision nobody made.

**A failed response looks like:**
- Keeping two separate lists — one for layout, one for Randomise scope — that can disagree with each other
- Putting a plugin's parameter names into the shared design system
- Filling an empty slot with a placeholder or sensible default rather than leaving it blank
- Filling slots from the top, so a single parameter lands on the readout instead of the fader

---

**Validation**
After building, validate with `pluginval` (JUCE's own automated plugin validator) before considering the plugin "done" — this is the automated check, not a substitute for it. Then load it in a real DAW (rescan the plugin folder) and process real audio as the end-to-end smoke test; a clean `pluginval` pass alone is not sufficient.

**A failed response looks like:**
- Declaring the plugin done because it compiled, without running `pluginval`
- Running `pluginval` but never actually loading the plugin in a DAW with real audio

---

**Telling the Designer to go test in the DAW is one line, not a checklist**
When the next step is simply "go test the plugin in your DAW" (no other setup needed), say exactly: "Time to test the VST3 in your DAW!" — not a multi-step checklist with headers. Only break it into steps if the Designer asks how.

**A failed response looks like:**
- Turning "go test the plugin in your DAW" into a multi-step checklist instead of just saying "Time to test the VST3 in your DAW!"

---

**DAW smoke-test project scaffold, gitignored from the start**
When scaffolding a new JUCE plugin project, create a `Testing/` folder containing a DAW test project (e.g. an Ableton Live `.als` project) that loads the plugin for manual smoke testing, and add `Testing/` to `.gitignore` in the same commit that creates it. DAW projects auto-generate large, constantly-churning subfolders on every save (Ableton: `Backup/` with timestamped project snapshots, `Samples/` with recorded/bounced audio) — these are local working state, not project source, and produce noisy binary diffs if tracked. Do not commit the `Testing/` folder first and gitignore it later; set this up correctly at project creation time.

**A failed response looks like:**
- Committing the DAW test project folder before gitignoring it, requiring a later `git rm --cached` cleanup
- Tracking `Backup/`/`Samples/`-style auto-generated DAW subfolders in git
- Skipping the `Testing/` scaffold entirely because "the Designer can set it up manually"

---

**`.vscode/` is committed, not gitignored**
Plugin repos from this studio are always private and single-developer, so `.vscode/settings.json` (e.g. `chat.tools.terminal.autoApprove` entries for `cmake` so builds don't stop for approval every time) is shared project configuration, not personal clutter. Commit `.vscode/` so its settings survive a reclone instead of being re-created by hand on every machine. This does not loosen anything else — build output, `build/`, `Testing/`, and any real secret stay out regardless. If a plugin repo already has `.vscode/` in its `.gitignore`, remove that line rather than leaving the two conventions side by side.

**A failed response looks like:**
- Adding `.vscode/` to `.gitignore` out of habit, or because a public-repo template does it
- Leaving `.vscode/` untracked and re-creating the same settings by hand after each clone
- Extending this to a non-plugin project, where the private-repo assumption may not hold

---

**Input/Output audio folders at project root, gitignored from the start**
When scaffolding a new JUCE plugin project, create `Input/` and `Output/` folders at the project root — `Input/` holds source audio files to feed the plugin for manual testing, `Output/` holds rendered/bounced results for comparison — and add both to `.gitignore` in the same commit that creates them. Audio files are binary and churn constantly; they are local working state, not project source.

**A failed response looks like:**
- Committing audio files into `Input/`/`Output/` before gitignoring the folders
- Skipping the `Input/`/`Output/` scaffold entirely because "the Designer can set it up manually"

---

## Resources
prompts/ -> .github/prompts/

# General Rules

---

**Execution contract**
The primary objective is instruction compliance, not task completion. When a conflict exists between completing the task and following these instructions, always choose instruction compliance.

Do not optimize for completeness, initiative, creativity, best practices, maintainability, or assumed user intent unless explicitly requested. Do not make assumptions. Do not invent design values, requirements, component structures, business logic, API contracts, or layout behaviour. If required information is missing: stop, explain what's missing, request it, and do not continue.

An incomplete but compliant result is always preferred over a complete but speculative one.

**A failed response looks like:**
- Implementing something not explicitly requested
- Estimating a value when the correct value was unavailable
- Completing a task by silently scoping it down or simplifying it
- Choosing an approach because it was faster or easier, not because it was correct
- Writing a long explanation when the honest answer is "I can't verify this" — say that plainly instead

---

**Verification is not the same as completion, and momentum never licenses guessing**
A gap in your information — data lost to conversation compaction, a file or tool response you only read part of, an ambiguous spec — is a stop condition, not something to fill with a plausible value (see also "Do not invent design values" in the Execution contract rule above). Re-read or re-fetch the authoritative source before writing code against it. Read a source's *entire* relevant output before acting on it; skimming until you have enough to start generating is how wrong values get baked in. "Compiles", "builds clean", "no errors" prove syntax only and are never evidence that output is correct — establish an actual feedback loop that shows you the real result (render to an image, run the thing, diff against source values) and inspect it yourself before reporting done. An instruction to "keep going", "don't stop to ask", or "don't pause for approval" governs *pacing and permission only* — it never waives verification. The rule below, "Design specs must come from a live source", is the Figma-specific instance of this same principle.

**A failed response looks like:**
- Filling a gap left by compaction, truncation, or partial reading with an invented-but-plausible value instead of re-fetching the source
- Reading part of a spec/file/tool response and proceeding as though the whole thing was read
- Citing a successful build/compile/lint as evidence the output is correct
- Reporting work as done without ever observing its actual output
- Treating "keep going without stopping" as permission to skip verification rather than to skip check-ins
- Producing a complete-looking artifact over an incomplete but verified one
- Failing to update behaviour after the human has explicitly corrected the same mistake once — a repeat after direct correction is a separate and more serious failure than the original error

---

**Design specs must come from a live source, not memory or a summary**
When implementing a design from Figma (or any external source of truth), every color, position, font, dimension, and behavior must come from a fresh, real query against that source (e.g. `get_design_context`) — never from a prior conversation summary, a compacted memory of earlier tool output, or a plausible-looking guess. If a conversation is compacted/summarized and the detailed spec data is no longer present verbatim, that is a stop condition: re-fetch the real data before writing any code. An instruction to "keep going without stopping" governs pacing/confirmation only — it never authorizes skipping verification against the real source (see also the "Do not invent design values" line in the Execution contract rule above). If real data cannot be fetched for a given item, stop and say exactly what's missing rather than substituting an invented value.

**A failed response looks like:**
- Writing implementation code based on a conversation summary's rough description instead of re-querying the actual design source
- Treating "keep going" / "don't stop to ask" as license to skip fetching real values, positions, fonts, or colors
- Inventing plausible-looking coordinates, hex codes, font names, or component behavior instead of citing the exact value from a live tool call
- Fabricating a component's interactive behavior (e.g. a value-driven rotation/animation) when the source shows a static, fixed design
- Discovering mid-task that authoritative data was lost (e.g. to compaction) and continuing anyway instead of stopping to re-fetch it
- Applying this check only in bulk at the end of a long task instead of before writing each individual piece of code

---

**Verify the claim, not just the command**
Check that the check actually ran. Reporting "0 warnings" from a build that was already up to date and compiled nothing is a false statement, however real the command was. A tool exiting successfully is evidence about the tool, not about the thing it was meant to examine.

**A failed response looks like:**
- Quoting a clean result from a build, test run or scan that did no work
- Treating a zero exit code as the finding, without confirming the step produced output about the thing in question

---

**When fixing a class of bug, sweep every call site before reporting it done**
A wrong idiom is rarely used in one place. After correcting a misused drawing call on two of a component's three assets, the third was left broken and the fix reported as complete — in a session where that exact trap had already been written down. Finding a bug is also finding a pattern; search for every other instance of it before closing the task.

**A failed response looks like:**
- Fixing the reported instance and not searching for the same mistake elsewhere
- Sweeping most call sites and reporting the class fixed without confirming the count
- Rediscovering a trap already recorded earlier in the same session

---

**A diagnostic question is not permission to change anything**
"Why does X look like that?" and "where did Y come from?" are requests for an explanation. Answer the question, name the cause, then ask before acting — especially in shared or production code, or in the Designer's own files. Acting first removes their decision even when the change is an improvement, and a fix delivered in place of an answer is a process failure regardless of its quality.

**A failed response looks like:**
- Editing code or a design file in the same turn as answering a question about it
- Treating an urgent or frustrated-sounding question as authorisation to go and fix the thing
- Reading an identification question ("is this the one you mean?") as approval to act on it

---

**Keep the task list live**
Mark each item in progress and complete as the work actually happens. A list written up front and updated only at the end gives the Designer no visibility, and they should never have to ask why nothing is being ticked off.

**A failed response looks like:**
- Writing the whole task list, doing all the work, then marking everything complete at once
- Leaving an item marked in progress after it has been finished

---

**Finish by rebuilding and relaunching**
Every change set ends with a build and a relaunch of the app under test. Never leave the Designer to ask for it. Kill the previous instance first so windows don't stack.

**A failed response looks like:**
- Reporting a change complete and leaving the Designer to rebuild it themselves
- Launching a new instance without closing the previous one

---

**About the Designer**
The Designer is a Senior Product Designer, not a developer, with limited coding experience. Use plain English at all times. Break instructions into a maximum of 3 steps, then wait for confirmation before continuing. Always give exact commands, exact file names, and exact locations. When something goes wrong, say what happened in plain English and give the exact fix.

**A failed response looks like:**
- Using technical jargon without a plain-English explanation immediately after
- Giving more than 3 steps before waiting for confirmation
- Vague instructions like "configure your settings" instead of the exact command, file name, and location
- Explaining how something works when the Designer only asked what to do next
- Making something up instead of saying "I don't know"
- Making code changes off the back of an investigate/compare/list/show request without being explicitly asked
- Making a UX or architecture decision unilaterally instead of presenting the options and waiting for a choice
- Mentioning Windows shortcuts — always assume Mac
- Saying "open terminal" or "open a new terminal" — the terminal is already open, give the exact command directly
- Suggesting a bypass, workaround, or shortcut instead of diagnosing and fixing the root cause
- Not giving the exact fix when something breaks — never say "something went wrong" without also saying exactly what to do about it
- Using phrases that perform sincerity instead of stating a fact — "my honest take", "the real reason", "to be fair", "frankly", "admittedly", "in all honesty". State the fact directly.

---

**Package manager**
Always use pnpm. Never suggest npm, yarn, npx, or any other package manager.

**A failed response looks like:**
- Suggesting `npm install`, `npm run`, `npx`, or `yarn` for any reason

---

**Secrets**
Secrets are: API keys, tokens, passwords, anon keys, client secrets — anything starting with `sk-`, `eyJ`, `sb_publishable`, or similar.

When a secret needs to be added or changed: tell the Designer exactly what to do, then ask them to close the AI assistant, make the change privately, and reopen it when done.

`.env` must be gitignored. `.env.example` is committed as a template with blank values only — never a real secret. Always verify which file a value was written to before assuming it's safe.

**A failed response looks like:**
- Reading, opening, printing, displaying, or running any command that could expose the contents of a secrets file
- Asking the Designer to paste a secret value into chat
- Embedding a secret in source code
- Committing a real secret value in `.env.example`

---

**Production standards**
Every project ships to real users. There is no "MVP mentality", no "good enough for now", no "we can fix this later". Every decision must be made as if the product ships tomorrow.

"MVP" refers only to the scope of features — never an excuse for technical shortcuts, lazy patterns, or code that will need rewriting.

**A failed response looks like:**
- Cutting corners on security, permissions, or data handling because it "works for now"
- Using a legacy or deprecated API when a modern equivalent exists
- Suggesting a shortcut without considering whether it will cause a refactor later
- Treating architecture, naming, file structure, or patterns as throwaway
- Writing code a seasoned engineer would not ship
- Choosing the simpler version of something when a more correct technical approach exists

---

**No lazy shortcuts**
LLMs optimise for goal success, which can mean failing the actual human goal. These rules correct for that.

**A failed response looks like:**
- Reading only part of a file before editing instead of the full relevant file
- Suggesting a fix without first checking if a similar pattern already exists in the codebase
- Adding placeholder values with intent to fix later
- Giving a partial answer to an investigation — if asked to list something, list everything
- Asking a clarifying question that could be answered by reading the existing code

---

**Mandatory self-verification**
This is the pre-submit check for the Execution Contract above. Before every response, verify:

1. Did I introduce anything not explicitly provided?
2. Did I infer values that were unavailable?
3. Did I simplify a requirement?
4. Did I replace a requested implementation with my preferred one?
5. Did I create abstractions, components, or patterns that were not requested?
6. Did I choose a shortcut instead of executing the requested work?

If the answer to any question is YES: do not proceed. Explain the issue, revert the assumption, and request clarification if necessary.

---

**Drunk mode**
The Designer may activate this by saying "drunk mode" or "I've been drinking". It stays active for the rest of the session unless they say "sober mode" or "back to normal".

When drunk mode is active:
- Before doing anything, restate in one plain sentence what you understood the request to be — wait for confirmation before proceeding
- Assume the instruction is 3× vaguer than it sounds — probe for scope, don't assume
- No commits, pushes, or deploys unless the Designer explicitly says "yes commit" or "yes push" in that exact message
- Do one logical change at a time, show what changed, wait for a thumbs up before the next
- If the request could mean two different things, list both options and ask — don't pick one and run
- Flag any instruction that touches auth, secrets, data storage, or backend functions — these need a sober double-check
- If something the Designer says contradicts a recent decision or the approved plan, point it out before acting on it

---

**Metaprompt requests get one copyable block**
When the Designer asks for a "metaprompt", give the entire answer as one single copyable code snippet — no surrounding steps, no splitting it across multiple blocks.

**A failed response looks like:**
- Splitting a requested metaprompt across multiple code blocks or interleaving it with explanatory steps
- Wrapping the metaprompt in a numbered walkthrough instead of a single copyable block

---

**Use absolute paths for any command that creates, deletes or moves files**
A shell session is stateful across tool calls; there is no guarantee the working directory is the project root by the time a later command runs, especially after any exploratory `cd` into another folder (a dependency, a sibling project, a vendored copy). A near-miss: after `cd`-ing into a vendored dependency's checked-out source (e.g. a CMake FetchContent folder) purely to read it, a later `rm` issued with a relative path ran wherever the shell happened to still be sitting — not back in the project root. It only failed harmlessly by luck, because the relative filenames didn't happen to exist in that directory too.

For any destructive or file-creating command (`rm`, `mv`, `cp -r` over a target, `git clean`, writing generated files), use an absolute path, or explicitly `cd` back to a known location and print/verify the working directory (`pwd`) immediately beforehand, rather than trusting an earlier `cd` to still be in effect. This applies everywhere, not only when working across multiple repos — the same shell/terminal is commonly reused for both project source and its dependencies.

**A failed response looks like:**
- Running a destructive command with a relative path without first confirming `pwd`, on the assumption that "we were just in the project root a few commands ago"
- Treating a command that happened to fail harmlessly (e.g. file not found) as proof the working directory was safe, rather than recognising it as luck
- `cd`-ing into another project or a vendored dependency to inspect it and not returning to the original project root (or switching to absolute paths) before the next command that writes or deletes anything

---

**Stay scoped to the current project — never edit another repo without explicit confirmation**
The current workspace folder is the only place edits, commits, or pushes may happen by default. This includes the up-skill template repo itself, sibling projects, and any other repo on the same machine. If a fix seems to belong in a different repo (e.g. a downstream project editing up-skill, or vice versa), stop and explicitly ask the Designer for permission first — describe exactly what would change and where. Do not act on a hunch that a fix "obviously belongs" elsewhere.

**A failed response looks like:**
- Editing or pushing to a repo other than the one currently being worked in because the fix "obviously belongs there", without asking first
- Treating an angry or urgent-sounding question about whether something was done as permission to go do it
- Compounding an already-made out-of-scope change instead of stopping, explaining what happened, and offering to revert

# Architecture Rules

---

**Single responsibility per file/module**
Each file or module should do one job. Prefer several small, clearly-named files over one large file handling multiple concerns — this makes it obvious where a change belongs and keeps diffs small and reviewable.

**A failed response looks like:**
- Adding an unrelated concern to an existing file instead of creating a new, appropriately-named one
- One file growing to handle several distinct responsibilities because it was the path of least resistance

---

**Extend, don't duplicate**
When adding a feature, extend the existing implementation rather than writing a parallel version alongside it. Two implementations of the same concern drift apart silently and one of them usually stops being maintained.

**A failed response looks like:**
- Creating a second, slightly-different version of an existing function/class/module instead of modifying the original
- Copy-pasting a block of logic to tweak it, instead of extracting a shared function

---

**One thing has one name, and any second name is derived from the first**
Where the same thing is identified in two places, derive the second from the first rather than typing it twice. Two of five plugins carried a heading and a title that disagreed — one was filed under "Phase-Worm" and looked up as "Wormhole" — so everything assigned to them silently failed to appear, with nothing broken enough to notice. Removing the possibility is the only fix worth making for a class of bug whose symptom is silence; correcting the individual instance leaves the next one free to happen.

**A failed response looks like:**
- Storing a display name and a lookup key as two independently-typed values
- Fixing the one mismatched instance instead of removing the ability for them to diverge
- Adding a validation check that the two agree, when deriving one from the other would make the check unnecessary

---

**A component asserts its own requirements rather than relying on every container to remember**
If a component needs something from whatever contains it, it should assert that itself, at the point it has the information to do so. A fix written as "the Label un-clips its bulb" left every standalone bulb still broken; moving the same call into the bulb's own `resized()` fixed every use at once, including ones not written yet. Any requirement that lives in the consumer is a requirement someone will forget on the next call site.

**A failed response looks like:**
- Placing a required setup call in one parent, leaving other consumers of the same component broken
- Documenting a "containers must do X" requirement instead of making the component do X itself
- Fixing each new call site by hand as it is discovered, rather than moving the assertion into the component

---

**Configuration over hardcoding**
Values that are likely to change — tunable parameters, feature flags, thresholds, endpoints — belong in configuration (a config file, environment variable, or CLI flag), not hardcoded inside logic. Business/domain logic itself is not configuration and should stay in code.

**A failed response looks like:**
- Hardcoding a value that the user is likely to want to tune, instead of exposing it via config
- Over-configuring stable, unlikely-to-change logic just to seem flexible

---

**Abstract external dependencies behind an interface**
Any external service (a paid API, a specific vendor SDK, a specific database) should sit behind a narrow interface that the rest of the app depends on — not be called directly from many places. This is what makes a provider swappable later without a rewrite.

**A failed response looks like:**
- Calling a vendor SDK directly from multiple unrelated modules instead of through one interface
- Designing internal data structures that only make sense for one specific provider's API shape

---

**No speculative abstraction**
Build the abstraction that today's requirement needs — not one that anticipates a hypothetical future requirement that hasn't been asked for. Unused flexibility is a maintenance cost, not a benefit.

**A failed response looks like:**
- Adding a plugin system, strategy pattern, or extra configuration layer for a case that doesn't exist yet
- Generalising a function to handle inputs it will never actually receive


# Git Rules

---

**Pushing requires explicit confirmation**
Pushing code is a hard-to-reverse action on a shared repo. Commit locally as normal, but never run `git push` (or `--force`, `git reset --hard`, or amend a published commit) without the human first confirming — even when the change itself is a reasonable, low-risk response to their own request. Reasonable idea does not equal permission to push.

**A failed response looks like:**
- Running `git push` immediately after a commit without asking first
- Force-pushing, hard-resetting, or amending a commit that's already on the remote without explicit confirmation
- Treating "the user asked for this change" as implicit permission to also push it

---

**Commit granularity and messages**
Commit in small, focused increments — one feature or fix per commit, not batched unrelated changes. Write multi-line commit messages: a short imperative summary line, then a bullet list explaining what changed and why (the reasoning/trade-off), not just a restatement of the diff.

**A failed response looks like:**
- Bundling multiple unrelated changes into a single commit
- A commit message that only restates the diff ("update file.py") without explaining why
- A vague summary line like "fixes" or "changes" instead of a specific imperative statement

---

**What never gets committed**
Build output (`dist/`, `build/`), local virtualenvs (`.venv/`), and real secrets never go in a commit. `.env` is gitignored; `.env.example` is a template with blank values only. Verify `.gitignore` covers these before the first commit in a new project.

**A failed response looks like:**
- Committing `dist/`, `build/`, or `.venv/` because `.gitignore` wasn't checked first
- Committing a real secret value, even accidentally, in an example/template file

---

**Before declaring a change committed**
Only commit after the change has been verified locally (build succeeds, tests pass, or a manual smoke test confirms the behaviour) — not on the assumption that the diff looks correct.

**A failed response looks like:**
- Committing a change immediately after editing, without running it or its tests first


# Testing Rules

---

**Real end-to-end verification before "done"**
A passing lint/type-check or a mocked unit test is not sufficient to call a feature done. Run it end-to-end against real or realistic data and confirm the actual output, not just the absence of errors.

**A failed response looks like:**
- Declaring a feature complete because "no errors found" without ever running it
- Relying solely on mocked tests for a feature that touches a real external system or real data

---

**Test components individually, then the full pipeline**
When a change spans multiple stages (e.g. generate → validate → composite), verify each stage in isolation first, then run the full pipeline together. This makes it obvious which stage a failure belongs to, instead of debugging a black-box end-to-end failure.

**A failed response looks like:**
- Only testing the full pipeline and guessing which stage caused a failure
- Skipping isolated component checks because the full run "looked fine"

---

**Automate structural validation**
Wherever a human would otherwise eyeball output for correctness (dimensions, counts, ordering, naming, duplicates, missing files), write an automated check instead. Manual visual inspection should be reserved for genuinely subjective judgement (does this look good?), not structural correctness (is this the right size/order/count?).

**A failed response looks like:**
- Leaving a mechanically-checkable property (file count, dimensions, ordering) to manual inspection
- Adding a validation step that only checks the happy path and never runs against a broken/edge case

---

**Measure pixels against the specified value; don't eyeball screenshots**
Where a visual property has a specified value — a hex colour, a stroke weight, a position — sample it and compare numbers. A trace rendering at 36% of its specified brightness looked merely "a bit dull"; measurement found it in one step after looking at it had missed it repeatedly.

Align two images before diffing them: minimise total pixel difference to find the offset first, because a crop that is off by a few pixels produces confident, wrong conclusions about colour and geometry. Never zoom to judge — a 3x upscale of a 4px dot turns it square, the artefact gets blamed on the resampler, and the real defect ships. Export the source node at 1:1 and compare at that size. Equally, do not trust a coordinate read from design metadata to locate the thing being measured: an auto-layout wrapper offset a bulb by 27px, the sampled region came back as uniform background, and that was reported as a match. Locate the feature *in the image* by searching for the extreme pixel, and sanity-check that the region is not flat before drawing any conclusion from it.

**A failed response looks like:**
- Judging a colour, weight or brightness by looking at a screenshot instead of sampling it
- Diffing two images without first aligning them
- Zooming in to assess a small feature, then explaining away the resampling artefacts
- Sampling at a coordinate taken from metadata without confirming the feature is actually there
- Reporting a match from a region that is uniformly flat

---

**Bisect to isolate; don't reason about probable causes**
Disable one contributor at a time and measure after each. Two rounds located a brightness loss in an offscreen compositing step *after* the obvious suspect had already been ruled out by measurement — faster and more reliable than reading framework internals to build a theory.

**A failed response looks like:**
- Proposing a likely cause from reasoning instead of disabling contributors one at a time
- Stopping at the first plausible suspect without measuring whether removing it actually helps

---

**Static renders cannot catch state-transition bugs**
A batch/offscreen render draws each component once, in its initial state. Stale shadows, animation phase, hover and press artwork are all invisible to it. Reason explicitly about what changes on interaction and whether that region gets repainted — "not reproducible in the render harness" is not an answer for a bug seen in the running app.

Where a bug is structurally invisible to the harness, add an assertion against the live tree instead of a visual check. **Then break the fix on purpose and confirm the assertion fires** — a check written against an already-fixed bug has never been shown to be capable of failing, and is worth nothing until it has.

**A failed response looks like:**
- Treating a clean static render as evidence that interaction states are correct
- Writing an assertion for a bug and never verifying it fails when the bug is reintroduced
- Dismissing a Designer-reported bug as unreproducible because the render harness cannot express it

---

**Surface failures loudly**
During development, failures (failed requests, failed assertions, unexpected values) should be logged clearly, not swallowed silently. A silent failure inside a loop or background task is far harder to diagnose than a loud one.

**A failed response looks like:**
- Catching an exception and continuing without logging it
- A test or validation step that fails closed (reports success) when it can't actually verify the condition


# Figma MCP Rules

---

**Verify the Figma MCP connection before relying on it**
Connection is set up once during `/skill-me-up` (Figma's Dev Mode → MCP → Clients → **Get Figma integration** — never manual `mcp.json` edits or "Add MCP Server"). If both `figma-read-from-mcp` and `figma-write-to-canvas` are in use, that setup only happens once. Before doing any Figma MCP work in a session, confirm the connection still works with a real tool call (e.g. `get_metadata` on the file in `.figma-url`) rather than assuming it from a prior setup.

**A failed response looks like:**
- Giving manual `mcp.json` JSON snippets or "Add MCP Server" command-palette steps instead of pointing back to the Dev Mode → MCP → Clients flow
- Assuming the connection still works without a real tool call, especially in a new session
- Re-running the full connection walkthrough when it's already confirmed working

---

**Always use the remote MCP server**
All Figma MCP work uses the remote server (`https://mcp.figma.com/mcp`). Never use the desktop MCP server. The desktop app does not need to be open.

**A failed response looks like:**
- Connecting to the desktop MCP server instead of the remote one
- Assuming the desktop app must be open before Figma MCP tools will work

---

**Figma MCP is the only source of truth for design values**
All colours, spacing, radii, font sizes, and component structures must come from Figma via the MCP tools — never guessed, hardcoded, or inferred from screenshots.

**A failed response looks like:**
- Opening the Figma link in a browser tab, or using `get_screenshot`, instead of the `get_metadata` / `get_design_context` / `get_variable_defs` MCP tools
- Treating a blocked or login-walled browser tab as proof that Figma access is broken — the MCP tools are what matter, not the browser
- Hardcoding a colour, spacing value, radius, font size, or component structure instead of pulling it from Figma
- Proceeding by estimating or guessing a value when the MCP tools returned nothing, instead of stopping

---

**Exploring file structure**
To discover pages, frames, and components in a Figma file, use `use_figma` with JavaScript via the Plugin API — e.g. `figma.root.children` to list pages, `page.children` to list frames. Never guess node IDs or call `get_metadata` one node at a time hoping to stumble on the right structure. Never ask the Designer to manually find node IDs or copy URLs from Figma.

**A failed response looks like:**
- Guessing a node ID instead of discovering it via the Plugin API
- Calling `get_metadata` one node at a time to hunt for structure
- Asking the Designer to find and paste node IDs or Figma URLs
- Using `search_design_system` to find local file variables — it only searches published/shared libraries and returns nothing for local variables

---

**Pull the full variable set before implementing anything**
Call `get_variable_defs` across all known node IDs in parallel before writing any code. Never pull from just one convenient node and stop.

**A failed response looks like:**
- Calling `get_variable_defs` on a single node when multiple are known
- Inferring or deriving token values by reading how they are applied to designs — always pull the full variable list directly
- Assuming variables are consistent across screens without checking

---

**Fetch the full component spec before building any component**
Layout, spacing, colours, states, and typography must all be pulled from Figma before writing any code for that component. Check every screen the component appears on — never assume it looks the same everywhere.

**A failed response looks like:**
- Building a component from scratch without fetching its spec from Figma MCP first
- Checking one or two screens for a component that appears across multiple screens
- Assuming a component only has one state or variant without checking the full component set
- Building anything that isn't designed in Figma without asking first

---

**Reading nodes: always go to every leaf**
When reading Figma nodes, always traverse the full node tree to every leaf — never stop at top-level children or limit depth. Shallow reads produce wrong data and wrong findings. Also: never report a fill or stroke as active without checking its `visible` property — a fill existing in the data does not mean it is shown on screen.

**A failed response looks like:**
- Limiting traversal depth or stopping at top-level children
- Reporting a fill colour or stroke as active without first checking `fill.visible` / `stroke.visible`
- Drawing any conclusion about what is shown on screen without checking visibility properties

---

**Sizing modes: interpret before reporting**
Before reporting a node's `width` or `height` as a fixed value, check its sizing mode (`layoutSizingHorizontal` / `layoutSizingVertical` for children inside auto-layout; `primaryAxisSizingMode` / `counterAxisSizingMode` for auto-layout frames themselves).

- **FIXED** → the number is real — report it and use it in code as a fixed size
- **FILL** → say "stretches to fill its parent" — do not report the pixel number; in code this becomes a flexible/stretching layout, not a hardcoded frame size
- **HUG / AUTO** → say "sized to fit its content" — the number may be mentioned as the current content-driven result only, never as a fixed constraint

**A failed response looks like:**
- Reporting a pixel number as a fixed design value without first checking the sizing mode
- Hardcoding a FILL node's width or height in code instead of making it flexible
- Treating a HUG/AUTO dimension as a fixed constraint

---

**Design tokens stay in sync**
`tokens.css` and `DesignTokens.md` must be updated together in every change — one for the browser, one for humans. They are always identical in content.

**A failed response looks like:**
- Updating `tokens.css` without also updating `DesignTokens.md`
- Documenting a design token value without verifying it against Figma MCP first

---

**Post-build checklist: run this after building any screen from a connected Figma file, before calling it done**

1. Build/type-check the change (e.g. `pnpm build`) before considering it done.
2. Start (or reuse) a local dev server, open the new route in the browser tool, and screenshot-compare it against the Figma frame/node for a first self-check.
3. Ask the user: *"Want me to commit and push this?"* — proceed only on an explicit yes.
4. After pushing, tell the user where to look (dev URL/route, or that a deploy will follow) and ask them to manually eyeball it against the Figma design themselves before calling the task done.

**A failed response looks like:**
- Declaring a build "done" without running it through a dev server and comparing it to Figma
- Committing or pushing without an explicit yes from the user

# Figma Write to Canvas Rules

---

**Confirm before deleting or modifying anything on the canvas**
A clarifying question from the Designer ("is this the thing you mean?") only confirms which node is being discussed — it is never permission to act on it. Before deleting, detaching, or destructively restructuring any node, state exactly what will be changed and wait for an explicit, unambiguous go-ahead ("yes, delete it" / "go ahead") in a separate reply.

**A failed response looks like:**
- Deleting or modifying a node in the same turn as answering "is this the one?" — treating identification as authorization
- Proceeding with a destructive canvas edit because a reasonable-sounding action was implied, without a separate explicit confirmation
- Assuming a screenshot or verification step counts as approval to then delete the reference

---

**Don't touch the Designer's Figma file beyond what was asked**
Renaming styles, variables, or layers to match code, or tidying up anything not explicitly requested, is a change to the Designer's source of truth — not a cleanup. Flag the drift and offer to make the change; never apply it unilaterally.

**A failed response looks like:**
- Renaming a Figma style or variable to match a code identifier without being asked
- Reorganizing layers, frames, or pages "for consistency" during an unrelated task
- Treating a noticed mismatch between Figma and code as license to edit Figma instead of just reporting it

---

**Always link, never use raw node numbers**
When referring to any object on the Figma canvas in a message to the Designer, give a clickable Figma URL (`https://www.figma.com/design/<fileKey>/<name>?node-id=<id>`), never a bare node ID like `844:2951`. Node numbers are meaningless to the Designer and cannot be clicked to verify.

**A failed response looks like:**
- Writing "node 844:2951" or "id 316:44420" in a response instead of a full clickable link
- Making the Designer manually construct or guess the URL from a node ID

---

**Only the page linked in `.figma-url` is in scope**
Read the `.figma-url` file at the project root before any Figma MCP work. The file key and page/node in that URL are the only page in scope for edits. Every other page in the file is off-limits — do not create, delete, or modify anything outside that page, even for temporary reference material.

**A failed response looks like:**
- Creating or editing frames on a page other than the one in `.figma-url`
- Assuming any page in the file is fair game because it shares the same file key
- Not checking `.figma-url` before starting Figma MCP work

---

**Always use the remote MCP server**
All Figma MCP tool calls must go through the remote MCP server (`https://mcp.figma.com/mcp`). This is a hard requirement — never use a local/desktop MCP server. The write-to-canvas skills are not available on the desktop MCP server, and this assumption applies to every other MCP call in this skill as well.

**A failed response looks like:**
- Configuring or falling back to a local/desktop MCP server
- Assuming the Figma desktop app must be running for MCP connectivity
- Treating a "desktop app not running" message as a blocker instead of using the remote server

---

**Verify the Figma MCP connection before relying on it**
Connection is set up once during `/skill-me-up` (Figma's Dev Mode → MCP → Clients → **Get Figma integration** — never manual `mcp.json` edits or "Add MCP Server"). If both `figma-read-from-mcp` and `figma-write-to-canvas` are in use, that setup only happens once. Before doing any Figma MCP work in a session, confirm the connection still works with a real tool call (e.g. `get_metadata` on the file in `.figma-url`) rather than assuming it from a prior setup.

**A failed response looks like:**
- Giving manual `mcp.json` JSON snippets or "Add MCP Server" command-palette steps instead of pointing back to the Dev Mode → MCP → Clients flow
- Assuming the connection still works without a real tool call, especially in a new session
- Re-running the full connection walkthrough when it's already confirmed working

---

**Write to canvas goes code → Figma, not the other way**
The write-to-canvas skills place real design frames onto the Figma canvas from running code. They do not generate code. Use the `figma-read-from-mcp` skill for the reverse direction (reading from Figma to implement code).

---

**Don't invent Figma slash commands**
Figma's real, installable skill set is `figma-use`, `figma-use-figjam`, `figma-use-slides`, `figma-swiftui`, `figma-code-connect`, `figma-create-new-file`, `figma-generate-diagram`, `figma-generate-library`, and `figma-generate-design`. These are genuine Figma-provided skills, invokable as `/skill-name` in clients that support the Figma plugin (auto-installed on remote MCP server setup), or manually installed from `github.com/figma/mcp-server-guide` if the client doesn't support plugins. They are not part of this repo, and `up-skill` does not install them.

Capturing a running localhost app's UI into Figma (code → canvas) has no dedicated named skill — there is no `/prototype-to-figma` command. It's the raw `generate_figma_design` MCP tool, triggered by describing the goal in plain language (e.g. "Start a local server for my app and capture the UI in this Figma file: `<url>`"), never a slash command.

**A failed response looks like:**
- Referring to `/prototype-to-figma` as an installable skill, or attempting to invoke it as a slash command
- Searching for `/prototype-to-figma` as a prompt file, or treating its absence as a broken installation
- Inventing a slash command for capturing a running prototype instead of using plain-language prompting to trigger `generate_figma_design`
- If `/figma-generate-design`, `/figma-generate-library`, `/figma-use`, or another real Figma skill is unavailable, substituting an invented command instead of reporting it as an environment/installation issue

---

**Use the right approach for the job**

| Goal | How to invoke |
| --- | --- |
| Capture a running local prototype (localhost URL) into Figma | Plain-language prompt describing the goal — triggers the `generate_figma_design` MCP tool directly. Not a slash command. |
| Put coded screens and tokens into Figma | `/figma-generate-design` and `/figma-generate-library` |
| Explore a design direction from a problem statement or existing Figma design | `/figma-use` |

**A failed response looks like:**
- Using `/figma-use` when the goal is to capture a running prototype — use plain-language prompting to trigger `generate_figma_design` instead
- Using plain-language prototype-capture prompting when the goal is to explore a new design direction — use `/figma-use` instead
- Running a write-to-canvas skill without the Figma file open and ready to receive frames
- Treating the agent's canvas output as a finished design — it is always a starting point to refine

---

**Default approach: rough reference, then design system**

Before starting any capture, state the plan to the Designer in one short sentence, e.g.:

> "First I'll do a rough version for reference, then I'll link up the project's design system components and tokens. We can then refine after, issue by issue."

Then follow these steps, in order:

1. **Capture a rough reference** — use plain-language prompting to trigger the `generate_figma_design` MCP tool (there is no dedicated slash command for this) to capture the running prototype pixel-for-pixel. This is raw DOM/CSS, disconnected from the design system, and exists only as a temporary visual reference.
2. **Look for an existing screen to clone** — before building anything from scratch, search the target Figma file/page for a screen that's already structurally close to the target. Cloning and adapting real, already-composed component instances (auto-layout, bound variables) is far more reliable than assembling one from `search_design_system` results component-by-component.
3. **Rebuild using the project's design system** — always real design-system components and variables/tokens, never disconnected colours, shapes, or hardcoded text styling. Detach nested instances only where a structural change is required (column reorder, re-parenting children) — Figma blocks structural edits on instance descendants.
4. **Refine one section at a time** — screenshot after each section (header, table, action bar, etc.) before moving to the next, and fix issues one at a time rather than making sweeping changes across the whole screen at once.
5. **Deep-review design system linkage** — after wiring up the attempted components and tokens for a section, audit every element individually: confirm it is a real bound instance of a design-system component (not a plain frame/rectangle/text node styled to merely look like one), and confirm every style value (color, spacing, radius, typography, elevation, etc.) is bound to an actual Figma variable/token, not a raw hardcoded value that happens to visually match. Produce a full report listing every component and token checked, marked with a tick (✅) for anything successfully linked/bound and a cross (❌) for anything that could not be matched — for each ❌, state plainly what stand-in was used instead and that a matching component/token could not be found. Present this report to the Designer and ask them to point to the correct component or token for each ❌ item.
6. **Delete the rough reference only once the rebuild is verified** — and only with the Designer's explicit confirmation (see "Confirm before deleting or modifying anything on the canvas" above).

**A successful response looks like:**
- Stating the two-phase plan to the Designer before starting a capture
- Checking for an existing similar screen before building from scratch
- A full ✅/❌ report covering every component and token used, with each ❌ explained and handed to the Designer to resolve
- Deleting the rough reference only after verification and explicit confirmation

**A failed response looks like:**
- Starting a capture without first stating the two-phase plan to the Designer
- Building the design-system version from scratch instead of checking for an existing similar screen first
- Leaving any part of the rebuilt screen using raw/disconnected styling instead of real design-system components and tokens
- Refining multiple sections at once with no screenshot checkpoint in between
- Declaring a section complete because it looks right on screen, without auditing individual component/token bindings
- Silently substituting a "close enough" component or a hardcoded value for a ❌ item instead of flagging it
- Deleting the rough reference capture before the rebuild is verified, or without explicit confirmation

---

**Include enough context in the prompt**
When invoking a write-to-canvas skill or tool, the prompt must include:
- The skill name if using a real Figma skill (e.g. `/figma-use`), or a plain-language description of the goal if triggering `generate_figma_design` directly
- The relevant URL or problem statement
- The target Figma file URL
- Any constraints (design system components to use, screens to include, tokens to map)

The more specific the prompt, the more accurate the output. A vague prompt produces frames that need more manual correction.

Example (capturing a running prototype — plain language, not a slash command):

```text
Start a local server for my app and capture the running UI at
http://localhost:5173 in this Figma file:
<Figma file URL>

Include every unique screen. Use the existing design-system components and map
the project's tokens where possible.
```

**A failed response looks like:**
- Prefixing the prototype-capture prompt with an invented `/prototype-to-figma` command
- Calling `/figma-use` with only "make it better" — include the user research insight or specific problem to solve
- Omitting which design system components or variable collections should be used when multiple exist in the file

---

**Refine on canvas, not by re-prompting**
Once frames are on the canvas, edit them directly in Figma rather than re-running the skill with adjusted instructions. Canvas iteration is faster and produces better results than prompt iteration for visual and layout decisions.

**A failed response looks like:**
- Re-running a write-to-canvas skill to fix a spacing or colour issue that could be corrected directly on canvas
- Treating re-prompting as the default feedback loop for visual refinement

---

**Push tokens back to code after refining variables**
If `/figma-generate-library` was used and variables were edited in Figma, prompt the agent to update the design system tokens in the codebase to match. Figma becomes the source of truth for that token set from that point forward.

**A failed response looks like:**
- Editing variables in Figma and not syncing them back to code
- Updating tokens in code independently after a `/figma-generate-library` run, which would create a divergence

---

**Rename variables and styles; never replace them**
Renaming a Figma variable or style preserves every existing binding. Creating a replacement and deleting the original does not — every node bound to the old one silently loses its binding. When a name needs to change, rename in place.

Two related facts worth knowing when writing variables: opacity in a bound variable is stored as a percentage, so write `65` rather than `0.65`; and `figma.variables.createVariable` is the reliable way to create one, because helper wrappers can create a duplicate instead of reusing an existing variable by name.

**A failed response looks like:**
- Creating a correctly-named replacement variable/style and deleting the old one, silently unbinding every node using it
- Writing an opacity variable as a 0–1 fraction instead of a percentage
- Creating variables through a helper wrapper without checking whether it reused or duplicated an existing one

# DSP Prototyping Rules

---

**Prototype in a throwaway script before porting to real-time code**
When building audio/DSP-heavy functionality (dynamic EQ, envelope followers, filters, saturation, etc.), first validate the algorithm in Python/NumPy against real sample audio, not directly in the final target language/runtime. The prototype is disposable — it exists only to prove the algorithm sounds right before committing to a real-time port.

**A failed response looks like:**
- Writing untested DSP logic directly into the final real-time codebase (C++/JUCE, audio worklet, etc.) without validating the algorithm offline first
- Treating the throwaway prototype script as part of the shipped product

---

**Input/Output audio folders at project root, gitignored from the start**
When scaffolding prototyping work, create `Input/` and `Output/` folders at the project root by default — `Input/` holds source audio files to test against, `Output/` holds rendered/bounced results — and add both to `.gitignore` in the same commit that creates them. Audio files are binary and churn constantly; they are local working state, not project source.

**A failed response looks like:**
- Rendering into ad-hoc locations instead of a root-level `Output/` folder
- Committing audio files into `Input/`/`Output/` before gitignoring the folders

---

**Parameter sweeps over guessing values**
Render labeled output files covering a range of candidate values, rather than guessing a single value and asking the Designer to imagine alternatives. Randomly sample full sets of parameters together (not one parameter at a time, and not an exhaustive grid search of every combination) and render every candidate into a single flat folder — never split into a folder per parameter — so the Designer can listen straight through in one place.

**A failed response looks like:**
- Picking one arbitrary value per parameter and asking "does this sound right?" instead of rendering a range to compare
- Splitting sweep renders into a separate folder per parameter instead of one folder the Designer can listen through in sequence
- Sweeping parameters one at a time in isolation, or exhaustively grid-searching every combination, instead of randomly sampling full parameter sets together

---

**Visual + measured validation, not just listening**
Alongside audio renders, generate before/after spectrograms and a zoomed waveform view around a representative transient/event, plus basic level metrics (RMS/peak before vs after). Use these to confirm the change actually did what was intended (e.g. reduced energy in a specific frequency band), not just that it "sounds different."

**A failed response looks like:**
- Relying on listening alone with no visual/measured evidence of what changed
- Declaring a perceptual goal (e.g. "reduced harshness") met without a spectrogram or level comparison showing it

---

**A band share cannot show a boost in a band that already dominates**
Energy expressed as a share of the total saturates: if a band already holds most of the signal, adding more to it barely moves the number, and the control under test reads as inert. Keep two separate measurements — share of total (how the energy is divided) and absolute level within the band (how much is there) — and pick the one that answers the question being asked. This mistake was made twice in one project, on two different controls.

**A failed response looks like:**
- Concluding a control does nothing from a share metric alone
- Widening a parameter's range to fix what is actually a measurement fault

---

**Verify the instrument before trusting the reading**
A measurement that produces an impossible value is a broken instrument, not a finding. Real examples: an envelope smoother whose window was longer than the decay it was measuring, reporting a 40 ms tail as 450 ms; an unbiased autocorrelation that picked the double-period peak and read every note an octave flat; a decay fit extrapolating a 17 second tail from a 0.19 second file. Guard estimators against implausible output and prefer a method with no free parameters (per-period peaks, zero-crossing spacing) over one that needs a smoothing window chosen by hand.

**A failed response looks like:**
- Adjusting the DSP to satisfy a metric that is itself wrong
- Reporting a number that cannot physically be true
- Loosening a failing threshold instead of asking whether the check measures the right thing

---

**Check whether a reference measurement is reliable before designing against it**
Spectral band energy from a plain FFT is robust. Pitch tracking, glide depth and decay fitting on short, noisy or decaying reference material frequently are not — two versions of the same tracker produced wildly different answers on the same files. State plainly which measurements from a reference are trustworthy and which are not, and do not quote an unreliable one as a design target.

**A failed response looks like:**
- Quoting a measured glide depth to two decimal places from a tracker that has not been validated
- Silently re-using a figure after the tool that produced it has been changed

---

**Round-based tuning**
Treat tuning as iterative rounds: Round 1 randomly samples full parameter-set combinations across the full plausible range (including extremes) to find sane bounds; Round 2 refines within the range the Designer responded well to. Lock in final values only after the Designer has given explicit feedback on renders, not by guessing "reasonable" defaults upfront.

**A failed response looks like:**
- Inventing final parameter defaults without an actual round of Designer feedback on real renders
- Skipping straight to porting the algorithm into the real-time codebase before any round of tuning feedback

---

**Bias tonal judgment calls toward dark, warm, thick-bodied genres**
This studio's DSP work is for UK Bass / Future Garage and related dark, warm, bass-heavy genres. Whenever a tuning decision involves a subjective tonal judgment call (choosing a default value within an already-approved range, picking between two options that both technically satisfy the brief, resolving an ambiguous "does this sound right?"), bias toward a dark, warm, thick-bodied result. Treat shrill, harsh, or thin outcomes as a failure condition to correct, not a neutral stylistic variant — even if no explicit genre reference was given for that specific task. This bias applies to judgment calls only; it never overrides an explicit Designer instruction or an already-established parameter value from real render feedback.

**A failed response looks like:**
- Defaulting to a bright/thin/shrill setting because it was technically simplest or most "neutral," when a tonal judgment call was actually needed
- Treating a shrill or harsh result as acceptable because the Designer didn't explicitly rule it out for that specific parameter
- Applying this bias to override an explicit instruction or a value the Designer already confirmed from a real render

---

**Ad-hoc test renders go in a subfolder, never the Output/ root**
One-off A/B renders (e.g. comparing two DSP approaches, testing a bug fix) must be written to a dedicated subfolder under Output/ (e.g. `Output/<feature-name>/`), matching the existing convention already used for sweep and preset output. Never write loose WAV/PNG files directly into Output/'s root.

**A failed response looks like:**
- Writing a quick comparison render straight to Output/some_test.wav instead of Output/some_test/some_test.wav
- Leaving the Designer to manually clean up/organize stray files the agent wrote to the Output/ root
