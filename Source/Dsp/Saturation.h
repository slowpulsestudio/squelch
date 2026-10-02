#pragma once

#include <algorithm>
#include <cmath>

/** Pointwise nonlinearities, ported from prototype/saturation.py term for
    term. These are the curves the oversampler (Oversampler.h) runs at 4x the
    sample rate; they carry no state of their own.
*/
namespace squelch::dsp
{
    //: Below this the curve is linear. tanh starts bending immediately, so
    //: quiet material picks up harmonics it has no business having.
    inline constexpr double kSoftClipKnee = 0.62;

    /// Linear up to the knee, then a smooth approach to the limit.
    inline double softClip (double x, double drive = 1.0) noexcept
    {
        const auto y = x * drive;
        const auto magnitude = std::abs (y);

        if (magnitude <= kSoftClipKnee)
            return y;

        const auto excess = (magnitude - kSoftClipKnee) / (1.0 - kSoftClipKnee);
        const auto sign = y < 0.0 ? -1.0 : 1.0;
        return sign * (kSoftClipKnee + (1.0 - kSoftClipKnee) * std::tanh (excess));
    }

    inline double hardClip (double x, double ceiling = 1.0) noexcept
    {
        return std::clamp (x, -ceiling, ceiling);
    }
}
