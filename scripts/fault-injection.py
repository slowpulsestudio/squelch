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


def run_suite() -> list[str] | None:
    """Every failing line from both suites, or None if it will not build.

    Both, because the faults do not all surface in one place: the level
    match's is a behavioural check in Python and the resonator's is an
    assertion in the C++ validator. A harness that only watched one of them
    would report a working test as missing.
    """
    build = subprocess.run(["cmake", "--build", "build", "--target",
                            "SquelchValidate", "SquelchHarness"],
                           cwd=ROOT, capture_output=True, text=True)
    if "error:" in build.stdout + build.stderr:
        return None

    failures = []

    out = subprocess.run([str(ROOT / "build/SquelchValidate")], cwd=ROOT,
                         capture_output=True, text=True).stdout
    failures += [line for line in out.splitlines() if line.startswith("[FAIL]")]

    out = subprocess.run(["./.venv/bin/python", "-W", "ignore", "-m", "prototype.checks"],
                         cwd=ROOT, capture_output=True, text=True).stdout
    failures += [line for line in out.splitlines() if line.startswith("[FAIL]")]

    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="run one fault by key")
    args = parser.parse_args()

    if git_dirty():
        print("Working tree is dirty. Fault injection edits Source/ and prototype/ and restores\n"
              "git checkout, which would discard your changes. Commit or stash first.")
        return 1

    faults = [f for f in FAULTS if args.only in (None, f.key)]
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
    if undetected:
        print(f"{len(undetected)} of {len(faults)} not caught: " + ", ".join(undetected))
        return 1

    print(f"All {len(faults)} faults caught by the assertion written for them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
