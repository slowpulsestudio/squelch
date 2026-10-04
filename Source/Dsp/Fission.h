#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "Filters.h"
#include "Rng.h"

/** FISSION's engine, ported from `prototype/reactor.py`'s `_fission_engine`.

    Two coupled branch resonators per channel, not one filter with a detuned
    coefficient. Each pair is detuned apart by a slowly wandering amount,
    cross-fed into each other, and recombined with the second branch read back
    through a moving fractional delay. Left and right mirror their detuning and
    delay (d_L = d, d_R = -d; D_L = D0 + Dm*m, D_R = D0 - Dm*m), so the image
    moves because the branch relationship diverges rather than from a pan.

    The cross-feedback is a FRACTION of the pair's own stability headroom, not
    an absolute gain. In sum/difference coordinates the coupled pair decouples
    into two sections with poles at radius r only while they stay complex;
    with both branches close in frequency that headroom is tiny, so an
    absolute k_c sized to be audible would simply saturate against the ceiling
    regardless of EXPOSURE.
*/
namespace squelch::dsp
{
    inline constexpr double kFissionTauMS = 0.9;
    inline constexpr double kFissionTauDS = 1.3;
    inline constexpr double kFissionDeltaFSemitones = 9.0;
    inline constexpr double kFissionD0 = 0.35;
    inline constexpr double kFissionDm = 0.65;
    inline constexpr double kFissionD0DelayS = 0.0025;
    inline constexpr double kFissionDmDelayS = 0.0035;
    inline constexpr double kFissionKcMinFrac = 0.1;
    inline constexpr double kFissionKcNormFrac = 0.85;

    inline constexpr int kFissionStreamM = 701;
    inline constexpr int kFissionStreamD = 702;

    /// Worst case of (D0 + Dm) * sr at 96 kHz, rounded up, so the line is
    /// long enough at every supported rate without allocating on the fly.
    inline constexpr int kFissionMaxDelay = 640;

    struct FissionProfile
    {
        double baseHz { 320.0 };
        double decayLoS { 0.08 };
        double decayHiS { 0.6 };
    };

    struct FissionParams
    {
        double spread { 0.0 };
        double decay { 0.3 };
        double exposure { 0.0 };
        std::uint64_t seed { 0 };
    };

    namespace detail
    {
        /// One channel's coupled pair: two resonators cross-feeding, with v2
        /// read back through a moving fractional delay.
        class BranchPair
        {
        public:
            void reset() noexcept
            {
                v1m1 = v1m2 = v2m1 = v2m2 = 0.0;
                line.fill (0.0);
                pos = 0;
            }

            double process (double x, double d, double delaySamples, double r,
                            double coupling, double baseHz, double sr) noexcept
            {
                const auto omega1 = 2.0 * M_PI * baseHz * std::pow (2.0, d / 12.0) / sr;
                const auto omega2 = 2.0 * M_PI * baseHz * std::pow (2.0, -d / 12.0) / sr;
                const auto c1 = std::cos (omega1);
                const auto c2 = std::cos (omega2);

                const auto kcLimit = 0.8 * 2.0 * r * (1.0 - std::max (c1, c2));
                const auto kc = std::clamp (coupling, 0.0, 0.95) * kcLimit;

                // Sustained-tone normalisation: this pair is driven at its own
                // resonance, so the DC-domain (1 - r) is right here, unlike
                // RADIATION's broadband drive which needs sqrt(1 - r^2).
                const auto excitation = x * std::max (1.0 - r, 1e-4);

                const auto newV1 = 2.0 * r * c1 * v1m1 - r * r * v1m2 + (excitation - kc * v2m1);
                const auto newV2 = 2.0 * r * c2 * v2m1 - r * r * v2m2 + (excitation - kc * v1m1);

                v1m2 = v1m1; v1m1 = newV1;
                v2m2 = v2m1; v2m1 = newV2;

                line[pos] = newV2;
                const auto delayed = readDelayed (delaySamples);
                pos = (pos + 1 == kFissionMaxDelay) ? 0 : (pos + 1);

                return 0.5 * newV1 + 0.5 * delayed;
            }

        private:
            /// Linear interpolation between the two neighbouring taps, which
            /// is what np.interp does on the prototype's whole-array read.
            double readDelayed (double delaySamples) const noexcept
            {
                const auto clamped = std::clamp (delaySamples, 0.0,
                                                 double (kFissionMaxDelay - 2));
                const auto whole = static_cast<int> (clamped);
                const auto frac = clamped - whole;

                const auto idx0 = (pos - whole + kFissionMaxDelay) % kFissionMaxDelay;
                const auto idx1 = (idx0 - 1 + kFissionMaxDelay) % kFissionMaxDelay;

                return line[idx0] + (line[idx1] - line[idx0]) * frac;
            }

            double v1m1 { 0.0 }, v1m2 { 0.0 }, v2m1 { 0.0 }, v2m2 { 0.0 };
            std::array<double, kFissionMaxDelay> line {};
            int pos { 0 };
        };
    }

    class FissionEngine
    {
    public:
        void prepare (double sampleRateIn) noexcept
        {
            sr = sampleRateIn;
            left.reset();
            right.reset();
            m = mD = 0.0;
            index = 0;
        }

        void configure (const FissionProfile& profile, const FissionParams& p) noexcept
        {
            baseHz = profile.baseHz;
            seed = p.seed;

            aM = std::exp (-1.0 / std::max (kFissionTauMS * sr, 1.0));
            aD = std::exp (-1.0 / std::max (kFissionTauDS * sr, 1.0));
            // Energy-normalised, not DC-normalised: (1-a) is for a filter that
            // must pass a constant unchanged, and these states ARE the signal.
            bM = std::sqrt (1.0 - aM * aM);
            bD = std::sqrt (1.0 - aD * aD);

            deltaF = kFissionDeltaFSemitones * p.spread;
            delayBase = kFissionD0DelayS * sr;
            delayMod = kFissionDmDelayS * p.spread * sr;

            const auto decayTime = profile.decayLoS
                                 + (profile.decayHiS - profile.decayLoS) * p.decay;
            // Same correction as RADIATION's, and FISSION had it worse: every
            // setting of its 80 ms to 600 ms range asked for a bandwidth under
            // the old 5 Hz floor, so DECAY there did nothing at all.
            const auto bandwidth = std::max (bandwidthForT60 (decayTime), 0.2);
            r = std::clamp (std::exp (-M_PI * bandwidth / sr), 0.0, 0.99995);

            coupling = kFissionKcMinFrac + kFissionKcNormFrac * p.exposure;
        }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            m = bM * rng::ubipolar ({ seed, kFissionStreamM, index }) + aM * m;
            mD = bD * rng::ubipolar ({ seed, kFissionStreamD, index }) + aD * mD;
            ++index;

            const auto d = deltaF * (kFissionD0 + kFissionDm * m);
            const auto delayL = delayBase + delayMod * mD;
            const auto delayR = delayBase - delayMod * mD;

            outL = left.process (xL, d, delayL, r, coupling, baseHz, sr);
            outR = right.process (xR, -d, delayR, r, coupling, baseHz, sr);
        }

    private:
        double sr { 44100.0 };
        std::uint64_t seed { 0 }, index { 0 };
        double baseHz { 320.0 }, deltaF { 0.0 };
        double aM { 0.0 }, aD { 0.0 }, bM { 0.0 }, bD { 0.0 }, m { 0.0 }, mD { 0.0 };
        double delayBase { 0.0 }, delayMod { 0.0 }, r { 0.0 }, coupling { 0.0 };

        detail::BranchPair left, right;
    };
}
