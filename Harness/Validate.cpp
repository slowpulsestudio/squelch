/** The DSP validation suite, dsp-testing.md's Tests 2, 3, 4, 5 and 15.

    The comparison harness proves the C++ computes the same numbers as the
    prototype, but only at 44.1 kHz, in one block configuration, in one pass.
    It cannot see a coefficient hardcoded in samples, a state variable that
    does not survive a block boundary, a buffer sized for one sample rate, or
    output that is not reproducible. Those are the faults a port introduces,
    and this is what looks for them.

    Prints one PASS/FAIL line per test and the verdict dsp-testing.md asks
    for. A failed invariant is never hidden behind an overall PASS.
*/

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../Source/Dsp/Alien.h"
#include "../Source/Dsp/Chemical.h"
#include "../Source/Dsp/Fission.h"
#include "../Source/Dsp/OutputStage.h"
#include "../Source/Dsp/Radiation.h"
#include "../Source/Dsp/Scheduler.h"
#include "../Source/Dsp/Sludge.h"

using namespace squelch;

namespace
{
    struct Stereo { std::vector<double> l, r; };

    //==============================================================================
    // dsp-testing.md's required test signals. Noise is hashed rather than
    // drawn from a generator so it is identical at every sample rate and in
    // every block configuration.
    //==============================================================================
    Stereo makeSignal (const std::string& name, double sr, double seconds)
    {
        const auto n = static_cast<int> (sr * seconds);
        Stereo s;
        s.l.resize (static_cast<size_t> (n));
        s.r.resize (static_cast<size_t> (n));

        for (int i = 0; i < n; ++i)
        {
            const auto t = i / sr;
            double a = 0.0, b = 0.0;

            if (name == "silence")              { a = b = 0.0; }
            else if (name == "impulse")         { a = b = (i == 0 ? 1.0 : 0.0); }
            else if (name == "sine100")         { a = b = 0.5 * std::sin (2.0 * M_PI * 100.0 * t); }
            else if (name == "sine440")         { a = b = 0.5 * std::sin (2.0 * M_PI * 440.0 * t); }
            else if (name == "sine1k")          { a = b = 0.5 * std::sin (2.0 * M_PI * 1000.0 * t); }
            else if (name == "sine5k")          { a = b = 0.5 * std::sin (2.0 * M_PI * 5000.0 * t); }
            else if (name == "lowsine")         { a = b = 0.5 * std::sin (2.0 * M_PI * 30.0 * t); }
            else if (name == "noise")           { a = b = 0.4 * rng::ubipolar ({ 99, 1, std::uint64_t (i) }); }
            else if (name == "saw")
            {
                const auto phase = std::fmod (220.0 * t, 1.0);
                a = b = 0.5 * (2.0 * phase - 1.0);
            }
            else if (name == "antiphase")       { a = 0.5 * std::sin (2.0 * M_PI * 440.0 * t); b = -a; }
            else if (name == "identical")       { a = b = 0.5 * std::sin (2.0 * M_PI * 440.0 * t); }
            else if (name == "maxlegal")        { a = b = (i % 2 ? 1.0 : -1.0); }

            s.l[static_cast<size_t> (i)] = a;
            s.r[static_cast<size_t> (i)] = b;
        }
        return s;
    }

    //==============================================================================
    // One reaction, rendered in blocks of a given size. The scheduler is driven
    // by absolute sample position, so block size must not change the result.
    //==============================================================================
    Stereo render (const std::string& reaction, const Stereo& in, double sr, int blockSize)
    {
        const auto n = static_cast<int> (in.l.size());
        Stereo out;
        out.l.resize (static_cast<size_t> (n));
        out.r.resize (static_cast<size_t> (n));

        dsp::ScheduleSettings settings;
        settings.gridIndex = 8;   // 1/16
        settings.bpm = 140.0;
        settings.flux = 0.3;
        settings.probability = 0.9;
        settings.reactivity = 0.5;
        settings.volatility = 0.4;
        settings.subEventBias = 1.0;
        settings.seed = 7;

        dsp::Scheduler scheduler;
        scheduler.prepare (sr);
        scheduler.configure (settings);

        dsp::SludgeEngine sludge;
        dsp::AlienEngine alien;
        dsp::ChemicalEngine chemical;
        dsp::RadiationEngine radiation;
        dsp::FissionEngine fission;

        sludge.prepare (sr);
        sludge.configure ({}, { 0.4, 0.6, 0.65, 0.5, 0.7, 0.35 });
        alien.prepare (sr);
        alien.configure ({}, { 0.6, 0.35, 0.7, 0.4, 5 });
        chemical.prepare (sr);
        chemical.setSeed (7);
        radiation.prepare (sr);
        radiation.configure ({}, { 0.7, 0.6, 0.45, 0.3, 7 });
        fission.prepare (sr);
        fission.configure ({}, { 0.8, 0.5, 0.6, 7 });

        std::vector<dsp::ScheduledEvent> pending;
        pending.reserve (64);

        for (int at = 0; at < n; at += blockSize)
        {
            const auto length = std::min (blockSize, n - at);

            pending.clear();
            scheduler.forRange (at, length, [&pending] (const dsp::ScheduledEvent& e)
            {
                pending.push_back (e);
            });

            for (int i = 0; i < length; ++i)
            {
                const auto position = static_cast<std::int64_t> (at + i);

                // Sample-accurate: an event fires at its own position, not at
                // the edge of whatever block it happened to land in. Firing at
                // the block start instead makes the output depend on the host's
                // buffer size, which is exactly what Test 4 exists to catch.
                for (const auto& e : pending)
                {
                    if (e.start != position)
                        continue;

                    if (reaction == "ALIEN")
                        alien.trigger (e.index, e.accent, e.pan, 0.0);
                    else if (reaction == "CHEMICAL")
                        chemical.setEvent (e.index);
                    else if (reaction == "RADIATION")
                        radiation.trigger (e.accent);
                }

                const auto k = static_cast<size_t> (position);
                const auto xl = in.l[k];
                const auto xr = in.r[k];
                double yl = 0.0, yr = 0.0;

                if (reaction == "SLUDGE")         sludge.process (xl, xr, yl, yr);
                else if (reaction == "ALIEN")     alien.process (yl, yr);
                else if (reaction == "CHEMICAL")  { yl = chemical.process (xl, 700.0, 2.2, 1.4);
                                                    yr = chemical.process (xr, 700.0, 2.2, 1.4); }
                else if (reaction == "RADIATION") radiation.process (xl, xr, yl, yr);
                else if (reaction == "FISSION")   fission.process (xl, xr, yl, yr);

                out.l[k] = yl;
                out.r[k] = yr;
            }
        }
        return out;
    }

    const std::vector<std::string> reactions { "SLUDGE", "ALIEN", "CHEMICAL", "RADIATION", "FISSION" };
    const std::vector<double> rates { 44100.0, 48000.0, 96000.0 };
    const std::vector<int> blockSizes { 32, 64, 128, 256, 512 };

    int failures = 0;
    int warnings = 0;

    void report (const std::string& test, bool ok, const std::string& detail)
    {
        if (! ok)
            ++failures;
        std::printf ("[%s] %-28s %s\n", ok ? "PASS" : "FAIL", test.c_str(), detail.c_str());
    }

    double maxAbs (const Stereo& s)
    {
        auto m = 0.0;
        for (size_t i = 0; i < s.l.size(); ++i)
            m = std::max (m, std::max (std::abs (s.l[i]), std::abs (s.r[i])));
        return m;
    }

    double rms (const Stereo& s)
    {
        auto sum = 0.0;
        for (size_t i = 0; i < s.l.size(); ++i)
            sum += s.l[i] * s.l[i] + s.r[i] * s.r[i];
        return std::sqrt (sum / std::max<size_t> (s.l.size() * 2, 1));
    }

    double dcOffset (const Stereo& s)
    {
        auto sum = 0.0;
        for (size_t i = 0; i < s.l.size(); ++i)
            sum += s.l[i] + s.r[i];
        return sum / std::max<size_t> (s.l.size() * 2, 1);
    }

    double maxDifference (const Stereo& a, const Stereo& b)
    {
        auto m = 0.0;
        const auto n = std::min (a.l.size(), b.l.size());
        for (size_t i = 0; i < n; ++i)
            m = std::max (m, std::max (std::abs (a.l[i] - b.l[i]), std::abs (a.r[i] - b.r[i])));
        return m;
    }

    //==============================================================================
    // Test 2 — finite output. The ceiling is generous on purpose: this is
    // looking for instability and NaN, not for a mix decision.
    //==============================================================================
    void test2()
    {
        constexpr auto ceiling = 100.0;
        for (const auto& reaction : reactions)
        {
            auto worstPeak = 0.0, worstRms = 0.0, worstDc = 0.0;
            int nans = 0, infs = 0;
            std::string worstSignal;

            for (const auto& signal : { "silence", "impulse", "sine440", "noise", "maxlegal" })
            {
                const auto out = render (reaction, makeSignal (signal, 44100.0, 1.0), 44100.0, 256);

                for (size_t i = 0; i < out.l.size(); ++i)
                    for (auto v : { out.l[i], out.r[i] })
                    {
                        if (std::isnan (v)) ++nans;
                        else if (std::isinf (v)) ++infs;
                    }

                const auto peak = maxAbs (out);
                if (peak > worstPeak) { worstPeak = peak; worstSignal = signal; }
                worstRms = std::max (worstRms, rms (out));
                worstDc = std::max (worstDc, std::abs (dcOffset (out)));
            }

            const auto ok = nans == 0 && infs == 0 && worstPeak < ceiling;
            report ("Test 2 finite " + reaction, ok,
                    "peak " + std::to_string (worstPeak).substr (0, 6)
                    + " (" + worstSignal + "), rms " + std::to_string (worstRms).substr (0, 6)
                    + ", |dc| " + std::to_string (worstDc).substr (0, 8)
                    + ", " + std::to_string (nans) + " NaN, " + std::to_string (infs) + " Inf");
        }
    }

    //==============================================================================
    // Test 3 — deterministic rendering. Same everything, twice, bit for bit.
    //==============================================================================
    void test3()
    {
        for (const auto& reaction : reactions)
        {
            const auto in = makeSignal ("noise", 44100.0, 1.0);
            const auto a = render (reaction, in, 44100.0, 256);
            const auto b = render (reaction, in, 44100.0, 256);
            const auto diff = maxDifference (a, b);
            report ("Test 3 determinism " + reaction, diff == 0.0,
                    "max |y1 - y2| = " + std::to_string (diff));
        }
    }

    //==============================================================================
    // Test 4 — block-size invariance. Every engine is per-sample and the
    // scheduler is driven by absolute position, so this must be exact, not
    // merely close. Anything else means a mechanism depends on block edges.
    //==============================================================================
    void test4()
    {
        for (const auto& reaction : reactions)
        {
            const auto in = makeSignal ("noise", 44100.0, 1.0);
            const auto reference = render (reaction, in, 44100.0, 512);

            auto worst = 0.0;
            int worstBlock = 0;
            for (auto block : blockSizes)
            {
                const auto diff = maxDifference (reference, render (reaction, in, 44100.0, block));
                if (diff > worst) { worst = diff; worstBlock = block; }
            }

            report ("Test 4 blocksize " + reaction, worst == 0.0,
                    "worst max difference " + std::to_string (worst)
                    + " at block " + std::to_string (worstBlock));
        }
    }

    //==============================================================================
    // Test 5 — sample-rate invariance. Measured in seconds and hertz, not in
    // samples: a coefficient hardcoded in samples moves when the rate does.
    //==============================================================================
    void test5()
    {
        // SLUDGE's subharmonic sits at f_h/2 = 50 Hz by construction, at any rate.
        std::vector<double> measured;
        for (auto sr : rates)
        {
            const auto out = render ("SLUDGE", makeSignal ("silence", sr, 2.0), sr, 256);

            // Zero crossings over the settled second give the dominant period.
            const auto from = static_cast<size_t> (sr);
            int crossings = 0;
            for (auto i = from + 1; i < out.l.size(); ++i)
                if ((out.l[i - 1] <= 0.0) != (out.l[i] <= 0.0))
                    ++crossings;

            const auto seconds = double (out.l.size() - from) / sr;
            measured.push_back (crossings / (2.0 * seconds));
        }

        const auto lo = *std::min_element (measured.begin(), measured.end());
        const auto hi = *std::max_element (measured.begin(), measured.end());
        const auto spreadCents = 1200.0 * std::log2 (hi / std::max (lo, 1e-9));

        report ("Test 5 samplerate SLUDGE", spreadCents < 50.0,
                "subharmonic rate " + std::to_string (measured[0]).substr (0, 6) + " / "
                + std::to_string (measured[1]).substr (0, 6) + " / "
                + std::to_string (measured[2]).substr (0, 6) + " Hz across 44k/48k/96k, spread "
                + std::to_string (spreadCents).substr (0, 5) + " cents");
    }

    //==============================================================================
    // Test 15 — silence behaviour. Bounded is the requirement, not silent:
    // several reactions are specified to generate from silence.
    //==============================================================================
    void test15()
    {
        for (const auto& reaction : reactions)
        {
            const auto out = render (reaction, makeSignal ("silence", 44100.0, 4.0), 44100.0, 256);
            const auto peak = maxAbs (out);

            // Growing without bound is the failure. Compare the last quarter
            // against the second quarter rather than asking for decay, which
            // ALIEN and RADIATION are allowed not to do.
            const auto quarter = out.l.size() / 4;
            auto early = 0.0, late = 0.0;
            for (size_t i = quarter; i < quarter * 2; ++i)
                early = std::max (early, std::abs (out.l[i]));
            for (size_t i = quarter * 3; i < out.l.size(); ++i)
                late = std::max (late, std::abs (out.l[i]));

            const auto growth = late / std::max (early, 1e-12);
            const auto ok = std::isfinite (peak) && peak < 100.0 && growth < 10.0;
            report ("Test 15 silence " + reaction, ok,
                    "peak " + std::to_string (peak).substr (0, 6)
                    + ", late/early " + std::to_string (growth).substr (0, 6));
        }
    }
    //==============================================================================
    // The four scheduler modes. GRID and FREE are covered numerically by the
    // comparison harness; RANDOM and INPUT are not, because neither can be
    // computed from the step index alone -- RANDOM accumulates its own step
    // lengths and INPUT reads the audio. This asserts what they are for: that
    // they fire at all, that they are reproducible, and that each lays events
    // out differently from the grid.
    //==============================================================================
    std::vector<std::int64_t> eventsOf (dsp::Mode mode, double flux, const Stereo& in, int blockSize)
    {
        dsp::ScheduleSettings settings;
        settings.gridIndex = 8;
        settings.bpm = 140.0;
        settings.flux = flux;
        settings.probability = 1.0;
        settings.mode = mode;
        settings.seed = 7;

        dsp::Scheduler scheduler;
        scheduler.prepare (44100.0);
        scheduler.configure (settings);

        std::vector<std::int64_t> starts;
        const auto n = static_cast<int> (in.l.size());

        for (int at = 0; at < n; at += blockSize)
        {
            const auto length = std::min (blockSize, n - at);
            scheduler.forRange (at, length, [&starts] (const dsp::ScheduledEvent& e)
            {
                starts.push_back (e.start);
            });

            for (int i = 0; i < length; ++i)
                scheduler.detectOnsets (std::abs (in.l[static_cast<size_t> (at + i)]),
                                        at + i,
                                        [&starts] (const dsp::ScheduledEvent& e)
                                        {
                                            starts.push_back (e.start);
                                        });
        }

        std::sort (starts.begin(), starts.end());
        return starts;
    }

    /// Eight bars of 1/16 pulses, so INPUT has onsets to find.
    Stereo makeTransients()
    {
        const auto sr = 44100.0;
        auto s = makeSignal ("silence", sr, 4.0);
        const auto period = static_cast<size_t> (sr * 60.0 / 140.0 / 4.0);

        for (size_t at = 0; at < s.l.size(); at += period)
            for (size_t i = 0; i < 200 && at + i < s.l.size(); ++i)
            {
                const auto v = std::exp (-double (i) / 40.0)
                             * std::sin (2.0 * M_PI * 220.0 * double (i) / sr);
                s.l[at + i] = v;
                s.r[at + i] = v;
            }

        return s;
    }

    void testSchedulerModes()
    {
        const auto in = makeTransients();

        const auto grid = eventsOf (dsp::Mode::grid, 0.0, in, 256);
        const auto random = eventsOf (dsp::Mode::random, 0.6, in, 256);
        const auto input = eventsOf (dsp::Mode::input, 0.0, in, 256);

        report ("Scheduler RANDOM fires", ! random.empty(),
                std::to_string (random.size()) + " events");
        report ("Scheduler INPUT fires", ! input.empty(),
                std::to_string (input.size()) + " events");

        // Reproducible: same settings, same events, regardless of block size.
        report ("Scheduler RANDOM is reproducible",
                random == eventsOf (dsp::Mode::random, 0.6, in, 256), "");
        report ("Scheduler INPUT is block-size invariant",
                input == eventsOf (dsp::Mode::input, 0.0, in, 64), "");

        // A mode that lands on the grid anyway is not a mode. Compare the
        // spacing between consecutive events rather than the count, which
        // probability alone could change.
        const auto spread = [] (const std::vector<std::int64_t>& v)
        {
            if (v.size() < 3) return 0.0;
            auto mean = 0.0;
            for (size_t i = 1; i < v.size(); ++i)
                mean += double (v[i] - v[i - 1]);
            mean /= double (v.size() - 1);

            auto variance = 0.0;
            for (size_t i = 1; i < v.size(); ++i)
            {
                const auto d = double (v[i] - v[i - 1]) - mean;
                variance += d * d;
            }
            return std::sqrt (variance / double (v.size() - 1)) / std::max (mean, 1e-9);
        };

        const auto gridSpread = spread (grid);
        const auto randomSpread = spread (random);
        report ("Scheduler RANDOM is not the grid", randomSpread > gridSpread + 0.1,
                "spacing spread " + std::to_string (randomSpread).substr (0, 5)
                + " vs grid " + std::to_string (gridSpread).substr (0, 5));

        // INPUT should track the transients it was given, within the detector's
        // hop. One onset per pulse, not a stream of them.
        const auto period = 44100.0 * 60.0 / 140.0 / 4.0;
        const auto expected = static_cast<size_t> (in.l.size() / period);
        const auto perPulse = double (input.size()) / double (std::max<size_t> (expected, 1));
        report ("Scheduler INPUT tracks the transients", perPulse > 0.5 && perPulse < 3.0,
                std::to_string (input.size()) + " events for "
                + std::to_string (expected) + " pulses");

        // And it is reading the audio rather than running free: nothing
        // arrives in INPUT mode when nothing arrives at the input. This is
        // what separates a detector from a timer.
        const auto onSilence = eventsOf (dsp::Mode::input, 0.0, makeSignal ("silence", 44100.0, 4.0), 256);
        report ("Scheduler INPUT is silent on silence", onSilence.empty(),
                std::to_string (onSilence.size()) + " events");

        // A steady tone has one onset at its start and no more; a detector
        // that fires on level rather than on change would run all the way
        // through it.
        const auto onTone = eventsOf (dsp::Mode::input, 0.0, makeSignal ("sine440", 44100.0, 4.0), 256);
        report ("Scheduler INPUT does not fire on a steady tone", onTone.size() <= 3,
                std::to_string (onTone.size()) + " events");
    }
}

int main()
{
    std::printf ("SQUELCH DSP validation\n\n");

    test2();
    std::printf ("\n");
    test3();
    std::printf ("\n");
    test4();
    std::printf ("\n");
    test5();
    std::printf ("\n");
    test15();
    std::printf ("\n");
    testSchedulerModes();

    std::printf ("\nSQUELCH DSP VALIDATION: %s\n", failures == 0 ? "PASS" : "FAIL");
    if (warnings > 0)
        std::printf ("%d warning(s)\n", warnings);

    return failures == 0 ? 0 : 1;
}
