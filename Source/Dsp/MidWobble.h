#pragma once

#include <algorithm>
#include <cmath>

#include "Filters.h"
#include "OutputStage.h"
#include "Rng.h"

/** FALLOUT's pitch half, ported from `output_stage.py`'s `_mid_wobble`.

    Staccato vibrato on the midrange only. Dispersion for reactions that
    should stay put in the stereo field: the material scatters in pitch
    instead of in space. Gated to short bursts on a subset of events so it
    reads as rhythmic rather than as a constant warble.
*/
namespace squelch::dsp
{
    inline constexpr double kWobbleLoHz = 300.0;
    inline constexpr double kWobbleHiHz = 2500.0;
    inline constexpr double kWobbleRateHz = 6.0;
    inline constexpr double kWobbleBurstS = 0.09;
    inline constexpr double kWobbleChance = 0.45;
    inline constexpr int kWobbleStream = 70;

    class MidWobble
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            controlRate = sr / kControlBlock;

            for (auto* b : { &hpL, &hpR, &lpL, &lpR })
                b->reset();

            hpL.setCoefficients (highpass (kWobbleLoHz, 0.7, sr));
            hpR.setCoefficients (highpass (kWobbleLoHz, 0.7, sr));
            lpL.setCoefficients (lowpass (kWobbleHiHz, 0.7, sr));
            lpR.setCoefficients (lowpass (kWobbleHiHz, 0.7, sr));

            windL.prepare (sr);
            windR.prepare (sr);

            gateSmooth = std::exp (-1.0 / std::max (0.004 * controlRate, 1e-6));
            burstBlocks = std::max (static_cast<int> (kWobbleBurstS * controlRate), 1);

            gate = 0.0;
            gateTarget = 0.0;
            remaining = 0;
            phase = 0.0;
            counter = 0;
            eventCount = 0;
        }

        void set (double wobbleAmount) noexcept { amount = wobbleAmount; }

        /// Each event rolls once for whether it opens a burst, so the wobble
        /// lands on a subset of them rather than on all of them.
        void trigger() noexcept
        {
            if (rng::urand ({ 1, kWobbleStream, eventCount }) < kWobbleChance)
                remaining = burstBlocks;
            ++eventCount;
        }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            if (amount <= 0.0)
            {
                outL = xL;
                outR = xR;
                return;
            }

            if (counter == 0)
            {
                gateTarget = remaining > 0 ? 1.0 : 0.0;
                if (remaining > 0)
                    --remaining;
                gate = (1.0 - gateSmooth) * gateTarget + gateSmooth * gate;
                phase += 2.0 * M_PI * kWobbleRateHz / controlRate;
                if (phase >= 2.0 * M_PI)
                    phase -= 2.0 * M_PI;
            }
            counter = (counter + 1 == kControlBlock) ? 0 : (counter + 1);

            const auto lfo = 0.5 + 0.5 * std::sin (phase);
            const auto wind = lfo * gate * amount;

            // Only the midrange moves; the rest passes through untouched, so
            // the effect reads as a wobble in the material rather than a
            // warble over the whole mix.
            const auto midL = lpL.process (hpL.process (xL));
            const auto midR = lpR.process (hpR.process (xR));

            outL = (xL - midL) + windL.process (midL, wind);
            outR = (xR - midR) + windR.process (midR, wind);
        }

    private:
        double sr { 44100.0 }, controlRate { 5512.5 }, amount { 0.0 };
        Biquad hpL, hpR, lpL, lpR;
        PitchWind windL, windR;
        double gateSmooth { 0.0 }, gate { 0.0 }, gateTarget { 0.0 }, phase { 0.0 };
        int burstBlocks { 1 }, remaining { 0 }, counter { 0 };
        std::uint64_t eventCount { 0 };
    };
}
