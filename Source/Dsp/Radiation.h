#pragma once

#include <algorithm>
#include <cmath>

#include "Filters.h"
#include "Rng.h"

/** RADIATION's engine, ported from `prototype/reactor.py`'s `_radiation_engine`.

    A quadrature resonator whose frequency is driven by two correlated AR(1)
    states. dsp-maths.md puts that state in the SAMPLE domain against
    CHEMICAL's event domain: it evolves every sample rather than being held
    across an event, which is why it is advanced inside `process()` here and
    not in a `setEvent()` the way CHEMICAL's register is.

    The resonator is the coupled quadrature form (a rotation, not a
    direct-form recurrence) because the frequency moves rapidly and
    continuously, and a direct form would pump amplitude as its coefficients
    move. There is no saturation in this loop: it is linear and stable by
    r < 1, unlike CHEMICAL's ladder which needs in-loop tanh to self-limit.

    Each stochastic stream hashes (seed, stream, absolute sample index), so
    every sample gets the same value no matter where the host cuts the blocks.
*/
namespace squelch::dsp
{
    inline constexpr double kRadiationTauQS = 0.22;
    inline constexpr double kRadiationTauMS = 0.018;
    inline constexpr double kRadiationDeltaFSemitones = 30.0;
    inline constexpr double kRadiationBetaM = 0.4;
    inline constexpr double kRadiationTauPS = 0.012;
    inline constexpr double kRadiationEpsP = 0.8;
    inline constexpr double kRadiationTauGS = 0.008;
    inline constexpr double kRadiationEpsG = 0.015;
    inline constexpr double kRadiationExcitationTrackS = 0.3;
    inline constexpr double kRadiationExcitationRef = 0.3;

    /** Where the chain's gain staging wants RADIATION to sit.

        While a clamp pinned rDamp at 0.99 for every setting, the energy
        normalisation was a constant that normalised nothing, and the drive
        staging and the limiter's headroom were both set against it. The
        window either side is narrow: below about 1.2 the drive curve is never
        reached and above about 1.5 the limiter stops being a safety net.
    */
    inline constexpr double kRadiationCalibrationGain = 1.3;

    inline constexpr int kRadiationStreamQ = 601;
    inline constexpr int kRadiationStreamM = 602;
    inline constexpr int kRadiationStreamTick = 603;

    struct RadiationProfile
    {
        double baseHz { 220.0 };
        double decayLoS { 0.025 };
        double decayHiS { 0.3 };
    };

    struct RadiationParams
    {
        double volatility { 0.0 };
        double spread { 0.0 };
        double decay { 0.3 };
        double exposure { 0.0 };
        std::uint64_t seed { 0 };
    };

    class RadiationEngine
    {
    public:
        void prepare (double sampleRateIn) noexcept
        {
            sr = sampleRateIn;
            level.prepare (sr, kRadiationExcitationTrackS);
            q = m = tickState = 0.0;
            vReL = vImL = vReR = vImR = 0.0;
            pulse = 0.0;
            index = 0;
        }

        void configure (const RadiationProfile& profile, const RadiationParams& p) noexcept
        {
            baseHz = profile.baseHz;
            seed = p.seed;

            aQ = std::exp (-1.0 / std::max (kRadiationTauQS * sr, 1.0));
            bQ = std::sqrt (1.0 - aQ * aQ) * p.volatility;
            aM = std::exp (-1.0 / std::max (kRadiationTauMS * sr, 1.0));
            bM = std::sqrt (1.0 - aM * aM) * p.volatility;
            aG = std::exp (-1.0 / std::max (kRadiationTauGS * sr, 1.0));

            deltaF = kRadiationDeltaFSemitones * p.spread;

            const auto decayTime = profile.decayLoS
                                 + (profile.decayHiS - profile.decayLoS) * p.decay;
            auto bandwidth = bandwidthForT60 (decayTime) * (1.0 - 0.7 * p.exposure);
            // A safety limit now rather than the operating point: 0.2 Hz is an
            // 11 s t60 and 0.99995 is 3.1 s, and the control range reaches
            // neither. The old 5 Hz floor and 0.99 ceiling between them pinned
            // r flat and left DECAY and EXPOSURE bit-identical throughout.
            bandwidth = std::max (bandwidth, 0.2);
            rDamp = std::clamp (std::exp (-M_PI * bandwidth / sr), 0.0, 0.99995);
            excitationNorm = std::max (std::sqrt (1.0 - rDamp * rDamp), 1e-4)
                           * kRadiationCalibrationGain;

            pulseDecay = std::exp (-1.0 / (kRadiationTauPS * sr));
        }

        /// Events add a percussive pulse rather than gating the resonator.
        void trigger (bool accent) noexcept { pulse += accent ? 1.3 : 1.0; }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            q = bQ * rng::ubipolar ({ seed, kRadiationStreamQ, index }) + aQ * q;
            m = bM * rng::ubipolar ({ seed, kRadiationStreamM, index }) + aM * m;

            const auto fR = std::clamp (
                baseHz * std::pow (2.0, deltaF * (std::tanh (q) + kRadiationBetaM * std::tanh (m)) / 12.0),
                20.0, sr * 0.45);
            const auto omega = 2.0 * M_PI * fR / sr;
            const auto c = std::cos (omega);
            const auto s = std::sin (omega);

            tickState = (1.0 - aG) * rng::ubipolar ({ seed, kRadiationStreamTick, index }) + aG * tickState;

            // running_rms averages POWER across channels, so the level feeds
            // on sqrt of the mean square, not the mean sample.
            const auto tracked = level.process (std::sqrt (0.5 * (xL * xL + xR * xR)));
            const auto exciteGain = std::clamp (tracked / kRadiationExcitationRef, 0.0, 1.0);
            const auto excitation = exciteGain * (kRadiationEpsP * pulse + kRadiationEpsG * tickState);

            // Broadband drive, so the resonator's steady-state gain follows
            // its ENERGY response: sqrt(1 - r^2), not the DC-domain (1 - r).
            const auto eL = (xL + excitation) * excitationNorm;
            const auto eR = (xR + excitation) * excitationNorm;

            const auto newReL = rDamp * (c * vReL - s * vImL) + eL;
            const auto newImL = rDamp * (s * vReL + c * vImL);
            vReL = newReL; vImL = newImL;

            const auto newReR = rDamp * (c * vReR - s * vImR) + eR;
            const auto newImR = rDamp * (s * vReR + c * vImR);
            vReR = newReR; vImR = newImR;

            pulse *= pulseDecay;
            ++index;

            outL = vReL;
            outR = vReR;
        }

        /// The stochastic state itself. Test 7 has to see that this evolves
        /// sample to sample and is correlated, rather than being held between
        /// events the way CHEMICAL's register is.
        double stateValue() const noexcept { return q; }

    private:
        double sr { 44100.0 };
        std::uint64_t seed { 0 }, index { 0 };
        double baseHz { 220.0 }, deltaF { 0.0 };
        double aQ { 0.0 }, bQ { 0.0 }, aM { 0.0 }, bM { 0.0 }, aG { 0.0 };
        double rDamp { 0.0 }, excitationNorm { 1.0 }, pulseDecay { 0.0 };

        double q { 0.0 }, m { 0.0 }, tickState { 0.0 }, pulse { 0.0 };
        double vReL { 0.0 }, vImL { 0.0 }, vReR { 0.0 }, vImR { 0.0 };
        RunningRms level;
    };
}
