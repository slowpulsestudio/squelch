/** Numerical comparison harness.

    Runs the shipping DSP primitives and prints the results as JSON for
    prototype/compare.py to check against the Python. The point is that "it
    compiles" and "it sounds about right" are not evidence: a filter with a
    transposed coefficient still compiles and still makes a noise.

    Kept in the repo as its own target. It does not get deleted once it passes.
*/

#include <cstdio>
#include <string>
#include <vector>

#include "../Source/Dsp/Filters.h"
#include "../Source/Dsp/Rng.h"

namespace
{
    constexpr double sampleRate = 44100.0;

    void printArray (const char* name, const std::vector<double>& values, bool last = false)
    {
        std::printf ("  \"%s\": [", name);

        for (size_t i = 0; i < values.size(); ++i)
            std::printf ("%s%.17g", i == 0 ? "" : ", ", values[i]);

        std::printf ("]%s\n", last ? "" : ",");
    }

    /// An impulse response pins every coefficient at once: any transposed or
    /// mistyped term shows up immediately, where a sine sweep might not.
    std::vector<double> impulseResponse (const squelch::dsp::BiquadCoefficients& c, int length)
    {
        squelch::dsp::Biquad filter;
        filter.setCoefficients (c);

        std::vector<double> out;
        out.reserve (static_cast<size_t> (length));

        for (int i = 0; i < length; ++i)
            out.push_back (filter.process (i == 0 ? 1.0 : 0.0));

        return out;
    }
}

int main()
{
    using namespace squelch;

    std::printf ("{\n");

    // Hashing has to agree exactly. One bit out and the C++ schedules a
    // different piece of music from the same seed.
    std::vector<double> randoms;
    for (std::uint64_t i = 0; i < 16; ++i)
        randoms.push_back (rng::urand ({ 7, 3, i }));
    printArray ("urand", randoms);

    std::vector<double> hashes;
    for (std::uint64_t i = 0; i < 8; ++i)
        hashes.push_back (static_cast<double> (rng::uhash ({ i }) >> 11));
    printArray ("uhash", hashes);

    printArray ("lowpass", impulseResponse (dsp::lowpass (500.0, 2.0, sampleRate), 32));
    printArray ("highpass", impulseResponse (dsp::highpass (500.0, 2.0, sampleRate), 32));
    printArray ("notch", impulseResponse (dsp::notch (500.0, 2.0, sampleRate), 32));
    printArray ("peaking", impulseResponse (dsp::peaking (1000.0, -6.0, 0.9, sampleRate), 32));
    printArray ("low_shelf", impulseResponse (dsp::lowShelf (70.0, 2.5, sampleRate), 32));
    printArray ("high_shelf", impulseResponse (dsp::highShelf (15000.0, 1.0, sampleRate), 32));

    // The level tracker decides the output gain, so a mismatch here is a
    // mismatch in loudness everywhere.
    {
        dsp::RunningRms tracker;
        tracker.prepare (sampleRate, 1.5);

        std::vector<double> levels;
        for (int i = 0; i < 2000; ++i)
        {
            const auto value = tracker.process (std::sin (2.0 * M_PI * 220.0 * i / sampleRate));
            if (i % 100 == 0)
                levels.push_back (value);
        }
        printArray ("running_rms", levels, true);
    }

    std::printf ("}\n");
    return 0;
}
