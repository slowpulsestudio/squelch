#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

/** The filter primitives, ported from prototype/filters.py.

    Coefficients are RBJ cookbook and match the prototype term for term. Each
    one is a direct-form-I biquad carrying its own state, so it runs sample by
    sample on the audio thread instead of over a whole array.
*/
namespace squelch::dsp
{
    struct BiquadCoefficients
    {
        double b0 { 1.0 }, b1 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
    };

    class Biquad
    {
    public:
        void reset() noexcept { x1 = x2 = y1 = y2 = 0.0; }

        void setCoefficients (const BiquadCoefficients& next) noexcept { c = next; }

        double process (double x) noexcept
        {
            const auto y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }

    private:
        BiquadCoefficients c;
        double x1 { 0.0 }, x2 { 0.0 }, y1 { 0.0 }, y2 { 0.0 };
    };

    namespace detail
    {
        inline double clampCutoff (double fc, double sr) noexcept
        {
            return std::clamp (fc, 20.0, sr * 0.45);
        }

        struct Terms { double w0, cosW0, alpha; };

        inline Terms terms (double fc, double q, double sr) noexcept
        {
            const auto w0 = 2.0 * M_PI * clampCutoff (fc, sr) / sr;
            return { w0, std::cos (w0), std::sin (w0) / (2.0 * std::max (q, 0.3)) };
        }

        inline BiquadCoefficients normalise (double b0, double b1, double b2,
                                             double a0, double a1, double a2) noexcept
        {
            return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        }
    }

    inline BiquadCoefficients lowpass (double fc, double q, double sr) noexcept
    {
        const auto t = detail::terms (fc, q, sr);
        return detail::normalise ((1.0 - t.cosW0) / 2.0, 1.0 - t.cosW0, (1.0 - t.cosW0) / 2.0,
                                  1.0 + t.alpha, -2.0 * t.cosW0, 1.0 - t.alpha);
    }

    inline BiquadCoefficients highpass (double fc, double q, double sr) noexcept
    {
        const auto t = detail::terms (fc, q, sr);
        return detail::normalise ((1.0 + t.cosW0) / 2.0, -(1.0 + t.cosW0), (1.0 + t.cosW0) / 2.0,
                                  1.0 + t.alpha, -2.0 * t.cosW0, 1.0 - t.alpha);
    }

    inline BiquadCoefficients notch (double fc, double q, double sr) noexcept
    {
        const auto t = detail::terms (fc, q, sr);
        return detail::normalise (1.0, -2.0 * t.cosW0, 1.0,
                                  1.0 + t.alpha, -2.0 * t.cosW0, 1.0 - t.alpha);
    }

    inline BiquadCoefficients peaking (double fc, double gainDb, double q, double sr) noexcept
    {
        const auto t = detail::terms (fc, q, sr);
        const auto amp = std::pow (10.0, gainDb / 40.0);
        return detail::normalise (1.0 + t.alpha * amp, -2.0 * t.cosW0, 1.0 - t.alpha * amp,
                                  1.0 + t.alpha / amp, -2.0 * t.cosW0, 1.0 - t.alpha / amp);
    }

    namespace detail
    {
        struct ShelfTerms { double amp, cosW0, beta; };

        inline ShelfTerms shelfTerms (double fc, double gainDb, double sr) noexcept
        {
            const auto amp = std::pow (10.0, gainDb / 40.0);
            const auto w0 = 2.0 * M_PI * clampCutoff (fc, sr) / sr;
            const auto alpha = std::sin (w0) / 2.0 * std::sqrt (2.0);
            return { amp, std::cos (w0), 2.0 * std::sqrt (amp) * alpha };
        }
    }

    inline BiquadCoefficients lowShelf (double fc, double gainDb, double sr) noexcept
    {
        const auto [amp, cosW0, beta] = detail::shelfTerms (fc, gainDb, sr);
        return detail::normalise (
            amp * ((amp + 1.0) - (amp - 1.0) * cosW0 + beta),
            2.0 * amp * ((amp - 1.0) - (amp + 1.0) * cosW0),
            amp * ((amp + 1.0) - (amp - 1.0) * cosW0 - beta),
            (amp + 1.0) + (amp - 1.0) * cosW0 + beta,
            -2.0 * ((amp - 1.0) + (amp + 1.0) * cosW0),
            (amp + 1.0) + (amp - 1.0) * cosW0 - beta);
    }

    inline BiquadCoefficients highShelf (double fc, double gainDb, double sr) noexcept
    {
        const auto [amp, cosW0, beta] = detail::shelfTerms (fc, gainDb, sr);
        return detail::normalise (
            amp * ((amp + 1.0) + (amp - 1.0) * cosW0 + beta),
            -2.0 * amp * ((amp - 1.0) + (amp + 1.0) * cosW0),
            amp * ((amp + 1.0) + (amp - 1.0) * cosW0 - beta),
            (amp + 1.0) - (amp - 1.0) * cosW0 + beta,
            2.0 * ((amp - 1.0) - (amp + 1.0) * cosW0),
            (amp + 1.0) - (amp - 1.0) * cosW0 - beta);
    }

    /** Level as the plugin measures it: a one-pole on power, looking back only.

        Replaces every place the prototype would have taken the RMS of a whole
        render.

        Starting at zero would ramp the level up over the first second and a
        half. Priming from the opening of the file reads ahead, and priming
        from the first block makes the answer depend on the host's buffer size.
        So it averages everything heard so far until the window is full, then
        carries on exponentially.
    */
    class RunningRms
    {
    public:
        void prepare (double sampleRate, double timeSeconds) noexcept
        {
            coeff = std::exp (-1.0 / std::max (timeSeconds * sampleRate, 1.0));
            power = 0.0;
            count = 0;
        }

        double process (double sample) noexcept
        {
            ++count;

            // Equal weights until the exponential window is full, so the
            // estimate is a plain average of what has been heard so far.
            const auto alpha = std::max (1.0 - coeff, 1.0 / static_cast<double> (count));
            power += (sample * sample - power) * alpha;

            return std::sqrt (std::max (power, 1e-20));
        }

    private:
        double coeff { 0.0 };
        double power { 0.0 };
        std::uint64_t count { 0 };
    };

    /// One-pole smoothing, started at its first value rather than from zero.
    class OnePole
    {
    public:
        void prepare (double sampleRate, double timeSeconds) noexcept
        {
            coeff = timeSeconds <= 0.0
                      ? 0.0
                      : std::exp (-1.0 / std::max (timeSeconds * sampleRate, 1e-6));
            primed = false;
        }

        double process (double target) noexcept
        {
            if (! primed)
            {
                value = target;
                primed = true;
                return value;
            }

            value = target + (value - target) * coeff;
            return value;
        }

    private:
        double coeff { 0.0 };
        double value { 0.0 };
        bool primed { false };
    };

    /** One-pole lowpass with zero initial state, matching
        `scipy.signal.lfilter`'s default `zi`.

        This is `prototype/reactor.py`'s `_one_pole_hz` and the memory stage's
        `lfilter([1 - a], [1, -a], ...)`, both of which start their state at
        zero rather than priming to the first sample like `OnePole` above.
        The gain `g` is passed per call rather than fixed at `prepare()`-time
        because SLUDGE's final smoothing stage recomputes it every sample from
        a time-varying cutoff.
    */
    class ZeroStateOnePole
    {
    public:
        void reset() noexcept { state = 0.0; }

        /// g = 1 - e^(-2*pi*f_hz/sr) for a cutoff in Hz, or (1 - a) for a
        /// direct pole `a` such as `exp(-1/(tau*sr))`.
        double process (double x, double g) noexcept
        {
            state += g * (x - state);
            return state;
        }

    private:
        double state { 0.0 };
    };
}
