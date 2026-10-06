"""Fault injection: does the suite actually catch the faults it was built for?

A green suite proves the current code passes. It does not prove the suite
would notice if the code stopped being right, and those are different claims.
This makes the second one checkable: each entry below is a fault that was
really in the instrument, with the assertion that is supposed to catch it.
The fault goes back in, the suite runs, and the named assertion has to fail.

Three of the first ten regressions written for this project did not fail when
their own fault was reintroduced -- two because the test reimplemented the
formula it was checking rather than reading the engine, and one because the
change is smaller than the golden render's tolerance and nothing else looked
at it. None of that was visible from a passing run.

    ./.venv/bin/python scripts/fault-injection.py
    ./.venv/bin/python scripts/fault-injection.py --only snap

Refuses to start on a dirty tree, and restores with git rather than by
reversing its own edit, so an interrupted run cannot leave a fault behind.

Restoring the sources is not the whole of putting things back. The build
directory still holds the artefacts compiled from the last fault, so a clean
`git status` can sit over faulty binaries and every later harness run measures
the injected fault without saying so. This harness is the one thing that
deliberately creates that state, so it is the one thing that has to clear it:
the invariant on exit is restored source, freshly rebuilt artefacts and a
green baseline, and it does not exit 0 without all three.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Fault:
    def __init__(self, key: str, what: str, edits, caught_by):
        self.key = key
        self.what = what
        self.edits = edits
        self.caught_by = caught_by


FAULTS = [
    Fault(
        "resonator-clamp",
        "RADIATION's damping pinned by a clamp that binds at every setting",
        [("Source/Dsp/Radiation.h",
          "bandwidth = std::max (bandwidth, 0.2);",
          "bandwidth = std::max (bandwidth, 5.0);"),
         ("Source/Dsp/Radiation.h",
          "std::clamp (std::exp (-M_PI * bandwidth / sr), 0.0, 0.99995)",
          "std::clamp (std::exp (-M_PI * bandwidth / sr), 0.0, 0.99)")],
        ["Test 12 RADIATION DECAY", "Test 12 RADIATION EXPOSURE"],
    ),
    Fault(
        "decay-as-tau",
        "decay_time read as a 1/e constant rather than the t60 it is written as",
        [("Source/Dsp/Radiation.h",
          "bandwidthForT60 (decayTime) * (1.0 - 0.7 * p.exposure)",
          "1.0 / (M_PI * std::max (decayTime, 0.005)) * (1.0 - 0.7 * p.exposure)")],
        ["Test 17 RADIATION golden"],
    ),
    Fault(
        "fission-dc-normalised",
        "FISSION's modulators normalised by DC gain instead of energy",
        [("Source/Dsp/Fission.h", "bM = std::sqrt (1.0 - aM * aM);", "bM = 1.0 - aM;"),
         ("Source/Dsp/Fission.h", "bD = std::sqrt (1.0 - aD * aD);", "bD = 1.0 - aD;")],
        ["Regression FISSION modulator has its full range",
         "Regression FISSION branches decorrelate"],
    ),
    Fault(
        "toxicity-fixed-gamma",
        "TOXICITY driving the saturator's gain while its asymmetry stays fixed",
        [("Source/Dsp/Sludge.h",
          "gamma = kGammaLo + (kGammaHi - kGammaLo) * p.toxicity;",
          "gamma = 0.6;")],
        ["Regression TOXICITY raises even orders throughout"],
    ),
    Fault(
        "memory-unnormalised",
        "the long memory saturated without a reference, so tanh stays linear",
        [("Source/Dsp/Sludge.h", "std::tanh (m / kMemoryRef)", "std::tanh (m)")],
        ["Regression the memory reaches the cutoff"],
    ),
    Fault(
        "half-life-mismapped",
        "HALF-LIFE reaching only half the time-constant range it asks for",
        [("Source/Dsp/Sludge.h",
          "const auto tauM = kTauMLoS + (kTauMHiS - kTauMLoS) * p.halfLife;",
          "const auto tauM = kTauMLoS + (kTauMHiS - kTauMLoS) * 0.5 * p.halfLife;")],
        ["Regression HALF-LIFE sets the decay it asks for"],
    ),
    Fault(
        "snap",
        "the snap's two followers made a cascade instead of siblings",
        [("Source/Dsp/Sludge.h", "rFilter.process (e, gR)", "rFilter.process (q, gR)")],
        ["Regression the snap is a transient term"],
    ),
    Fault(
        "level-match-fast",
        "the level match tracking fast enough to follow the reaction's own ring",
        # Both sides, because this one's assertion lives in the Python checks
        # and the prototype is what they run.
        [("Source/Dsp/OutputStage.h",
          "inline constexpr double kPeakTrackS = 4.0;",
          "inline constexpr double kPeakTrackS = 1.2;"),
         ("Source/Dsp/OutputStage.h",
          "inline constexpr double kPeakAttackS = 2.0;",
          "inline constexpr double kPeakAttackS = 0.6;"),
         ("prototype/output_stage.py", "PEAK_TRACK_S = 4.0", "PEAK_TRACK_S = 1.2"),
         ("prototype/output_stage.py", "PEAK_ATTACK_S = 2.0", "PEAK_ATTACK_S = 0.6")],
        ["level control does not pump"],
    ),
    Fault(
        "enrichment-unwired",
        "ENRICHMENT read from the parameter tree but never applied to the engines' input",
        [("Source/PluginProcessor.cpp",
          "const auto feedL = dryL * enrichment;",
          "const auto feedL = dryL;"),
         ("Source/PluginProcessor.cpp",
          "const auto feedR = dryR * enrichment;",
          "const auto feedR = dryR;")],
        ["Processor ENRICHMENT changes the RADIATION output"],
    ),
    Fault(
        "enrichment-on-dry",
        "ENRICHMENT leaking into the dry path as well as the engines",
        [("Source/PluginProcessor.cpp",
          "dryDelay.setSample (0, dryDelayPos, dryL);",
          "dryDelay.setSample (0, dryDelayPos, feedL);")],
        ["Processor ENRICHMENT leaves the dry path alone"],
    ),
    Fault(
        "meltdown-recalled",
        "a saved state reopening with Meltdown still held",
        [("Source/PluginProcessor.cpp",
          "apvts.getParameter (squelch::ids::meltdown)->setValueNotifyingHost (0.0f);",
          "")],
        ["Processor MELTDOWN comes back released from a saved state"],
    ),
    Fault(
        "meltdown-unwired",
        "MELTDOWN read from the parameter tree but never opening the gate",
        [("Source/PluginProcessor.cpp",
          "meltdown.setGate (apvts.getRawParameterValue (ids::meltdown)->load() > 0.5f);",
          "meltdown.setGate (false);")],
        ["Processor MELTDOWN changes the RADIATION output"],
    ),
    Fault(
        "meltdown-snaps",
        "the last stage arriving with the first, so a tap reaches everything",
        [("Source/Dsp/Meltdown.h",
          "{ 0.66, 0.75, 3.40, 1.00 },  // contamination",
          "{ 0.00, 0.75, 3.40, 1.00 },  // contamination")],
        ["Processor MELTDOWN stages in, and a tap stops short"],
    ),
    Fault(
        "meltdown-block-size",
        "MELTDOWN's clock restarting every block, so the stages depend on the host's buffer size",
        [("Source/Dsp/Meltdown.h",
          "heldSamples += samples;",
          "heldSamples = samples;")],
        ["Processor MELTDOWN does not depend on the block size"],
    ),
    Fault(
        "contamination-unwired",
        "CONTAMINATION read from the parameter tree but never reaching the noise bed",
        [("Source/PluginProcessor.cpp",
          "noiseBed.setAmount (staged (dsp::Staged::contamination, ids::contamination));",
          "noiseBed.setAmount (0.0);")],
        ["Processor CONTAMINATION grows with the control in RADIATION"],
    ),
    Fault(
        "contamination-ignores-meltdown",
        "the noise bed reading CONTAMINATION's knob, so MELTDOWN's last stage has nowhere to go",
        [("Source/PluginProcessor.cpp",
          "noiseBed.setAmount (staged (dsp::Staged::contamination, ids::contamination));",
          "noiseBed.setAmount (value (ids::contamination));")],
        ["Processor MELTDOWN bed follows the staged envelope"],
    ),
    Fault(
        "contamination-not-squared",
        "the bed's level linear in CONTAMINATION, so full travel is 6 dB above half and not 12",
        [("Source/Dsp/NoiseBed.h",
          "levels.fullLevel * amountNow * amountNow * (1.0 - 0.5 * damping)",
          "levels.fullLevel * amountNow * (1.0 - 0.5 * damping)")],
        ["Processor CONTAMINATION level of the FISSION bed"],
    ),
    Fault(
        "contamination-fixed-reference",
        "the bed referenced to a constant, so it ignores how loud the output is",
        [("Source/Dsp/NoiseBed.h",
          "* (wetLevel / levels.unitRms) * level;",
          "* (0.1 / levels.unitRms) * level;")],
        ["Processor CONTAMINATION follows the output's level"],
    ),
    Fault(
        "noise-radiation-band",
        "RADIATION's ticks filtered to the wrong band, which no behavioural check sees",
        [("Source/Dsp/NoiseBed.h",
          "radiationHpL.setCoefficients (highpass (2000.0, 0.8, sr));",
          "radiationHpL.setCoefficients (highpass (4000.0, 0.8, sr));")],
        ["noise_radiation"],
    ),
    Fault(
        "meltdown-button-latches",
        "the MELTDOWN button writing on at release as well, so a press never lets go",
        [("Source/PluginEditor.cpp",
          "parameter.setValueNotifyingHost (0.0f);",
          "parameter.setValueNotifyingHost (1.0f);")],
        ["Processor MELTDOWN button engages on press and lets go on release"],
    ),
    Fault(
        "meltdown-press-mid-schedule",
        "a press starting its clock part-way through the schedule, so the stages are late in arriving",
        [("Source/Dsp/Meltdown.h",
          "                heldSamples = 0;",
          "                heldSamples = 22050;")],
        ["Processor MELTDOWN held press reaches the stages in order"],
    ),
    Fault(
        "meltdown-never-returns",
        "a release that does not bring the stages back towards their knobs",
        [("Source/Dsp/Meltdown.h",
          "progress[i] *= std::exp (-static_cast<double> (samples) / (tau * sr));",
          "progress[i] *= 1.0;")],
        ["Processor MELTDOWN release returns every stage toward its knob"],
    ),
    Fault(
        "meltdown-never-settles",
        "stages that never snap back to their knobs, so a released MELTDOWN stays active for ever",
        [("Source/Dsp/Meltdown.h",
          "static constexpr double kSettledBelow = 1.0e-6;",
          "static constexpr double kSettledBelow = 0.0;")],
        ["Processor MELTDOWN leaves no state behind after release"],
    ),
    Fault(
        "meltdown-press-restarts",
        "a press that throws away the stages' current level, so a re-press mid-release jumps",
        [("Source/Dsp/Meltdown.h",
          "            gate = open;",
          "            if (open && ! gate)\n                progress.fill (0.0);\n\n            gate = open;")],
        ["Processor MELTDOWN pressed mid-release carries on from where it was"],
    ),
    Fault(
        "clip-disconnected",
        "the CLIP control read by nothing, so the output stays on the limiter whatever it says",
        [("Source/PluginProcessor.cpp",
          "clipOn = apvts.getRawParameterValue (ids::clip)->load() > 0.5f;",
          "clipOn = false;")],
        ["Processor CLIP on clips it hard at the ceiling",
         "Processor CLIP clips more as the signal rises over the ceiling"],
    ),
    Fault(
        "clip-misaligned",
        "the clipper's delay one sample off the limiter's, so switching CLIP moves the signal",
        [("Source/Dsp/OutputStage.h",
          "padding = std::max (total - kOversamplerLatencySamples, 1);",
          "padding = std::max (total - kOversamplerLatencySamples + 1, 1);")],
        ["Processor CLIP and the limiter are aligned and transparent below the ceiling"],
    ),
    Fault(
        "clip-wrong-ceiling",
        "the clipper's ceiling above the limiter's, so CLIP lets peaks through",
        [("Source/Dsp/OutputStage.h",
          "const auto clippedL = hardClip (oversamplerL.process (xL, curve), kLimiterCeiling);",
          "const auto clippedL = hardClip (oversamplerL.process (xL, curve), 1.0);")],
        ["Processor CLIP on clips it hard at the ceiling"],
    ),
    Fault(
        "transport-stopped-position",
        "a stopped host's frozen position read as a new one every block, as the first pass did",
        [("Source/PluginProcessor.cpp",
          "    if (! position->getIsPlaying())\n        return;\n",
          "")],
        ["Processor sequencer ignores a stopped host's position",
         "Processor sequencer ignores a stopped host wherever it is parked",
         "Processor RADIATION ticks sound under a stopped host"],
    ),
    Fault(
        "transport-samples-before-ppq",
        "the host's sample counter taken ahead of its PPQ, so tempo and seeks go by samples",
        [("Source/PluginProcessor.cpp",
          "const auto havePpq = ppq.hasValue() && *ppq >= 0.0;",
          "const auto havePpq = false;")],
        ["Processor sequencer follows the host's PPQ before its sample counter",
         "Processor sequencer converts the host's PPQ at the host's tempo"],
    ),
    Fault(
        "transport-tempo-ignored",
        "PPQ converted to samples at a fixed 120 bpm whatever the host's tempo",
        [("Source/PluginProcessor.cpp",
          "const auto samplesPerQuarter = 60.0 / hostBpm * preparedSampleRate;",
          "const auto samplesPerQuarter = 60.0 / 120.0 * preparedSampleRate;")],
        ["Processor sequencer converts the host's PPQ at the host's tempo"],
    ),
    Fault(
        "transport-loop-block-edge",
        "a loop wrapped only at the next block, so a block that crosses the end runs past it",
        [("Source/PluginProcessor.cpp",
          "if (timelinePosition < end && timelinePosition + numSamples > end)",
          "if (false && timelinePosition < end && timelinePosition + numSamples > end)")],
        ["Processor sequencer loop wraps at the sample, whatever the block size"],
    ),
    Fault(
        "transport-seek-ignored",
        "a seek that the sequencer does not follow, carrying on from its own clock",
        [("Source/PluginProcessor.cpp",
          "    if (havePpq)\n        timelinePosition = toSamples (*ppq);",
          "    if (havePpq)\n        timelinePosition += 0 * toSamples (*ppq);")],
        ["Processor sequencer seek changes what plays",
         "Processor sequencer start moves the sequencer to the host's position"],
    ),
    Fault(
        "randomise-unwired",
        "the Randomise button wired to nothing",
        [("Source/PluginEditor.cpp",
          "    squelch::presets::randomise (processor.apvts);\n", "")],
        ["Processor Randomise moves the parameters"],
    ),
    Fault(
        "randomise-touches-clip",
        "CLIP left out of the exclusion list, so Randomise flips an output setting",
        [("Source/Parameters.h",
          "return id == ids::clip || id == ids::meltdown",
          "return id == ids::meltdown")],
        ["Processor Randomise leaves the gestures and CLIP alone",
         "Processor the toolbar's next and previous buttons load presets and leave the gestures and CLIP alone"],
    ),
    Fault(
        "randomise-touches-ionize",
        "IONIZE left out of the exclusion list, so presets and Randomise perform a gesture",
        [("Source/Parameters.h",
          " || id == ids::ionize\n",
          "\n")],
        ["Processor Randomise leaves the gestures and CLIP alone",
         "Processor the toolbar's next and previous buttons load presets and leave the gestures and CLIP alone"],
    ),
    Fault(
        "clip-forgotten-on-reload",
        "CLIP forced off when a session loads, as MELTDOWN is",
        [("Source/PluginProcessor.cpp",
          "apvts.getParameter (squelch::ids::meltdown)->setValueNotifyingHost (0.0f);",
          "for (const auto* id : { squelch::ids::meltdown, squelch::ids::clip })\n        apvts.getParameter (id)->setValueNotifyingHost (0.0f);")],
        ["Processor CLIP survives a reload"],
    ),
    Fault(
        "ionize-forgotten-on-reload",
        "IONIZE forced off when a session loads, as MELTDOWN is",
        [("Source/PluginProcessor.cpp",
          "apvts.getParameter (squelch::ids::meltdown)->setValueNotifyingHost (0.0f);",
          "for (const auto* id : { squelch::ids::meltdown, squelch::ids::ionize })\n        apvts.getParameter (id)->setValueNotifyingHost (0.0f);")],
        ["Processor IONIZE survives a reload engaged"],
    ),
    Fault(
        "mono-right-leg-silent",
        "a mono source feeding only the left leg, the right reading an empty channel",
        [("Source/PluginProcessor.cpp",
          "const auto dryR = inputChannels > 1 ? buffer.getSample (1, sample) * in : dryL;",
          "const auto dryR = buffer.getSample (1, sample) * in;")],
        ["Processor mono in, stereo out is the stereo path fed the same signal twice"],
    ),
    Fault(
        "mono-layout-rejected",
        "mono input refused, as it was before",
        [("Source/PluginProcessor.cpp",
          "|| (isMono (in) && (isMono (out) || isStereo (out)));",
          "|| false;")],
        ["Processor accepts mono in, mono or stereo out, and stereo in and out"],
    ),
    Fault(
        "preset-values-ignored",
        "every preset loading the defaults whatever its row says",
        [("Source/Presets.h",
          "return rows[static_cast<size_t> (preset - 1)][i];",
          "return parameter.getDefaultValue();")],
        ["Processor every preset loads and leaves the performance controls alone"],
    ),
    Fault(
        "randomise-touches-ionize-amount",
        "IONIZEAMOUNT left out of the exclusion list, so presets and Randomise set it",
        [("Source/Parameters.h",
          "            || id == ids::ionizeAmount;",
          "            ;")],
        ["Processor Randomise leaves the gestures and CLIP alone",
         "Processor every preset loads and leaves the performance controls alone"],
    ),
    Fault(
        "default-drifts-to-prototype",
        "REACTIVITY's default taken from prototype/params.py instead of the shipping value",
        [("Source/Parameters.h",
          '{ ids::reactivity, "Reactivity", 0.3f,',
          '{ ids::reactivity, "Reactivity", 0.0f,')],
        ["Processor the parameter layout holds the shipping defaults",
         "Processor the editor opens on the shipping defaults"],
    ),
    Fault(
        "preset-select-unwired",
        "picking a preset that selects it without loading its values",
        [("Source/PluginEditor.cpp",
          "    squelch::presets::apply (processor.apvts, index);\n", "")],
        ["Processor the toolbar's next and previous buttons load presets and leave the gestures and CLIP alone"],
    ),
    Fault(
        "fresh-editor-clobbers-session",
        "a restored session taken for a fresh instance, so opening the editor loads Default over it",
        [("Source/PluginProcessor.cpp",
          "presetIndex = savedPreset == squelch::presets::notChosen ? 0 : savedPreset;",
          "presetIndex = savedPreset;")],
        ["Processor opening the editor on a restored session keeps its values"],
    ),
]


def git_dirty() -> bool:
    out = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"],
                         cwd=ROOT, capture_output=True, text=True).stdout
    return bool(out.strip())


def apply(fault: Fault) -> bool:
    for rel, before, after in fault.edits:
        path = ROOT / rel
        text = path.read_text()
        if before not in text:
            print(f"    could not find in {rel}: {before[:60]}")
            return False
        path.write_text(text.replace(before, after, 1))
    return True


def restore() -> None:
    subprocess.run(["git", "checkout", "--", "Source/", "prototype/"], cwd=ROOT, check=True)


def restore_and_verify() -> bool:
    """Put the sources back, rebuild from them, and prove the baseline is green.

    Run unconditionally on the way out, including after an exception, because
    the alternative is leaving faulty binaries under a clean working tree.
    """
    restore()
    print("Sources restored.")

    failures = run_suite()
    if failures is None:
        print("Clean rebuild FAILED.")
        return False
    print("Clean rebuild completed.")

    if failures:
        print("Baseline validation: FAIL")
        for line in failures:
            print("   ", line[:110])
        return False

    print("Baseline validation: PASS")
    return True


def run_suite() -> list[str] | None:
    """Every failing line from both suites, or None if it will not build.

    Both, because the faults do not all surface in one place: the level
    match's is a behavioural check in Python and the resonator's is an
    assertion in the C++ validator. A harness that only watched one of them
    would report a working test as missing.
    """
    build = subprocess.run(["cmake", "--build", "build", "--target",
                            "SquelchValidate", "SquelchHarness", "SquelchProcessorTest"],
                           cwd=ROOT, capture_output=True, text=True)
    if "error:" in build.stdout + build.stderr:
        return None

    failures = []

    out = subprocess.run([str(ROOT / "build/SquelchValidate")], cwd=ROOT,
                         capture_output=True, text=True).stdout
    failures += [line for line in out.splitlines() if line.startswith("[FAIL]")]

    ran = subprocess.run([str(ROOT / "build/SquelchProcessorTest_artefacts/Release/SquelchProcessorTest")],
                         cwd=ROOT, capture_output=True, text=True)
    failures += [line for line in ran.stdout.splitlines() if line.startswith("[FAIL]")]

    # A test run that dies is a failure in its own right, not a run with nothing to report.
    if ran.returncode < 0:
        failures.append(f"[FAIL] SquelchProcessorTest crashed (signal {-ran.returncode})")

    out = subprocess.run(["./.venv/bin/python", "-W", "ignore", "-m", "prototype.checks"],
                         cwd=ROOT, capture_output=True, text=True).stdout
    failures += [line for line in out.splitlines() if line.startswith("[FAIL]")]

    # The numerical comparison too: a fault in a port that the behavioural checks
    # cannot see, such as a noise bed's filter, is one it exists to catch.
    harness = subprocess.run([str(ROOT / "build/SquelchHarness")], cwd=ROOT,
                             capture_output=True, text=True).stdout
    out = subprocess.run(["./.venv/bin/python", "-W", "ignore", "-m", "prototype.compare"],
                         cwd=ROOT, input=harness, capture_output=True, text=True).stdout
    failures += [line for line in out.splitlines() if line.startswith(("[FAIL]", "[MISS]"))]

    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="run only these faults, by key (comma-separated)")
    args = parser.parse_args()

    if git_dirty():
        print("Working tree is dirty. Fault injection edits Source/ and prototype/ and restores\n"
              "git checkout, which would discard your changes. Commit or stash first.")
        return 1

    wanted = None if args.only is None else args.only.split(",")
    faults = [f for f in FAULTS if wanted is None or f.key in wanted]
    if not faults:
        print(f"No fault matching {args.only!r}. Known: " + ", ".join(f.key for f in FAULTS))
        return 1

    print("Fault injection\n")
    baseline = run_suite()
    if baseline is None:
        print("  baseline does not build")
        return 1
    if baseline:
        print("  baseline is not green; fix that first:")
        for line in baseline:
            print("   ", line[:110])
        return 1
    print("  baseline green\n")

    undetected = []
    clean = False
    try:
        for fault in faults:
            print(f"{fault.key}: {fault.what}")
            if not apply(fault):
                restore()
                undetected.append(fault.key)
                continue

            failures = run_suite()
            restore()

            if failures is None:
                print("    did not build (the fault may no longer apply cleanly)")
                undetected.append(fault.key)
                continue

            caught = [line for line in failures
                      if any(name in line for name in fault.caught_by)]
            if caught:
                for line in caught:
                    print("    caught by:", line[:100])
            else:
                print("    *** NOT CAUGHT by " + " or ".join(fault.caught_by) + " ***")
                if failures:
                    print("    (something else failed: "
                          + "; ".join(line[7:50].strip() for line in failures[:3]) + ")")
                undetected.append(fault.key)
            print()

        print()
        caught_count = len(faults) - len(undetected)
        print(f"Fault injection: {caught_count}/{len(faults)} caught.")
        if undetected:
            print("  not caught: " + ", ".join(undetected))
        print()
    finally:
        clean = restore_and_verify()

    return 0 if clean and not undetected else 1


if __name__ == "__main__":
    sys.exit(main())
