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

#include "../Source/Dsp/Alien.h"
#include "../Source/Dsp/Chemical.h"
#include "../Source/Dsp/Filters.h"
#include "../Source/Dsp/Fission.h"
#include "../Source/Dsp/Radiation.h"
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

    std::vector<double> sludgeRun (const squelch::dsp::SludgeProfile& profile,
                                   const squelch::dsp::SludgeParams& params)
    {
        constexpr int n = 4000;
        constexpr int settle = 1000;

        squelch::dsp::SludgeEngine engine;
        engine.prepare (sampleRate);
        engine.configure (profile, params);

        std::vector<double> values;

        for (int i = 0; i < n; ++i)
        {
            const auto t = i / sampleRate;
            const auto x = 0.5 * std::sin (2.0 * M_PI * 110.0 * t)
                         + 0.3 * std::sin (2.0 * M_PI * 850.0 * t);

            // Matches prototype/compare.py's np.roll(x, 3): a circular shift,
            // not a zero-padded delay, so the first three samples wrap round
            // to the end of the test signal rather than reading zero.
            const auto tPrev = ((i - 3 + n) % n) / sampleRate;
            const auto xPrev = 0.5 * std::sin (2.0 * M_PI * 110.0 * tPrev)
                             + 0.3 * std::sin (2.0 * M_PI * 850.0 * tPrev);

            double outL = 0.0, outR = 0.0;
            engine.process (x, 0.8 * xPrev, outL, outR);

            if (i >= settle && (i - settle) % 40 == 0)
            {
                values.push_back (outL);
                values.push_back (outR);
            }
        }

        return values;
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
    // Oversampler.h's docstring. Skip the latency, then the cold-start
    // region, which is exactly the two 81-tap filters' combined memory at
    // 4x: (81 + 81) / 4 = 41 base-rate samples, and not a sample more.
    {
        constexpr int settle = 41;
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
    // lookahead, so they need no latency of their own. The settling window
    // is far longer than the oversampler's 41 for a different reason: that
    // cold-start region is finite, but it feeds the final smoothing
    // one-pole, which is IIR and smears it into an exponentially decaying
    // tail (1/g_c, about 16 samples here, machine precision by ~300).
    // 1000 is margin -- g_c falls with f_effective, so the figure is
    // parameter-dependent, not universal.
    //
    // Run at two operating points: almost every constant in configure() is a
    // lo/hi interpolation, and one point cannot tell a correct one from an
    // inverted one.
    {
        dsp::SludgeProfile profile;
        profile.baseHz = 100.0;
        profile.cutoffLoHz = 150.0;
        profile.cutoffHiHz = 1300.0;
        profile.decayLoS = 0.25;
        profile.decayHiS = 1.60;

        dsp::SludgeParams a;
        a.decay = 0.4;
        a.halfLife = 0.6;
        a.spread = 0.65;
        a.reactivity = 0.5;
        a.exposure = 0.7;
        a.toxicity = 0.35;

        dsp::SludgeParams b;
        b.decay = 0.85;
        b.halfLife = 0.15;
        b.spread = 0.3;
        b.reactivity = 0.9;
        b.exposure = 0.2;
        b.toxicity = 0.75;

        printArray ("sludge_engine", sludgeRun (profile, a));
        printArray ("sludge_engine_b", sludgeRun (profile, b));
    }

    // One ALIEN voice. No oversampler and no acausal stage anywhere in it, so
    // there is no latency and no settling window to skip.
    {
        dsp::AlienProfile profile;
        dsp::AlienParams params;
        params.spread = 0.6;
        params.decay = 0.35;
        params.toxicity = 0.7;
        params.exposure = 0.4;
        params.seed = 5;

        dsp::AlienEngine engine;
        engine.prepare (sampleRate);
        engine.configure (profile, params);
        engine.trigger (3, false, 0.0, 0.0);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            double l = 0.0, r = 0.0;
            engine.process (l, r);
            if (i % 20 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("alien_engine", values);
    }

    // CHEMICAL: two events, so the register's zero-order hold is exercised
    // and not just the ladder underneath it.
    {
        dsp::ChemicalEngine engine;
        engine.prepare (sampleRate);
        engine.setSeed (9);
        engine.setEvent (2);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            if (i == 2000)
                engine.setEvent (7);

            const auto t = i / sampleRate;
            const auto x = 0.3 * std::sin (2.0 * M_PI * 150.0 * t);
            const auto y = engine.process (x, 700.0, 2.2, 1.4);

            if (i % 20 == 0)
            {
                values.push_back (y);
                values.push_back (y);
            }
        }
        printArray ("chemical_engine", values);
    }

    // RADIATION: one event, so the percussive pulse rides alongside the
    // resonator and the per-sample hashed AR(1) states.
    {
        dsp::RadiationProfile profile;
        dsp::RadiationParams params;
        params.volatility = 0.7;
        params.spread = 0.6;
        params.decay = 0.45;
        params.exposure = 0.3;
        params.seed = 4;

        dsp::RadiationEngine engine;
        engine.prepare (sampleRate);
        engine.configure (profile, params);
        engine.trigger (false);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            const auto t = i / sampleRate;
            double l = 0.0, r = 0.0;
            engine.process (0.3 * std::sin (2.0 * M_PI * 180.0 * t),
                            0.25 * std::sin (2.0 * M_PI * 240.0 * t), l, r);
            if (i % 20 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("radiation_engine", values);
    }

    // FISSION. The prototype clamps its fractional-delay read index at 0, so
    // it reads v2[0] where a real delay line reads zeros; that disagreement
    // is bounded by the longest delay the parameters can ask for,
    // (D0 + Dm) * sr = 265 samples at SPREAD 1. 300 is that rounded up.
    {
        constexpr int settle = 300;
        dsp::FissionProfile profile;
        dsp::FissionParams params;
        params.spread = 0.8;
        params.decay = 0.5;
        params.exposure = 0.6;
        params.seed = 2;

        dsp::FissionEngine engine;
        engine.prepare (sampleRate);
        engine.configure (profile, params);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            const auto t = i / sampleRate;
            double l = 0.0, r = 0.0;
            engine.process (0.3 * std::sin (2.0 * M_PI * 320.0 * t),
                            0.3 * std::sin (2.0 * M_PI * 300.0 * t), l, r);
            if (i >= settle && (i - settle) % 20 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("fission_engine", values, true);
    }

    std::printf ("}\n");
    return 0;
}
