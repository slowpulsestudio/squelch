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
