#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "Filters.h"

/** The output stage, ported from `prototype/output_stage.py`.

    Only the two stages that stand alone are here so far: the house voicing
    and the peak limiter. The rest of the chain (drive, collimate, fallout,
    unity_match) still has to come across.
*/
namespace squelch::dsp
{
    inline constexpr double kVoiceLowHz = 70.0;
    inline constexpr double kVoiceLowDb = 2.5;
    inline constexpr double kVoiceDipHz = 260.0;
    inline constexpr double kVoiceDipDb = -1.8;
    inline constexpr double kVoiceDipQ = 0.9;
    inline constexpr double kVoiceDeharshHz = 6000.0;
    inline constexpr double kVoiceDeharshDb = -3.5;
    inline constexpr double kVoiceDeharshQ = 0.75;
    inline constexpr double kVoiceAirHz = 15000.0;
    inline constexpr double kVoiceAirDb = 1.0;

    inline constexpr double kLimiterLookaheadS = 0.005;
    inline constexpr double kLimiterCeiling = 0.97;
    inline constexpr double kLimiterReleaseS = 0.050;

    inline constexpr double kCollimatorCentreHz = 650.0;

    inline constexpr double kPeakTarget = 0.89;
    inline constexpr double kPeakTrackS = 1.2;
    inline constexpr double kPeakAttackS = 0.6;
    inline constexpr double kLevelMatchRangeDb = 36.0;
    inline constexpr double kLevelMatchGateDb = -60.0;
    inline constexpr double kGainSmoothS = 0.05;

    /** Closes a high-pass and a low-pass in on kCollimatorCentreHz.

        Coefficients only move when COLLIMATOR does, so they are rebuilt in
        `set()` rather than per sample.
    */
    class Collimator
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            hp.reset();
            lp.reset();
            set (0.0);
        }

        void set (double collimator) noexcept
        {
            amount = collimator;
            if (amount <= 0.0)
                return;

            constexpr auto hpOpen = 20.0;
            constexpr auto lpOpen = 18000.0;
            const auto hpHz = hpOpen * std::pow (kCollimatorCentreHz * 0.62 / hpOpen, amount);
            const auto lpHz = lpOpen * std::pow (kCollimatorCentreHz * 2.2 / lpOpen, amount);
            const auto q = 0.707 + 0.5 * amount;

            hp.setCoefficients (highpass (hpHz, q, sr));
            lp.setCoefficients (lowpass (lpHz, q, sr));
        }

        double process (double x) noexcept
        {
            return amount <= 0.0 ? x : lp.process (hp.process (x));
        }

    private:
        double sr { 44100.0 }, amount { 0.0 };
        Biquad hp, lp;
    };

    /** Aims the peaks just under the ceiling, in both directions.

        Slow enough to be a level and not an envelope. It lifts quiet material
        as well as holding loud material down: a reaction that rings less is
        not meant to be quieter, it is meant to be a different sound at the
        same level.

        The gate and hold in the prototype's `matching_gain` are a no-op here
        because the target is the constant kPeakTarget, which is always above
        the gate, so only the clamp and the smoothing carry over.
    */
    class UnityMatch
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            attack = std::exp (-1.0 / std::max (kPeakAttackS * sr, 1.0));
            release = std::exp (-1.0 / std::max (kPeakTrackS * sr, 1.0));
            smooth = std::exp (-1.0 / std::max (kGainSmoothS * sr, 1.0));
            ceiling = std::pow (10.0, kLevelMatchRangeDb / 20.0);
            held = 0.0;
            // Unity, not the first computed gain: at sample zero the trackers
            // have seen one sample each and their ratio is noise.
            gain = 1.0;
        }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            const auto magnitude = std::max (std::abs (xL), std::abs (xR));
            const auto coeff = magnitude > held ? attack : release;
            held = magnitude + (held - magnitude) * coeff;

            const auto wanted = std::clamp (kPeakTarget / std::max (held, 1e-12),
                                            1.0 / ceiling, ceiling);
            gain = (1.0 - smooth) * wanted + smooth * gain;

            outL = xL * gain;
            outR = xR * gain;
        }

    private:
        double sr { 44100.0 };
        double attack { 0.0 }, release { 0.0 }, smooth { 0.0 }, ceiling { 1.0 };
        double held { 0.0 }, gain { 1.0 };
    };

    inline int lookaheadSamples (double sr) noexcept
    {
        return std::max (static_cast<int> (kLimiterLookaheadS * sr), 1);
    }

    /** Fixed Pultec-style house voicing.

        The low shelf and the dip above it are the Pultec boost-and-attenuate
        trick. The de-harsh stage is a broad bell rather than a shelf because
        a shelf from 5 kHz also pulls 15 kHz down, which removes harshness by
        dulling the whole top instead of scooping the part that is harsh.
    */
    class Voice
    {
    public:
        void prepare (double sr) noexcept
        {
            for (auto* c : { &low, &dip, &deharsh, &air })
                c->reset();

            low.setCoefficients (lowShelf (kVoiceLowHz, kVoiceLowDb, sr));
            dip.setCoefficients (peaking (kVoiceDipHz, kVoiceDipDb, kVoiceDipQ, sr));
            deharsh.setCoefficients (peaking (kVoiceDeharshHz, kVoiceDeharshDb, kVoiceDeharshQ, sr));
            air.setCoefficients (highShelf (kVoiceAirHz, kVoiceAirDb, sr));
        }

        double process (double x) noexcept
        {
            return air.process (deharsh.process (dip.process (low.process (x))));
        }

    private:
        Biquad low, dip, deharsh, air;
    };

    /** Lookahead peak limiter.

        Gain comes from a rolling max that looks forward, applied to delayed
        audio, so a single-sample transient is caught rather than slipping
        past ahead of the envelope. Gain falls instantly and recovers over
        kLimiterReleaseS: with no release it snapped back at over 4000 dB/s,
        which is a distortion in itself.

        The delay here is TWICE `lookaheadSamples`, and that is not a choice.
        The prototype centres its rolling max on the current input while
        delaying the audio by only one window, so the gain envelope leads the
        sample it scales by 2w: measured, a spike at 1500 starts ducking the
        output carrying input 1060. Reproducing that causally needs 2w of
        delay. `lookaheadSamples` therefore reports half what a real-time
        limiter actually costs, and the figure the host is told must be 2w.
    */
    class PeakLimiter
    {
    public:
        /// What the host has to be told, which is not kLimiterLookaheadS.
        static int latencySamples (double sr) noexcept { return 2 * lookaheadSamples (sr); }

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            window = lookaheadSamples (sr);
            span = 2 * window + 1;
            delayLength = 2 * window;

            magnitude.assign (static_cast<size_t> (span), 0.0);
            delayL.assign (static_cast<size_t> (delayLength), 0.0);
            delayR.assign (static_cast<size_t> (delayLength), 0.0);
            pos = 0;
            delayPos = 0;
            held = 1.0;
            release = std::exp (-1.0 / std::max (kLimiterReleaseS * sr, 1.0));
        }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            magnitude[static_cast<size_t> (pos)] = std::max (std::abs (xL), std::abs (xR));
            pos = (pos + 1 == span) ? 0 : (pos + 1);

            auto rolling = 0.0;
            for (auto v : magnitude)
                rolling = std::max (rolling, v);

            const auto want = std::min (1.0, kLimiterCeiling / std::max (rolling, 1e-9));
            held = want < held ? want : want + (held - want) * release;

            const auto dL = delayL[static_cast<size_t> (delayPos)];
            const auto dR = delayR[static_cast<size_t> (delayPos)];
            delayL[static_cast<size_t> (delayPos)] = xL;
            delayR[static_cast<size_t> (delayPos)] = xR;
            delayPos = (delayPos + 1 == delayLength) ? 0 : (delayPos + 1);

            outL = dL * held;
            outR = dR * held;
        }

    private:
        double sr { 44100.0 };
        int window { 220 }, span { 441 }, delayLength { 440 }, pos { 0 }, delayPos { 0 };
        double held { 1.0 }, release { 0.0 };
        std::vector<double> magnitude, delayL, delayR;
    };
}
