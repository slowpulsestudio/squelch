#pragma once

#include <algorithm>
#include <cmath>

/** The four-pole ladder, ported from `prototype/filters.py`'s `ladder`.

    Four one-pole stages with global feedback, saturating inside the loop.
    The saturation has to stay inside: there it grows with the resonance so
    the filter limits its own ring, where after the filter it is only
    distortion on top. Feedback reaches self-oscillation at 4.

    Summing different poles gives different shapes from the one structure,
    which is what the tap weights select: (0,0,0,1) is a 24 dB lowpass,
    (0,-1,0,1) a bandpass, (1,-2,0,1) a highpass.
*/
namespace squelch::dsp
{
    struct LadderTap
    {
        double a { 0.0 }, b { 0.0 }, c { 0.0 }, d { 1.0 };
    };

    class Ladder
    {
    public:
        void prepare (double sampleRateIn) noexcept
        {
            sr = sampleRateIn;
            reset();
        }

        void reset() noexcept { s1 = s2 = s3 = s4 = 0.0; }

        void setTap (const LadderTap& t) noexcept { tap = t; }

        double process (double x, double cutoffHz, double feedback, double drive) noexcept
        {
            const auto g = 1.0 - std::exp (-2.0 * M_PI * std::clamp (cutoffHz, 20.0, sr * 0.45) / sr);
            const auto k = std::clamp (feedback, 0.0, 3.97);

            const auto u = std::tanh ((x - k * s4) * drive) / drive;
            s1 += g * (u - s1);
            s2 += g * (s1 - s2);
            s3 += g * (s2 - s3);
            s4 += g * (s3 - s4);

            return tap.a * s1 + tap.b * s2 + tap.c * s3 + tap.d * s4;
        }

    private:
        double sr { 44100.0 };
        double s1 { 0.0 }, s2 { 0.0 }, s3 { 0.0 }, s4 { 0.0 };
        LadderTap tap;
    };
}
