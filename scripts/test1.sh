#!/usr/bin/env bash
#
# dsp-testing.md's Test 1 — build and plugin validity.
#
# Everything here runs before the DSP measurements, because a plugin that
# cannot be instantiated, cannot survive a sample-rate change, or loses its
# state on a round trip will produce numbers that are wrong for reasons that
# have nothing to do with the maths. A failure here blocks Tests 2 onward.
#
# pluginval does the host-compatibility half: instantiation at every supported
# sample rate, every block size, every parameter swept through its legal
# range, reset, and state save/restore. Strictness level 10 is its highest and
# includes the background-thread and non-realtime-safe checks.
#
# Usage: scripts/test1.sh [--skip-debug]

set -euo pipefail

cd "$(dirname "$0")/.."

PLUGINVAL="/Applications/pluginval.app/Contents/MacOS/pluginval"
PLUGIN="build/Squelch_artefacts/Release/VST3/Squelch.vst3"

skip_debug=0
for arg in "$@"; do
    [[ "$arg" == "--skip-debug" ]] && skip_debug=1
done

echo "Test 1 — build and plugin validity"
echo

# Debug and Release both, because an assertion that only fires in Debug is
# still a fault and Release is what ships.
if [[ $skip_debug -eq 0 ]]; then
    echo "  Debug build..."
    cmake --build build --config Debug --target Squelch_VST3 >/tmp/squelch-debug.log 2>&1 \
        || { echo "  FAIL: Debug build"; tail -20 /tmp/squelch-debug.log; exit 1; }
    echo "  PASS: Debug build"
fi

echo "  Release build..."
cmake --build build --config Release --target Squelch_VST3 >/tmp/squelch-release.log 2>&1 \
    || { echo "  FAIL: Release build"; tail -20 /tmp/squelch-release.log; exit 1; }
echo "  PASS: Release build"

if [[ ! -d "$PLUGIN" ]]; then
    echo "  FAIL: no bundle at $PLUGIN"
    exit 1
fi

if [[ ! -x "$PLUGINVAL" ]]; then
    echo "  SKIP: pluginval not installed at $PLUGINVAL"
    echo
    echo "  Install it from https://github.com/Tracktion/pluginval/releases"
    echo "  and rerun. Test 1 is not complete without it."
    exit 0
fi

echo "  pluginval, strictness 10, every supported rate and block size..."
if "$PLUGINVAL" \
        --strictness-level 10 \
        --validate-in-process \
        --timeout-ms 600000 \
        --sample-rates 44100,48000,88200,96000,192000 \
        --block-sizes 32,64,128,256,512,1024,2048 \
        --validate "$PLUGIN" >/tmp/squelch-pluginval.log 2>&1; then
    echo "  PASS: pluginval"
else
    echo "  FAIL: pluginval"
    grep -E "FAIL|!!!|Error" /tmp/squelch-pluginval.log | head -40
    echo
    echo "  Full log: /tmp/squelch-pluginval.log"
    exit 1
fi

echo
echo "TEST 1: PASS"
