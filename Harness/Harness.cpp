/** Numerical comparison harness.

    Runs the shipping DSP primitives and prints the results as JSON for
    prototype/compare.py to check against the Python. The point is that "it
    compiles" and "it sounds about right" are not evidence: a filter with a
    transposed coefficient still compiles and still makes a noise.

    Kept in the repo as its own target. It does not get deleted once it passes.
*/

#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

#include "../Source/Dsp/Alien.h"
#include "../Source/Dsp/Chemical.h"
#include "../Source/Dsp/Envelopes.h"
#include "../Source/Dsp/Filters.h"
#include "../Source/Dsp/Fission.h"
#include "../Source/Dsp/Meltdown.h"
#include "../Source/Dsp/NoiseBed.h"
#include "../Source/Dsp/Radiation.h"
#include "../Source/Dsp/Reverb.h"
#include "../Source/Dsp/Oversampler.h"
#include "../Source/Dsp/Placement.h"
#include "../Source/Dsp/OutputStage.h"
#include "../Source/Dsp/Rng.h"
#include "../Source/Dsp/Saturation.h"
#include "../Source/Dsp/Scheduler.h"
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
        printArray ("fission_engine", values);
    }

    // The house voicing is four fixed biquads, so an impulse pins all of them.
    {
        dsp::Voice voice;
        voice.prepare (sampleRate);

        std::vector<double> values;
        for (int i = 0; i < 64; ++i)
            values.push_back (voice.process (i == 0 ? 1.0 : 0.0));
        printArray ("voice", values);
    }

    // The prototype's gain envelope leads the sample it scales by 2w while
    // delaying the audio by only w, so the causal port delays by 2w and sits
    // one window behind it. Settling is 3w: the prototype uses mode='nearest'
    // for its first w samples, and the ring takes 2w to fill.
    {
        const auto w = dsp::lookaheadSamples (sampleRate);
        const auto settle = 3 * w;
        dsp::PeakLimiter limiter;
        limiter.prepare (sampleRate);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            const auto t = i / sampleRate;
            auto tone = 0.3 * std::sin (2.0 * M_PI * 220.0 * t);
            if (i >= 1500 && i < 1510)
                tone += 3.0;

            double l = 0.0, r = 0.0;
            limiter.process (tone, 0.8 * tone, l, r);
            if (i >= settle && (i - settle) % 20 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("peak_limiter", values);
    }

    // Collimator: two static filters, so an impulse pins both.
    {
        dsp::Collimator collimator;
        collimator.prepare (sampleRate);
        collimator.set (0.7);

        std::vector<double> values;
        for (int i = 0; i < 64; ++i)
            values.push_back (collimator.process (i == 0 ? 1.0 : 0.0));
        printArray ("collimate", values);
    }

    // UnityMatch: follower and gain smoother both start from a defined state,
    // so there is nothing to settle and sample 0 must agree. The level step
    // partway through exercises the release side as well as the attack.
    {
        dsp::UnityMatch match;
        match.prepare (sampleRate);

        std::vector<double> values;
        for (int i = 0; i < 8000; ++i)
        {
            const auto t = i / sampleRate;
            auto tone = 0.5 * std::sin (2.0 * M_PI * 200.0 * t);
            if (i >= 4000)
                tone *= 0.2;

            double l = 0.0, r = 0.0;
            match.process (tone, 0.7 * tone, l, r);
            if (i % 40 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("unity_match", values);
    }

    // Drive runs its curve through the oversampler, so the same latency and
    // cold-start region apply as to the oversampler on its own.
    {
        constexpr int settle = 41;
        dsp::Drive drive;
        drive.prepare (sampleRate);
        drive.set (0.6, 1.3);

        std::vector<double> values;
        for (int i = 0; i < 6000; ++i)
        {
            const auto t = i / sampleRate;
            const auto tone = 0.4 * std::sin (2.0 * M_PI * 180.0 * t);
            double l = 0.0, r = 0.0;
            drive.process (tone, 0.7 * tone, l, r);
            if (i >= settle && (i - settle) % 40 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("drive", values);
    }

    // Stereo spread: the prototype zero-fills ahead of its right-channel
    // shift, which a zero-initialised line reproduces, so nothing settles.
    {
        dsp::StereoSpread spread;
        spread.prepare (sampleRate);
        spread.set (0.65);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            const auto t = i / sampleRate;
            const auto l = 0.4 * std::sin (2.0 * M_PI * 90.0 * t)
                         + 0.3 * std::sin (2.0 * M_PI * 900.0 * t);
            const auto r = 0.35 * std::sin (2.0 * M_PI * 110.0 * t)
                         + 0.25 * std::sin (2.0 * M_PI * 1300.0 * t);

            double ol = 0.0, orr = 0.0;
            spread.process (l, r, ol, orr);
            if (i % 20 == 0)
            {
                values.push_back (ol);
                values.push_back (orr);
            }
        }
        printArray ("stereo_spread", values);
    }

    // Pitch wind. The prototype clips its read index at 0, reading x[0] where
    // a delay line reads zeros; bounded by the longest delay available,
    // 0.0035 * 44100 = 155 samples, rounded to 200.
    {
        constexpr int settle = 200;
        dsp::PitchWind wind;
        wind.prepare (sampleRate);

        std::vector<double> values;
        for (int i = 0; i < 4000; ++i)
        {
            const auto t = i / sampleRate;
            const auto control = 0.5 + 0.5 * std::sin (2.0 * M_PI * 6.0 * t);
            const auto y = wind.process (0.4 * std::sin (2.0 * M_PI * 440.0 * t), control);
            if (i >= settle && (i - settle) % 20 == 0)
                values.push_back (y);
        }
        printArray ("pitch_wind", values);
    }

    // The scheduler, walked in blocks rather than over the whole render, so
    // the block-wise path is what gets compared and not a one-shot render.
    {
        dsp::ScheduleSettings settings;
        settings.gridIndex = 5;          // "1/8"
        settings.bpm = 140.0;
        settings.flux = 0.4;
        settings.probability = 0.8;
        settings.reactivity = 0.5;
        settings.volatility = 0.6;
        settings.containment = 0.2;
        settings.subEventBias = 1.3;
        settings.seed = 12;

        dsp::Scheduler scheduler;
        scheduler.prepare (sampleRate);
        scheduler.configure (settings);

        std::vector<dsp::ScheduledEvent> events;
        constexpr int block = 512;
        const auto total = static_cast<std::int64_t> (sampleRate) * 4;
        for (std::int64_t at = 0; at < total; at += block)
            scheduler.forRange (at, static_cast<int> (std::min<std::int64_t> (block, total - at)),
                                [&events] (const dsp::ScheduledEvent& e) { events.push_back (e); });

        std::sort (events.begin(), events.end(),
                   [] (const auto& a, const auto& b) { return a.start < b.start; });

        std::vector<double> values;
        for (const auto& e : events)
        {
            values.push_back (double (e.start));
            values.push_back (double (e.index));
            values.push_back (e.pan);
            values.push_back (e.decayScale);
            values.push_back (e.accent ? 1.0 : 0.0);
            values.push_back (e.slide ? 1.0 : 0.0);
        }
        printArray ("scheduler", values);
    }

    // Envelopes: three events including a slid one, so the portamento path is
    // exercised and not only the exponential fall.
    {
        dsp::Envelopes envelopes;
        envelopes.prepare (sampleRate);

        dsp::EnvelopeParams params;
        params.spread = 0.7;
        params.decay = 0.4;
        params.exposure = 0.6;
        params.toxicity = 0.5;
        params.containment = 0.2;
        params.halfLife = 0.5;
        params.seed = 3;
        envelopes.configure ({}, params);

        struct Fire { int at; std::uint64_t index; bool accent, slide; std::int64_t toNext; };
        const Fire fires[] { { 0, 0, false, false, 2000 },
                             { 2000, 5, true, false, 2000 },
                             { 4000, 10, false, true, 2000 } };

        std::vector<double> values;
        for (int i = 0; i < 6000; ++i)
        {
            for (const auto& f : fires)
            {
                if (f.at != i)
                    continue;

                dsp::ScheduledEvent e;
                e.start = f.at;
                e.index = f.index;
                e.intensity = 1.0;
                e.decayScale = 1.0;
                e.accent = f.accent;
                e.slide = f.slide;
                envelopes.trigger (e, f.toNext);
            }

            const auto v = envelopes.process();
            if (i % 25 == 0)
            {
                values.push_back (v.cutoffHz);
                values.push_back (v.feedback);
                values.push_back (v.drive);
            }
        }
        printArray ("envelopes", values);
    }

    // Placement: three overlapping events, because the rule is loudest-wins
    // rather than an average and one event would not exercise it.
    {
        dsp::Placement placement;
        placement.prepare (sampleRate);
        placement.configure (0.6, 0.0, 0.025, 0.3, 0.4, 1.0, 0.4);

        struct Fire { int at; std::uint64_t index; double pan, shape, depth; };
        const Fire fires[] { { 0, 0, -0.8, 0.3, 0.2 },
                            { 3000, 1, 0.7, 0.6, 0.8 },
                            { 9000, 2, 0.1, 0.5, 0.5 } };

        std::vector<double> values;
        for (int i = 0; i < 88200; ++i)
        {
            for (const auto& f : fires)
            {
                if (f.at != i)
                    continue;

                dsp::ScheduledEvent e;
                e.start = f.at;
                e.index = f.index;
                e.pan = f.pan;
                e.shape = f.shape;
                e.depth = f.depth;
                e.decayScale = 1.0;
                placement.trigger (e);
            }

            const auto v = placement.process();
            // Every 40 CONTROL steps, which is 40 * kControlBlock samples.
            if (i % (40 * dsp::kControlBlock) == 0)
            {
                values.push_back (v.env);
                values.push_back (v.panL);
                values.push_back (v.panR);
                values.push_back (v.send);
            }
        }
        printArray ("placement", values);
    }

    // MELTDOWN: the gate opens at 0.5 s and shuts at 1.5 s, which is a sample
    // boundary at this rate, so the prototype's continuous-time curves and the
    // streaming state are being asked the same question. One row of eight
    // staged values every 400 samples, stepped a sample at a time.
    {
        const std::array<double, 8> knobs { 0.2, 0.4, 0.3, 0.5, 0.2, 0.45, 0.6, 0.1 };

        dsp::Meltdown meltdown;
        meltdown.prepare (sampleRate);

        std::vector<double> values;
        for (int i = 0; i < 176400; ++i)
        {
            meltdown.setGate (i >= 22050 && i < 66150);

            if (i % 400 == 0)
                for (size_t s = 0; s < knobs.size(); ++s)
                    values.push_back (meltdown.value (static_cast<dsp::Staged> (s), knobs[s]));

            meltdown.advance (1);
        }
        printArray ("meltdown", values);
    }

    // CLIP: a hard ceiling at four times the rate, then a final clamp. The input
    // overshoots the 0.97 ceiling by a wide margin on both channels.
    {
        dsp::HardClip clipper;
        clipper.prepare (sampleRate);

        constexpr int n = 8000;
        constexpr int first = 700;

        std::vector<double> values;
        for (int i = 0; i < n; ++i)
        {
            const auto t = i / sampleRate;
            const auto l = 1.4 * std::sin (2.0 * M_PI * 170.0 * t);
            const auto r = 0.9 * std::sin (2.0 * M_PI * 311.0 * t + 0.2) + 0.5 * std::sin (2.0 * M_PI * 2900.0 * t);

            double outL = 0.0, outR = 0.0;
            clipper.process (l, r, outL, outR);

            if (i >= first && (i - first) % 17 == 0)
            {
                values.push_back (outL);
                values.push_back (outR);
            }
        }
        printArray ("hard_clip", values);
    }

    // CONTAMINATION's four exact beds, wet plus bed less the wet, so it is the bed
    // alone that is compared. The dry is silent for its first 4410 samples, which
    // is what the prototype's rolling peak needs: it reflects at the start of the
    // file, where a stream has nothing to reflect.
    {
        constexpr int n = 88200;
        const char* names[] { "noise_radiation", "noise_fission", "noise_sludge", "", "noise_alien" };

        for (const int reaction : { 0, 1, 2, 4 })
        {
            dsp::NoiseBed bed;
            bed.prepare (sampleRate);
            bed.configure (reaction, 7, 0.6, 0.4, 0.3);
            bed.setAmount (0.8);

            std::vector<double> values;
            double env = 0.0;

            for (int i = 0; i < n; ++i)
            {
                const auto t = i / sampleRate;
                const auto wetL = 0.3 * std::sin (2.0 * M_PI * 200.0 * t);
                const auto wetR = 0.2 * std::sin (2.0 * M_PI * 330.0 * t + 0.3);
                const auto on = i >= 4410 && ((i - 4410) % 11025) < 2205;
                const auto dryL = on ? 0.5 * std::sin (2.0 * M_PI * 150.0 * t) : 0.0;
                const auto dryR = on ? 0.4 * std::sin (2.0 * M_PI * 170.0 * t + 0.2) : 0.0;

                if (i % 8 == 0)
                    env = 0.5 + 0.5 * std::sin (0.01 * (i / 8));

                for (int k = 0; k < 20; ++k)
                    if (1000 + 3731 * k == i)
                    {
                        dsp::ScheduledEvent e;
                        e.start = i;
                        e.index = static_cast<std::uint64_t> (k);
                        bed.trigger (e);
                    }

                double outL = 0.0, outR = 0.0;
                bed.process (i, wetL, wetR, dryL, dryR, env, outL, outR);

                if (i % 16 == 5)
                {
                    values.push_back (outL - wetL);
                    values.push_back (outR - wetR);
                }
            }

            printArray (names[reaction], values);
        }
    }

    // Reverb: an impulse excites every comb and allpass at once.
    {
        dsp::Reverb reverb;
        reverb.prepare (sampleRate);
        reverb.set (0.6, 0.35);

        std::vector<double> values;
        for (int i = 0; i < 20000; ++i)
        {
            double l = 0.0, r = 0.0;
            reverb.process (i == 0 ? 1.0 : 0.0, i == 0 ? 0.7 : 0.0, l, r);
            if (i % 50 == 0)
            {
                values.push_back (l);
                values.push_back (r);
            }
        }
        printArray ("reverb", values, true);
    }

    std::printf ("}\n");
    return 0;
}
