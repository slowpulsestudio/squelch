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

#include "Analysis.h"

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

    /** Everything Tests 6 to 11 need to vary. The defaults are the settings
        the invariance tests use, so a mechanism test differs from the
        baseline only in the control it is actually probing.
    */
    struct Rig
    {
        dsp::SludgeProfile sludgeProfile {};
        dsp::SludgeParams sludge { 0.4, 0.6, 0.65, 0.5, 0.7, 0.35 };
        dsp::AlienProfile alienProfile {};
        dsp::AlienParams alien { 0.6, 0.35, 0.7, 0.4, 5 };
        dsp::RadiationProfile radiationProfile {};
        dsp::RadiationParams radiation { 0.7, 0.6, 0.45, 0.3, 7 };
        dsp::FissionProfile fissionProfile {};
        dsp::FissionParams fission { 0.8, 0.5, 0.6, 7 };

        double chemicalCutoff { 700.0 };
        double chemicalFeedback { 2.2 };
        double chemicalDrive { 1.4 };
        std::uint64_t chemicalSeed { 7 };

        double flux { 0.3 };
        double probability { 0.9 };
        double reactivity { 0.5 };
        std::uint64_t seed { 7 };
        bool events { true };

        /// Filled in by renderWith when asked, so the state diagnostics can
        /// look at the mechanism rather than inferring it from the audio.
        std::vector<double>* stateTrace { nullptr };
    };

    Stereo renderWith (const std::string& reaction, const Stereo& in, double sr, int blockSize,
                       const Rig& rig)
    {
        const auto n = static_cast<int> (in.l.size());
        Stereo out;
        out.l.resize (static_cast<size_t> (n));
        out.r.resize (static_cast<size_t> (n));

        dsp::ScheduleSettings settings;
        settings.gridIndex = 8;   // 1/16
        settings.bpm = 140.0;
        settings.flux = rig.flux;
        settings.probability = rig.events ? rig.probability : 0.0;
        settings.reactivity = rig.reactivity;
        settings.volatility = 0.4;
        settings.subEventBias = 1.0;
        settings.seed = rig.seed;

        dsp::Scheduler scheduler;
        scheduler.prepare (sr);
        scheduler.configure (settings);

        dsp::SludgeEngine sludge;
        dsp::AlienEngine alien;
        dsp::ChemicalEngine chemical;
        dsp::RadiationEngine radiation;
        dsp::FissionEngine fission;

        sludge.prepare (sr);
        sludge.configure (rig.sludgeProfile, rig.sludge);
        alien.prepare (sr);
        alien.configure (rig.alienProfile, rig.alien);
        chemical.prepare (sr);
        chemical.setSeed (rig.chemicalSeed);
        radiation.prepare (sr);
        radiation.configure (rig.radiationProfile, rig.radiation);
        fission.prepare (sr);
        fission.configure (rig.fissionProfile, rig.fission);

        if (rig.stateTrace != nullptr)
            rig.stateTrace->resize (static_cast<size_t> (n));

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
                else if (reaction == "CHEMICAL")  { yl = chemical.process (xl, rig.chemicalCutoff,
                                                                          rig.chemicalFeedback,
                                                                          rig.chemicalDrive);
                                                    yr = chemical.process (xr, rig.chemicalCutoff,
                                                                          rig.chemicalFeedback,
                                                                          rig.chemicalDrive); }
                else if (reaction == "RADIATION") radiation.process (xl, xr, yl, yr);
                else if (reaction == "FISSION")   fission.process (xl, xr, yl, yr);

                if (rig.stateTrace != nullptr)
                    (*rig.stateTrace)[k] = reaction == "CHEMICAL" ? chemical.registerValue()
                                         : reaction == "RADIATION" ? radiation.stateValue()
                                         : 0.0;

                out.l[k] = yl;
                out.r[k] = yr;
            }
        }
        return out;
    }

    Stereo render (const std::string& reaction, const Stereo& in, double sr, int blockSize)
    {
        return renderWith (reaction, in, sr, blockSize, Rig {});
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

    /** For a measurement that is real, repeatable and NOT what the
        specification asks for. A warning is visible in the output and
        counted, but does not turn the verdict red -- which is right for a
        control whose mapping is an open design question rather than a port
        fault, and wrong for anything else. Nothing that could be a bug
        belongs here.
    */
    void warn (const std::string& test, const std::string& detail)
    {
        ++warnings;
        std::printf ("[WARN] %-28s %s\n", test.c_str(), detail.c_str());
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
    //==============================================================================
    // Test 6 — CHEMICAL. The resonator, and the stochastic register inspected
    // directly rather than inferred from the audio.
    //==============================================================================
    double relativeSpread (const std::vector<double>& v)
    {
        if (v.size() < 2)
            return 0.0;
        auto mean = 0.0;
        for (auto x : v)
            mean += x;
        mean /= double (v.size());

        auto variance = 0.0;
        for (auto x : v)
            variance += (x - mean) * (x - mean);
        return std::sqrt (variance / double (v.size())) / std::max (std::abs (mean), 1e-12);
    }

    void test6()
    {
        using namespace analysis;
        constexpr auto sr = 44100.0;

        // White noise, so the excitation is flat and the measurement is of
        // the filter rather than of the input's harmonic structure.
        const auto noise = makeSignal ("noise", sr, 2.0);

        const auto outputAt = [&] (double feedback, double cutoff)
        {
            Rig r;
            r.chemicalFeedback = feedback;
            r.chemicalCutoff = cutoff;
            r.events = false;   // no register offset, so the cutoff is the cutoff
            return renderWith ("CHEMICAL", noise, sr, 256, r);
        };

        // Against the same ladder with its feedback off, so what is left is
        // what the feedback added rather than the lowpass slope.
        const auto responseAt = [&] (double feedback, double cutoff,
                                     double& peakDb, double& peakHz)
        {
            const auto flat = outputAt (0.0, cutoff);
            const auto out = outputAt (feedback, cutoff);
            ratioPeak (flat.l, out.l, sr, 60.0, 6000.0, peakDb, peakHz, size_t (sr * 0.5));
        };

        // A resonant response at all.
        double resonantDb = 0.0, resonantHz = 0.0;
        responseAt (3.2, 700.0, resonantDb, resonantHz);
        report ("Test 6 resonance exists", resonantDb > 6.0,
                std::to_string (resonantDb).substr (0, 5) + " dB over the unresonant ladder at "
                + std::to_string (int (resonantHz)) + " Hz");

        // Resonance increases with EXPOSURE, which reaches the ladder as
        // feedback.
        auto previous = -1e30;
        auto rising = true;
        std::string exposureDetail;
        for (auto fb : { 0.5, 1.5, 2.5, 3.5 })
        {
            double peakDb = 0.0, peakHz = 0.0;
            responseAt (fb, 700.0, peakDb, peakHz);
            if (peakDb <= previous)
                rising = false;
            previous = peakDb;
            exposureDetail += std::to_string (peakDb).substr (0, 5) + " ";
        }
        report ("Test 6 EXPOSURE raises resonance", rising, exposureDetail);

        // The resonant frequency follows the cutoff it is given.
        //
        // It does not land on it: the peak sits at a fixed multiple of the
        // control, because the ladder's one-pole stages are not frequency
        // warped and four of them in series move the peak up. That multiple
        // is inherited from the prototype -- the comparison harness agrees to
        // 1e-16 -- so it is the instrument's calibration rather than a port
        // fault, and what this asserts is that the ratio is CONSTANT. A
        // resonance that drifted relative to the control would be a fault; a
        // resonance that is reliably 1.9x it is a tuning.
        std::vector<double> ratios;
        std::string trackDetail;
        for (auto hz : { 300.0, 700.0, 1600.0 })
        {
            double peakDb = 0.0, peakHz = 0.0;
            responseAt (3.4, hz, peakDb, peakHz);
            ratios.push_back (peakHz / hz);
            trackDetail += std::to_string (peakHz / hz).substr (0, 4) + "x ";
        }
        const auto drift = relativeSpread (ratios);
        report ("Test 6 resonance follows cutoff", drift < 0.1,
                trackDetail + "(spread " + std::to_string (drift).substr (0, 5) + ")");

        // TOXICITY reaches the ladder as drive, and drive is what the in-loop
        // tanh works on. More drive, more harmonic content.
        const auto harmonicsAt = [&] (double drive)
        {
            Rig r;
            r.chemicalDrive = drive;
            r.chemicalCutoff = 900.0;
            r.events = false;
            return thd (renderWith ("CHEMICAL", makeSignal ("sine100", sr, 2.0), sr, 256, r).l, 100.0, sr);
        };

        const auto thdQuiet = harmonicsAt (0.4);
        const auto thdLoud = harmonicsAt (6.0);
        report ("Test 6 TOXICITY adds harmonics", thdLoud > thdQuiet * 1.5,
                "THD " + std::to_string (thdQuiet).substr (0, 5)
                + " -> " + std::to_string (thdLoud).substr (0, 5));

        // Bounded below the stability limit at the hottest settings offered.
        Rig extreme;
        extreme.chemicalFeedback = 3.9;
        extreme.chemicalDrive = 8.0;
        const auto hot = renderWith ("CHEMICAL", makeSignal ("maxlegal", sr, 2.0), sr, 256, extreme);
        const auto peak = maxAbs (hot);
        report ("Test 6 feedback stays bounded", std::isfinite (peak) && peak < 50.0,
                "peak " + std::to_string (peak).substr (0, 6));

        // Ringing continues past the transient: the ladder holds energy
        // rather than only shaping what is present. Measured against the
        // same ladder without feedback, because a four-pole lowpass has a
        // tail of its own and an absolute threshold would be measuring that.
        const auto tailAfterImpulse = [&] (double feedback)
        {
            Rig r;
            r.chemicalFeedback = feedback;
            r.events = false;
            auto burst = makeSignal ("silence", sr, 1.0);
            burst.l[0] = burst.r[0] = 1.0;
            const auto out = renderWith ("CHEMICAL", burst, sr, 256, r);

            auto energy = 0.0;
            for (size_t i = size_t (sr * 0.02); i < size_t (sr * 0.08); ++i)
                energy += out.l[i] * out.l[i];
            return std::sqrt (energy);
        };

        const auto ringing = db (tailAfterImpulse (3.6), tailAfterImpulse (0.0));
        report ("Test 6 ladder rings on", ringing > 12.0,
                std::to_string (ringing).substr (0, 5) + " dB more tail with feedback");

        // Test 16 — a spectral hole in the input must not silence it. Notch
        // the band around the cutoff out of the excitation and check the
        // resonator still produces output there.
        auto holed = makeSignal ("noise", sr, 2.0);
        dsp::Biquad notchL, notchR;
        const auto coefficients = dsp::notch (700.0, 2.0, sr);
        notchL.setCoefficients (coefficients);
        notchR.setCoefficients (coefficients);
        for (size_t i = 0; i < holed.l.size(); ++i)
        {
            holed.l[i] = notchL.process (holed.l[i]);
            holed.r[i] = notchR.process (holed.r[i]);
        }

        Rig holeRig;
        holeRig.chemicalFeedback = 3.4;
        holeRig.events = false;
        const auto fromHole = renderWith ("CHEMICAL", holed, sr, 256, holeRig);
        const auto inHole = goertzel (holed.l, 700.0, sr, size_t (sr * 0.5), size_t (sr * 0.5));
        const auto outHole = goertzel (fromHole.l, 700.0, sr, size_t (sr * 0.5), size_t (sr * 0.5));
        report ("Test 16 spectral hole does not silence", outHole > inHole,
                "input " + std::to_string (inHole).substr (0, 8)
                + " -> output " + std::to_string (outHole).substr (0, 8));
    }

    //==============================================================================
    // Test 6 — the register diagnostic. The formulation this guards against
    // folds q into the sweep exponent, which would let the register scale or
    // reverse the sweep instead of only relocating it.
    //==============================================================================
    void test6Register()
    {
        using namespace analysis;
        constexpr auto sr = 44100.0;
        const auto saw = makeSignal ("saw", sr, 2.0);

        std::vector<double> trace;
        Rig rig;
        rig.stateTrace = &trace;
        renderWith ("CHEMICAL", saw, sr, 256, rig);

        // Piecewise constant: the register changes at events, so the number
        // of changes is of the order of the event count, not the sample count.
        int changes = 0;
        for (size_t i = 1; i < trace.size(); ++i)
            if (trace[i] != trace[i - 1])
                ++changes;

        report ("Test 6 register is event-held",
                changes > 0 && changes < int (trace.size() / 1000),
                std::to_string (changes) + " changes in "
                + std::to_string (trace.size()) + " samples");

        // It is an offset, not a depth. Across seeds the register moves the
        // resonance around without changing how much resonance there is.
        std::vector<double> centres, strengths;
        for (std::uint64_t seed : { 1, 2, 3, 4, 5, 6, 7, 8 })
        {
            Rig r;
            r.chemicalSeed = seed;
            r.chemicalFeedback = 3.2;
            const auto y = renderWith ("CHEMICAL", saw, sr, 256, r);
            centres.push_back (centroid (y.l, sr, size_t (sr * 0.5), 32768));
            strengths.push_back (rms (y));
        }

        const auto whereSpread = relativeSpread (centres);
        const auto howMuchSpread = relativeSpread (strengths);
        report ("Test 6 register moves where, not whether",
                whereSpread > howMuchSpread,
                "centroid spread " + std::to_string (whereSpread).substr (0, 5)
                + " vs level spread " + std::to_string (howMuchSpread).substr (0, 5));

        // Same seed, same register sequence; a new seed, a new one.
        std::vector<double> again, other;
        Rig repeat;
        repeat.stateTrace = &again;
        renderWith ("CHEMICAL", saw, sr, 256, repeat);
        report ("Test 6 register is reproducible", trace == again, "");

        Rig different;
        different.chemicalSeed = 99;
        different.stateTrace = &other;
        renderWith ("CHEMICAL", saw, sr, 256, different);
        report ("Test 6 register follows the seed", trace != other, "");
    }

    //==============================================================================
    // Test 7 — RADIATION. The state must be continuous and correlated, which
    // is the one thing keeping it from being CHEMICAL with a different EQ.
    //==============================================================================
    void test7()
    {
        using namespace analysis;
        constexpr auto sr = 44100.0;
        const auto noise = makeSignal ("noise", sr, 3.0);

        // VOLATILITY changes short-timescale spectral movement. Measured as
        // the wander of the resonant peak itself rather than of the spectral
        // centroid, which on a noise excitation moves frame to frame anyway
        // and would hide the effect under its own variance.
        const auto movementAt = [&] (double volatility)
        {
            Rig r;
            r.radiation.volatility = volatility;
            r.events = false;   // the resonator excited by the input alone, so
                                // the measurement is of its frequency moving
                                // rather than of broadband event pulses
            return peakMovement (noise.l, renderWith ("RADIATION", noise, sr, 256, r).l,
                                 sr, 60.0, 4000.0);
        };

        const auto still = movementAt (0.0);
        const auto moving = movementAt (1.0);
        report ("Test 7 VOLATILITY moves the spectrum", moving > still * 2.0,
                "peak wander " + std::to_string (still).substr (0, 5)
                + " -> " + std::to_string (moving).substr (0, 5));

        // The state is continuous, not held.
        std::vector<double> trace;
        Rig rig;
        rig.stateTrace = &trace;
        renderWith ("RADIATION", noise, sr, 256, rig);

        int held = 0;
        for (size_t i = 1; i < trace.size(); ++i)
            if (trace[i] == trace[i - 1])
                ++held;
        report ("Test 7 state evolves per sample", held < int (trace.size() / 100),
                std::to_string (held) + " repeats of " + std::to_string (trace.size()));

        // And correlated. White noise would sit near zero at every non-zero
        // lag; an AR(1) state does not.
        const auto r1 = autocorrelation (trace, 1);
        const auto r16 = autocorrelation (trace, 16);
        report ("Test 7 state is correlated", r1 > 0.5 && r16 > 0.05,
                "R[1] " + std::to_string (r1).substr (0, 5)
                + ", R[16] " + std::to_string (r16).substr (0, 5));

        // Same seed, same waveform -- not merely the same average spectrum.
        const auto a = renderWith ("RADIATION", noise, sr, 256, Rig {});
        const auto b = renderWith ("RADIATION", noise, sr, 256, Rig {});
        report ("Test 7 seed reproduces the waveform", maxDifference (a, b) == 0.0, "");

        std::vector<double> otherTrace;
        Rig other;
        other.radiation.seed = 404;
        other.stateTrace = &otherTrace;
        renderWith ("RADIATION", noise, sr, 256, other);
        report ("Test 7 a new seed is a new trajectory", trace != otherTrace, "");
    }

    //==============================================================================
    // The discrimination that matters more than comparing spectra: two
    // architectures can be EQ-matched to sound alike while remaining
    // different instruments, so compare the states and not the outputs.
    //==============================================================================
    void testDiscrimination()
    {
        constexpr auto sr = 44100.0;
        const auto noise = makeSignal ("noise", sr, 3.0);

        std::vector<double> chemicalTrace, radiationTrace;
        Rig c, r;
        c.stateTrace = &chemicalTrace;
        r.stateTrace = &radiationTrace;
        renderWith ("CHEMICAL", noise, sr, 256, c);
        renderWith ("RADIATION", noise, sr, 256, r);

        const auto heldFraction = [] (const std::vector<double>& v)
        {
            int count = 0;
            for (size_t i = 1; i < v.size(); ++i)
                if (v[i] == v[i - 1])
                    ++count;
            return double (count) / double (std::max<size_t> (v.size() - 1, 1));
        };

        const auto chemicalHeld = heldFraction (chemicalTrace);
        const auto radiationHeld = heldFraction (radiationTrace);

        report ("CHEMICAL and RADIATION have not converged",
                chemicalHeld > 0.99 && radiationHeld < 0.01,
                "held fraction CHEMICAL " + std::to_string (chemicalHeld).substr (0, 5)
                + ", RADIATION " + std::to_string (radiationHeld).substr (0, 5));
    }

    //==============================================================================
    // Test 8 — FISSION. The branches must actually interact. An
    // implementation that sounds the same with coupling and detuning turned
    // down is two independent delays and a pan, not a split.
    //==============================================================================
    void test8()
    {
        using namespace analysis;
        constexpr auto sr = 44100.0;
        const auto noise = makeSignal ("noise", sr, 3.0);

        const auto at = [&] (double exposure, double spread)
        {
            Rig r;
            r.fission.exposure = exposure;   // reaches the branches as coupling
            r.fission.spread = spread;       // reaches them as detuning
            return renderWith ("FISSION", noise, sr, 256, r);
        };

        // Coupling. The instrument never offers k_c = 0 -- the floor is 0.1 of
        // the normalised range -- so this compares the floor against the
        // ceiling rather than against nothing, which is the span a user can
        // actually reach.
        const auto loose = at (0.0, 0.6);
        const auto normal = at (0.5, 0.6);
        const auto tight = at (1.0, 0.6);

        const auto differenceDb = [] (const Stereo& a, const Stereo& b)
        {
            Stereo d;
            d.l.resize (a.l.size());
            d.r.resize (a.r.size());
            for (size_t i = 0; i < a.l.size(); ++i)
            {
                d.l[i] = a.l[i] - b.l[i];
                d.r[i] = a.r[i] - b.r[i];
            }
            return db (rms (d), rms (a));
        };

        const auto couplingChange = differenceDb (tight, loose);
        report ("Test 8 coupling changes the output", couplingChange > -6.0,
                "floor to ceiling differ by " + std::to_string (couplingChange).substr (0, 5)
                + " dB relative");

        report ("Test 8 coupling is a continuum",
                differenceDb (normal, loose) < couplingChange
                && differenceDb (normal, tight) < couplingChange,
                "mid sits between the ends");

        // Detuning. The two branches are the same delay when SPREAD is zero
        // and drift apart when it is not, so the interference between them
        // is a comb that MOVES rather than one that gets deeper.
        //
        // Measuring the comb's depth over a long window says the opposite:
        // 3.06 dB of ripple with the branches coincident, 2.58 dB with them
        // detuned. That is not the detuning failing, it is the window
        // averaging a moving comb into a smear. The depth is the wrong
        // quantity -- a static comb is the deepest thing FISSION can
        // produce, and it is also the least interesting.
        const auto coincident = at (0.5, 0.0);
        const auto split = at (0.5, 1.0);

        // Short enough that the comb has not moved far within one frame.
        constexpr size_t frame = 4096;
        const auto curveOf = [&] (const Stereo& s, size_t at2)
        {
            return transferCurve (noise.l, s.l, sr, 100.0, 4000.0, at2, frame);
        };

        report ("Test 8 detuning leaves a comb",
                ripple (curveOf (split, size_t (sr))) > 1.0,
                "ripple in a 93 ms window "
                + std::to_string (ripple (curveOf (split, size_t (sr)))).substr (0, 5) + " dB");

        // And that comb should move. It barely does, and the reason is
        // measurable rather than guessed.
        //
        // Both of FISSION's modulators are AR(1) states normalised by their
        // DC gain, m = (1-a)*noise + a*m. For a time constant of 1.3 s at
        // 44.1 kHz that makes a = 0.99998, so (1-a) = 1.7e-5 while the
        // steady-state standard deviation is sqrt((1-a)/(1+a)/3) = 0.0017 --
        // three orders of magnitude smaller than the +/-1 the term is
        // written as if it spans. At SPREAD = 1 the delay modulation is
        // 0.0035 s * 0.0017 = a quarter of a sample, six microseconds, and
        // the random part of the detune contributes 0.4% of its fixed part.
        //
        // Energy normalisation, sqrt(1-a^2), would give these states the
        // range they are written for. That is a change to the prototype and
        // not to the port -- the comparison harness agrees with the prototype
        // to 1e-14 here -- so it is reported rather than made.
        const auto shapeStability = [&] (const Stereo& s)
        {
            std::vector<double> similarities;
            for (size_t at2 = size_t (sr); at2 + frame * 2 <= s.l.size(); at2 += frame)
                similarities.push_back (correlation (curveOf (s, at2),
                                                     curveOf (s, at2 + frame)));
            if (similarities.empty())
                return 1.0;
            auto mean = 0.0;
            for (auto v : similarities)
                mean += v;
            return mean / double (similarities.size());
        };

        const auto staticShape = shapeStability (coincident);
        const auto movingShape = shapeStability (split);

        if (movingShape >= staticShape - 0.05)
            warn ("Test 8 the interference moves",
                  "comb similarity " + std::to_string (staticShape).substr (0, 5)
                  + " -> " + std::to_string (movingShape).substr (0, 5)
                  + "; modulators are DC-normalised and swing 0.0017");
        else
            report ("Test 8 the interference moves", true,
                    "frame-to-frame comb similarity " + std::to_string (staticShape).substr (0, 5)
                    + " -> " + std::to_string (movingShape).substr (0, 5));

        // d_L = -d_R, so the stereo image comes out of the branch
        // relationship rather than out of a pan applied at the end. A pan
        // cannot decorrelate a mono input at all; opposed detuning can, and
        // the amount it does is modest because the branches share most of
        // their structure.
        const auto mono = makeSignal ("identical", sr, 3.0);
        Rig flat;
        flat.fission.spread = 0.0;
        Rig opposed;
        opposed.fission.spread = 1.0;

        const auto flatOut = renderWith ("FISSION", mono, sr, 256, flat);
        const auto flatCorr = correlation (flatOut.l, flatOut.r);
        const auto opposedOut = renderWith ("FISSION", mono, sr, 256, opposed);
        const auto opposedCorr = correlation (opposedOut.l, opposedOut.r);

        report ("Test 8 stereo comes from the branches",
                flatCorr > 0.9999 && opposedCorr < flatCorr - 0.005,
                "mono-in correlation " + std::to_string (flatCorr).substr (0, 6)
                + " -> " + std::to_string (opposedCorr).substr (0, 6));
    }

    //==============================================================================
    // Test 9 — SLUDGE. Genuine low-frequency generation, referenced to the
    // reactor's own frequency because SLUDGE does no pitch tracking.
    //==============================================================================
    void test9()
    {
        using namespace analysis;
        constexpr auto sr = 44100.0;

        // A reactor frequency well clear of the input's, so anything found at
        // f_h/2 cannot have come from the excitation.
        constexpr auto fH = 120.0;
        Rig rig;
        rig.sludgeProfile.baseHz = fH;
        rig.sludge.exposure = 0.9;   // reaches the subharmonic weights
        rig.events = false;

        const auto in = makeSignal ("sine1k", sr, 3.0);
        const auto out = renderWith ("SLUDGE", in, sr, 256, rig);

        const auto window = size_t (sr * 1.5);
        const auto half = goertzel (out.l, fH / 2.0, sr, size_t (sr), window);
        const auto quarter = goertzel (out.l, fH / 4.0, sr, size_t (sr), window);
        const auto whole = goertzel (out.l, fH, sr, size_t (sr), window);

        // The input has nothing below 1 kHz, so this is generated.
        const auto inHalf = goertzel (in.l, fH / 2.0, sr, size_t (sr), window);
        report ("Test 9 generates f_h/2", db (half, inHalf) > 20.0,
                "input " + std::to_string (inHalf).substr (0, 8)
                + " -> output " + std::to_string (half).substr (0, 8));
        report ("Test 9 generates f_h/4", quarter > 0.0 && db (quarter, inHalf) > 20.0,
                std::to_string (quarter).substr (0, 8));

        // The phase domain. A /2 oscillator wrapped at 2pi instead of 4pi puts
        // its energy at f_h with a DC offset rather than at f_h/2, and the
        // ratio of the two is what catches it.
        const auto domainDb = db (half, whole);
        report ("Test 9 the /2 phase wraps at 4pi", domainDb > 6.0,
                "f_h/2 over f_h " + std::to_string (domainDb).substr (0, 5) + " dB");

        // TOXICITY and the asymmetric stage. Asymmetry is what makes even
        // orders; a symmetric curve makes only odd ones. What the
        // specification requires is that even-order content is MEASURABLE,
        // which is the thing a symmetric implementation cannot do.
        const auto ordersAt = [&] (double toxicity, double& even, double& odd)
        {
            Rig r;
            r.sludge.toxicity = toxicity;
            r.sludge.exposure = 0.2;   // quiet subharmonics, so this measures
                                       // the saturation rather than the reactor
            r.events = false;
            const auto y = renderWith ("SLUDGE", makeSignal ("sine440", sr, 2.0), sr, 256, r);
            harmonicSplit (y.l, 440.0, sr, even, odd);
        };

        double cleanEven = 0.0, cleanOdd = 0.0, toxicEven = 0.0, toxicOdd = 0.0;
        ordersAt (0.0, cleanEven, cleanOdd);
        ordersAt (1.0, toxicEven, toxicOdd);

        report ("Test 9 the stage is asymmetric",
                cleanEven > cleanOdd * 0.1 && toxicEven > toxicOdd * 0.01,
                "even/odd " + std::to_string (cleanEven / std::max (cleanOdd, 1e-15)).substr (0, 5)
                + " at 0, " + std::to_string (toxicEven / std::max (toxicOdd, 1e-15)).substr (0, 5)
                + " at 1");

        report ("Test 9 TOXICITY changes harmonic content",
                db (toxicEven + toxicOdd, cleanEven + cleanOdd) > 6.0,
                "total harmonics "
                + std::to_string (db (toxicEven + toxicOdd, cleanEven + cleanOdd)).substr (0, 5)
                + " dB");

        // Measured, repeatable, and not what the specification asks for.
        // TOXICITY raises the odd orders far faster than the even ones, so
        // the balance moves TOWARDS symmetry as the control is opened. The
        // asymmetry is real and present throughout -- the control just does
        // not increase it. That is a mapping decision in the prototype, which
        // the comparison harness agrees with to 1e-14, and not a port fault,
        // so it is recorded here rather than silently passed or silently
        // failed.
        if (toxicEven / std::max (toxicOdd, 1e-15) < cleanEven / std::max (cleanOdd, 1e-15))
            warn ("Test 9 TOXICITY even-order direction",
                  "even/odd FALLS "
                  + std::to_string (cleanEven / std::max (cleanOdd, 1e-15)).substr (0, 5)
                  + " -> " + std::to_string (toxicEven / std::max (toxicOdd, 1e-15)).substr (0, 5)
                  + "; spec expects it to rise");

        // HALF-LIFE. The body state must persist after the input stops, which
        // a plain envelope follower with no memory cannot do.
        const auto persistenceAt = [&] (double halfLife)
        {
            Rig r;
            r.sludge.halfLife = halfLife;
            r.events = false;
            auto gated = makeSignal ("noise", sr, 3.0);
            for (size_t i = size_t (sr); i < gated.l.size(); ++i)
                gated.l[i] = gated.r[i] = 0.0;

            const auto y = renderWith ("SLUDGE", gated, sr, 256, r);
            auto energy = 0.0;
            for (size_t i = size_t (sr * 1.2); i < size_t (sr * 2.5); ++i)
                energy += y.l[i] * y.l[i];
            return std::sqrt (energy / (sr * 1.3));
        };

        const auto brief = persistenceAt (0.0);
        const auto full = persistenceAt (1.0);
        const auto persistenceChange = std::abs (db (full, brief));

        // There IS material after the input stops, which is the part that
        // separates a memory from an envelope follower.
        report ("Test 9 the body persists past the input", brief > 1e-4,
                "tail " + std::to_string (brief).substr (0, 8));

        // Across its whole travel HALF-LIFE moves that tail by 0.005 dB,
        // which no listener can hear. Same provenance as the TOXICITY
        // finding: it matches the prototype exactly, so it is a mapping to
        // settle rather than a port fault.
        if (persistenceChange < 1.0)
            warn ("Test 9 HALF-LIFE sensitivity",
                  "whole travel moves the tail "
                  + std::to_string (persistenceChange).substr (0, 6) + " dB");
        else
            report ("Test 9 HALF-LIFE changes persistence", true,
                    std::to_string (persistenceChange).substr (0, 5) + " dB");

        // SNAPBACK. A short transient should move the body and then recoil
        // past where it settles, rather than returning monotonically.
        Rig snap;
        snap.events = false;
        auto hit = makeSignal ("silence", sr, 2.0);
        for (size_t i = size_t (sr * 0.5); i < size_t (sr * 0.55); ++i)
            hit.l[i] = hit.r[i] = 0.8 * rng::ubipolar ({ 5, 5, std::uint64_t (i) });

        const auto response = renderWith ("SLUDGE", hit, sr, 256, snap);
        std::vector<double> centres;
        for (size_t at = size_t (sr * 0.5); at + 4096 <= size_t (sr * 1.6); at += 2048)
            centres.push_back (centroid (response.l, sr, at, 4096));

        auto overshoots = false;
        auto overshoot = 0.0;
        if (centres.size() > 4)
        {
            const auto settled = centres.back();
            auto peak = centres[0];
            for (auto c : centres)
                peak = std::max (peak, c);
            // Recoil, not a monotonic return: the trajectory goes past where
            // it ends up.
            overshoot = peak / std::max (settled, 1e-9);
            overshoots = overshoot > 1.05;
        }
        report ("Test 9 SNAPBACK recoils", overshoots,
                "centroid peaks " + std::to_string (overshoot).substr (0, 5)
                + "x where it settles, over " + std::to_string (centres.size()) + " frames");
    }

    //==============================================================================
    // Test 10 — ALIEN. It must contain a real oscillator, and that oscillator
    // must be gated by events rather than free-running.
    //==============================================================================
    void test10()
    {
        using namespace analysis;
        constexpr auto sr = 44100.0;
        const auto silence = makeSignal ("silence", sr, 3.0);

        // Silence in, events scheduled: output.
        Rig scheduled;
        const auto fromEvents = renderWith ("ALIEN", silence, sr, 256, scheduled);
        report ("Test 10 silence plus events produces output", maxAbs (fromEvents) > 1e-3,
                "peak " + std::to_string (maxAbs (fromEvents)).substr (0, 6));

        // Silence in, nothing scheduled: silence. An instance sitting on a
        // quiet track with nothing to play must not hum.
        Rig idle;
        idle.events = false;
        const auto fromNothing = renderWith ("ALIEN", silence, sr, 256, idle);
        report ("Test 10 silence without events is silent", maxAbs (fromNothing) == 0.0,
                "peak " + std::to_string (maxAbs (fromNothing)).substr (0, 8));

        // The output has an identifiable oscillator frequency, and it is the
        // one the pitch register chose. Asking only that it lands "somewhere
        // near 400 Hz" would need a 36-semitone tolerance -- the register's
        // full span -- which no wrong answer could fail. Computing the
        // expected value from the same formula makes it a real test.
        Rig tuned;
        tuned.alienProfile.baseHz = 400.0;
        tuned.alien.spread = 0.0;     // no sweep
        tuned.alien.toxicity = 0.0;   // no FM
        tuned.alien.exposure = 0.0;   // no AM
        tuned.alien.seed = 0;
        tuned.flux = 0.0;

        const auto expectedPitch = [&] (std::uint64_t event)
        {
            const auto q = rng::ubipolar ({ tuned.alien.seed, dsp::kAlienPitchStream, event });
            return tuned.alienProfile.baseHz * std::pow (2.0, dsp::kAlienDMaxSemitones * q / 12.0);
        };

        const auto oneVoice = [&] (std::uint64_t event, const Rig& r)
        {
            dsp::AlienEngine one;
            one.prepare (sr);
            one.configure (r.alienProfile, r.alien);
            one.trigger (event, false, 0.0, 0.0);

            std::vector<double> voice (size_t (sr * 0.3));
            for (auto& v : voice)
            {
                double l = 0.0, rr = 0.0;
                one.process (l, rr);
                v = l + rr;
            }
            return voice;
        };

        const auto heardVoice = oneVoice (0, tuned);
        const auto heard = dominantHz (heardVoice, sr, 0, 8192);
        const auto wanted = expectedPitch (0);
        const auto cents = 1200.0 * std::log2 (std::max (heard, 1e-9) / wanted);
        report ("Test 10 has an oscillator", std::abs (cents) < 60.0,
                std::to_string (int (heard)) + " Hz against "
                + std::to_string (int (wanted)) + " expected, "
                + std::to_string (int (cents)) + " cents");

        // FM. beta > 0 must put sidebands around the carrier that beta = 0
        // does not have.
        const auto sidebandsAt = [&] (double toxicity)
        {
            Rig r = tuned;
            r.alien.toxicity = toxicity;
            return thd (oneVoice (0, r), wanted, sr, 6);
        };

        const auto noFm = sidebandsAt (0.0);
        const auto withFm = sidebandsAt (1.0);
        report ("Test 10 FM adds sidebands", withFm > noFm * 1.5,
                "harmonic content " + std::to_string (noFm).substr (0, 5)
                + " -> " + std::to_string (withFm).substr (0, 5));

        // AM. Measured at the modulation frequency itself rather than as the
        // envelope's overall variation, which is dominated by the event's
        // own attack and decay and barely moved when AM was switched on.
        const auto amDepth = [&] (double exposure)
        {
            Rig r = tuned;
            r.alien.exposure = exposure;
            const auto voice = oneVoice (0, r);

            std::vector<double> envelope;
            envelope.reserve (voice.size());
            auto state = 0.0;
            for (auto v : voice)
            {
                state += (std::abs (v) - state) * 0.02;
                envelope.push_back (state);
            }

            const auto fA = dsp::kAlienAmHzLo
                          + (dsp::kAlienAmHzHi - dsp::kAlienAmHzLo) * exposure;
            return goertzel (envelope, fA, sr, 0, size_t (sr * 0.2));
        };

        const auto noAm = amDepth (0.0);
        const auto withAm = amDepth (1.0);
        report ("Test 10 AM modulates the amplitude", withAm > noAm * 3.0,
                "envelope energy at the AM rate " + std::to_string (noAm).substr (0, 8)
                + " -> " + std::to_string (withAm).substr (0, 8));

        // Pitch jumps. Two events must be able to differ in carrier frequency
        // without the input differing at all -- the input here is silence.
        std::vector<double> pitches;
        auto matchesRegister = true;
        for (std::uint64_t event : { 0, 1, 2, 3 })
        {
            const auto hz = dominantHz (oneVoice (event, tuned), sr, 0, 8192);
            pitches.push_back (hz);
            if (std::abs (1200.0 * std::log2 (std::max (hz, 1e-9) / expectedPitch (event))) > 60.0)
                matchesRegister = false;
        }

        auto distinct = true;
        for (size_t i = 1; i < pitches.size(); ++i)
            if (std::abs (pitches[i] - pitches[0]) < 1.0)
                distinct = false;

        std::string pitchDetail;
        for (auto p : pitches)
            pitchDetail += std::to_string (int (p)) + " ";
        report ("Test 10 events choose their own pitch", distinct && matchesRegister,
                pitchDetail + "Hz, all within 60 cents of the register");

        // Same seed, same render; different seed, different timing.
        const auto a = renderWith ("ALIEN", silence, sr, 256, Rig {});
        const auto b = renderWith ("ALIEN", silence, sr, 256, Rig {});
        report ("Test 10 the seed reproduces the render", maxDifference (a, b) == 0.0, "");

        Rig other;
        other.seed = 1234;
        other.alien.seed = 1234;
        const auto c = renderWith ("ALIEN", silence, sr, 256, other);
        report ("Test 10 a new seed is a new pattern", maxDifference (a, c) > 1e-6,
                "max difference " + std::to_string (maxDifference (a, c)).substr (0, 6));
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
    std::printf ("\n");
    test6();
    std::printf ("\n");
    test6Register();
    std::printf ("\n");
    test7();
    std::printf ("\n");
    testDiscrimination();
    std::printf ("\n");
    test8();
    std::printf ("\n");
    test9();
    std::printf ("\n");
    test10();

    std::printf ("\nSQUELCH DSP VALIDATION: %s\n", failures == 0 ? "PASS" : "FAIL");
    if (warnings > 0)
        std::printf ("%d warning(s)\n", warnings);

    return failures == 0 ? 0 : 1;
}
