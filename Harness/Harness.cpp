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
#include "../Source/Dsp/Oversampler.h"
#include "../Source/Dsp/Rng.h"
#include "../Source/Dsp/Saturation.h"
#include "../Source/Dsp/Sludge.h"

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
        printArray ("running_rms", levels);
    }

    // The oversampler is causal (ordinary delay-line state), where the Python
    // reference is acausal (scipy's zero-phase resample_poly). They produce
    // the same values, just kOversamplerLatencySamples apart -- see
    // Oversampler.h's docstring. Skip latency, then a further settling
    // region: the FIR's zero initial state takes longer than the bare
    // latency to settle for low-frequency content, so comparing right at
    // latency alone catches it still ringing from a cold start.
    {
        constexpr int settle = 320;
        dsp::Oversampler oversampler;
        std::vector<double> values;
        for (int i = 0; i < 2500; ++i)
        {
            const auto t = i / sampleRate;
            const auto x = 0.6 * std::sin (2.0 * M_PI * 300.0 * t)
                         + 0.5 * std::sin (2.0 * M_PI * 5000.0 * t);
            const auto y = oversampler.process (x, [] (double v) { return dsp::softClip (v); });
            if (i >= settle && (i - settle) % 20 == 0)
                values.push_back (y);
        }
        printArray ("oversampler_soft_clip", values);
    }

    // SLUDGE's own one-pole stages all start at zero state with no
    // lookahead, same as the oversampler's envelope followers, so they need
    // no latency of their own. But the engine runs its saturation through
    // Oversampler.h internally, same acausal-vs-causal story as that
    // primitive on its own: skip its latency, then a settling region.
    {
        constexpr int settle = 400;
        dsp::SludgeProfile profile;
        profile.baseHz = 100.0;
        profile.cutoffLoHz = 150.0;
        profile.cutoffHiHz = 1300.0;
        profile.decayLoS = 0.25;
        profile.decayHiS = 1.60;

        dsp::SludgeParams params;
        params.decay = 0.4;
        params.halfLife = 0.6;
        params.spread = 0.65;
        params.reactivity = 0.5;
        params.exposure = 0.7;
        params.toxicity = 0.35;

        dsp::SludgeEngine engine;
        engine.prepare (sampleRate);
        engine.configure (profile, params);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            const auto t = i / sampleRate;
            const auto x = 0.5 * std::sin (2.0 * M_PI * 110.0 * t)
                         + 0.3 * std::sin (2.0 * M_PI * 850.0 * t);

            // Matches prototype/compare.py's np.roll(x, 3): a circular shift,
            // not a zero-padded delay, so the first three samples wrap round
            // to the end of the 4000-sample test signal rather than reading
            // zero.
            const auto idx = (i - 3 + 4000) % 4000;
            const auto tPrev = idx / sampleRate;
            const auto xPrev = 0.5 * std::sin (2.0 * M_PI * 110.0 * tPrev)
                             + 0.3 * std::sin (2.0 * M_PI * 850.0 * tPrev);

            const auto xL = x;
            const auto xR = 0.8 * xPrev;

            double outL = 0.0, outR = 0.0;
            engine.process (xL, xR, outL, outR);

            if (i >= settle && (i - settle) % 40 == 0)
            {
                values.push_back (outL);
                values.push_back (outR);
            }
        }
        printArray ("sludge_engine", values, true);
    }

    std::printf ("}\n");
    return 0;
}
