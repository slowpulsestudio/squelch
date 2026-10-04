#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "Filters.h"
#include "Oversampler.h"

/** SLUDGE's engine, ported from `prototype/reactor.py`'s `_sludge_engine`.

    No event sweep and no ladder: the reaction tracks its own slow/fast
    envelope followers off the input, generates the /2 and /4 subharmonics
    from its own reactor frequency (not pitch-tracked), and runs the
    asymmetric-saturated result through a memory-driven lowpass whose cutoff
    genuinely varies every sample. `configure()` recomputes the per-render
    constants (profile + params only change at control rate); `process()` is
    the per-sample audio-thread path and allocates nothing.

    `prototype/reactor.py` mixes the oversampled/saturated path with the raw
    subharmonic oscillator `h` at the same sample index, which only works in
    the prototype because `saturation.oversampled` is acausal (scipy's
    zero-phase resample_poly, reading ahead over the whole render) and so
    secretly carries no delay relative to `h`. A real-time port cannot do
    that: Oversampler.h is causal and genuinely lags by
    `kOversamplerLatencySamples`. Mixing an undelayed `h` straight into a
    lagged `s` would comb-filter the two paths 20 samples out of phase, so
    this port delays `h` by the same amount before the final mix, and the
    whole engine's output is `kOversamplerLatencySamples` behind its input --
    report that via `setLatencySamples`, same as the oversampler itself.

    One difference from the prototype is inherent rather than a porting bug.
    The offline reference re-pads between its upsample and downsample stages,
    where a continuous causal stream cannot, so the two disagree over the
    oversampler's first 41 samples. That region then feeds the final smoothing
    one-pole, which is IIR, so it leaves an exponentially decaying tail rather
    than ending cleanly: measured at 1.5e-4 peak (-76 dB) at stream start,
    e-folding every 1/g_c samples and at machine precision within a few
    hundred. Bounded, inaudible, and only from a cold start, but real -- and
    it is why the comparison skips a settling window.
*/
namespace squelch::dsp
{
    /// The subset of ReactionProfile that SLUDGE's engine reads.
    struct SludgeProfile
    {
        double baseHz { 100.0 };
        double cutoffLoHz { 150.0 };
        double cutoffHiHz { 1300.0 };
        double decayLoS { 0.25 };
        double decayHiS { 1.60 };
    };

    /// The subset of Params that SLUDGE's engine reads, all 0..1.
    struct SludgeParams
    {
        double decay { 0.3 };
        double halfLife { 0.4 };
        double spread { 0.75 };
        double reactivity { 0.0 };
        double exposure { 0.6 };
        double toxicity { 0.5 };
    };

    class SludgeEngine
    {
    public:
        void prepare (double sampleRateIn) noexcept
        {
            sr = sampleRateIn;
            qFilter.reset();
            rFilter.reset();
            mFilter.reset();
            smoothL.reset();
            smoothR.reset();
            oversamplerL.reset();
            oversamplerR.reset();
            theta = 0.0;
            hDelay.reset();
            gCDelay.reset();
        }

        void configure (const SludgeProfile& profile, const SludgeParams& p) noexcept
        {
            baseHz = profile.baseHz;

            const auto decayTime = profile.decayLoS
                                  + (profile.decayHiS - profile.decayLoS) * p.decay;
            const auto fR = kFRLoHz + (kFRHiHz - kFRLoHz) * (decayTime / profile.decayHiS);
            gQ = 1.0 - std::exp (-2.0 * M_PI * kFQHz / sr);
            gR = 1.0 - std::exp (-2.0 * M_PI * fR / sr);

            const auto tauM = kTauMLoS + (kTauMHiS - kTauMLoS) * p.halfLife;
            const auto aM = std::exp (-1.0 / std::max (tauM * sr, 1.0));
            gM = 1.0 - aM;

            const auto spanOct = std::log2 (profile.cutoffHiHz / profile.cutoffLoHz);
            fBaseOct = std::log2 (profile.cutoffLoHz) + spanOct * 0.5;
            deltaF = spanOct * 0.5 * p.spread;
            deltaSnap = 0.4 * p.reactivity;

            betaM = 0.3 + 0.9 * p.exposure;
            betaH = 0.2 + 0.6 * p.exposure;
            exposureTerm = 0.5 + 1.0 * p.exposure;
            deltaH = 0.5 * p.spread;
            delta = 1.0 + 3.0 * p.toxicity;
            // TOXICITY sweeps the asymmetry too, not just the drive. Drive
            // alone saturates the even orders while the odd ones keep
            // climbing, which ran the control backwards.
            gamma = kGammaLo + (kGammaHi - kGammaLo) * p.toxicity;
            muH = 0.3 + 0.5 * p.reactivity;
        }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            const auto e = std::max (std::abs (xL), std::abs (xR));

            const auto q = qFilter.process (e, gQ);
            // Off e, not off q: r is q's sibling, not its child. As a cascade
            // r could only lag q, so q - r sat at the noise floor and DECAY,
            // which reaches the output through this term alone, did nothing.
            const auto r = rFilter.process (e, gR);
            // A fraction of the body, not a raw difference: deltaSnap is an
            // exponent in octaves, so this has to be O(1) to mean anything.
            const auto sSnap = std::clamp ((q - r) / std::max (q, kSnapFloor), -1.0, 1.0);
            snap = sSnap;

            const auto m = mFilter.process (q, gM);
            memory = m;
            // Against a reference level, because tanh needs an O(1) argument:
            // q is a lowpassed max|x| and never left the linear region.
            const auto mB = std::tanh (m / kMemoryRef);
            memoryReach = mB;

            const auto fC = std::pow (2.0, fBaseOct + deltaF * mB);
            const auto fEffective = std::clamp (fC * std::pow (2.0, deltaSnap * sSnap),
                                                20.0, sr * 0.45);

            const auto fH = baseHz * std::pow (2.0, deltaH * mB);
            theta += 2.0 * M_PI * fH / sr;
            if (theta >= 8.0 * M_PI)
                theta = std::fmod (theta, 8.0 * M_PI);

            const auto theta4 = theta >= 4.0 * M_PI ? theta - 4.0 * M_PI : theta;
            const auto h1 = std::sin (theta4 / 2.0);
            const auto h2 = std::sin (theta / 4.0);
            const auto aH = 0.5 + exposureTerm * std::tanh (q);
            const auto h = aH * (kW1 * h1 + kW2 * h2);

            const auto zL = xL + betaM * m + betaH * h;
            const auto zR = xR + betaM * m + betaH * h;

            const auto curve = [this] (double u) noexcept
            {
                const auto t = std::tanh (delta * u);
                return u >= 0.0 ? t : gamma * t;
            };

            const auto satL = oversamplerL.process (zL, curve);
            const auto satR = oversamplerR.process (zR, curve);

            // gC is computed from f_effective at the current sample, but
            // satL/satR are kOversamplerLatencySamples behind it (the
            // oversampler's own causal lag) -- delay gC to match, or the
            // one-pole below would filter a lagged signal with a
            // not-yet-lagged coefficient and drift from the prototype's
            // single, consistently-indexed recurrence.
            const auto gC = gCDelay.push (1.0 - std::exp (-2.0 * M_PI * fEffective / sr));
            const auto sL = smoothL.process (satL, gC);
            const auto sR = smoothR.process (satR, gC);

            // h feeds into z above with no delay of its own (it rides along
            // with the oversampler's natural lag there), but mixed in
            // directly here it would arrive kOversamplerLatencySamples ahead
            // of s -- delay it to match.
            const auto hDelayed = hDelay.push (h);

            outL = (1.0 - muH) * sL + muH * hDelayed;
            outR = (1.0 - muH) * sR + muH * hDelayed;
        }

        /// The long memory itself. Test 9 asks for the persistence of the body
        /// state after the input stops, and once there is no input the output
        /// is the reactor oscillator alone, so the state cannot be read back
        /// out of the audio.
        double memoryValue() const noexcept { return memory; }

        /// The saturated memory, which is what the cutoff actually follows.
        /// Recomputing tanh(M) in a test would not notice the engine dropping
        /// its reference and sitting in the linear region again.
        double memoryReachValue() const noexcept { return memoryReach; }

        /// The snap. It collapses if its two followers are made a cascade
        /// again, and that change is under 0.01 dB at the output, so nothing
        /// but the state itself catches it.
        double snapValue() const noexcept { return snap; }

    private:
        static constexpr double kFQHz = 5.0;
        static constexpr double kFRLoHz = 45.0;
        static constexpr double kFRHiHz = 12.0;
        static constexpr double kSnapFloor = 1e-3;
        static constexpr double kMemoryRef = 0.25;
        static constexpr double kTauMLoS = 0.3;        static constexpr double kTauMHiS = 5.0;
        static constexpr double kGammaLo = 0.9;
        static constexpr double kGammaHi = 0.1;
        static constexpr double kW1 = 0.65;
        static constexpr double kW2 = 0.45;

        double sr { 44100.0 };

        // Per-render constants, recomputed by configure().
        double baseHz { 100.0 };
        double gQ { 0.0 }, gR { 0.0 }, gM { 0.0 };
        double fBaseOct { 0.0 }, deltaF { 0.0 }, deltaSnap { 0.0 };
        double betaM { 0.0 }, betaH { 0.0 }, exposureTerm { 0.0 };
        double deltaH { 0.0 }, delta { 1.0 }, gamma { 0.9 }, muH { 0.0 };

        // Per-sample state.
        ZeroStateOnePole qFilter, rFilter, mFilter, smoothL, smoothR;
        Oversampler oversamplerL, oversamplerR;
        double theta { 0.0 }, memory { 0.0 }, snap { 0.0 }, memoryReach { 0.0 };

        /// A fixed-length delay line, used to keep a causally-computed
        /// signal or coefficient in step with the oversampler's lagged path.
        class FixedDelay
        {
        public:
            void reset() noexcept { line.fill (0.0); pos = 0; }

            double push (double x) noexcept
            {
                const auto delayed = line[pos];
                line[pos] = x;
                pos = (pos + 1 == kOversamplerLatencySamples) ? 0 : (pos + 1);
                return delayed;
            }

        private:
            std::array<double, kOversamplerLatencySamples> line {};
            std::size_t pos { 0 };
        };

        FixedDelay hDelay, gCDelay;
    };
}
