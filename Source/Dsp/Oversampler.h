#pragma once

#include <array>
#include <cstddef>

/** Causal, real-time equivalent of prototype/saturation.oversampled.

    prototype/saturation.py runs a nonlinearity at 4x the sample rate via
    scipy.signal.resample_poly, which is a ZERO-PHASE (acausal) filter: it
    reads the whole render and cancels the anti-aliasing filters' group delay
    by slicing samples that are, relative to a causal process, from the
    future. A real plugin cannot do that. This header is the causal
    equivalent: the same upsample-FIR / curve / downsample-FIR pipeline, run
    sample by sample with ordinary delay-line state, producing output that is
    identical to the Python reference's but delayed by kOversamplerLatencySamples
    samples at the base rate — the group delay is reported to the host via
    AudioProcessor::setLatencySamples, not silently erased.

    The two FIR filters are literally the same 81-tap Kaiser(8.0) lowpass
    scipy.signal.firwin(81, 0.25, window=('kaiser', 8.0)) designs for
    up=4/down=1, scaled by the upsample factor on the way up and left unity
    on the way down (resample_poly's own `h *= up`). Both FACTOR and WINDOW
    are fixed architecture constants in saturation.py, not user-facing
    controls, which is what makes hardcoding the designed taps here safe:
    they never need to be redesigned at a different ratio.

    Verified against prototype/saturation.oversampled: the causal pipeline's
    output sample i + kOversamplerLatencySamples equals the acausal
    reference's sample i to machine precision, for both an identity curve and
    the soft-clip/tanh curves actually used (see prototype/compare.py's
    oversampler expectation).
*/
namespace squelch::dsp
{
    inline constexpr int kOversampleFactor = 4;
    inline constexpr int kOversampleFilterTaps = 81;

    //: scipy.signal.firwin(81, 0.25, window=('kaiser', 8.0)). The cutoff 0.25
    //: is 1/kOversampleFactor, i.e. the original Nyquist expressed relative to
    //: the oversampled rate's Nyquist -- see prototype/saturation.oversampled's
    //: use of resample_poly(..., 4, 1, window=WINDOW).
    inline constexpr std::array<double, kOversampleFilterTaps> kOversampleFilter {
        -2.279337438576827e-20,  -2.6462262345687998e-05, -6.435528108851458e-05,
        -7.171909618134312e-05,  1.6636269015907442e-19,  0.00015238102783352264,
        0.0002980248128025553,   0.0002840891246441517,   -5.195448361255068e-19,
        -0.0004861931180949965,  -0.0008779146646607554,  -0.000782060317801968,
        1.1803903744786633e-18,  0.0011994631388749504,   0.002070080161846552,
        0.0017711988391128537,   -2.210082953194534e-18,  -0.0025362107449828698,
        -0.0042504596388292576,  -0.003541680745470664,   3.5958712522910524e-18,
        0.004846936744287676,    0.007969847402365364,    0.0065307714202767556,
        -5.228856038035346e-18,  -0.008705408876344524,   -0.01418076636456307,
        -0.011544255769852275,   6.908750361896574e-18,   0.015340851867117465,
        0.02510728381137605,     0.020647278836962893,    -8.379312071061862e-18,
        -0.028650470437300308,   -0.0487483392361389,     -0.04245177335530129,
        9.386967218113192e-18,   0.07346357047781228,     0.15767577793824084,
        0.22455795692367114,     0.2500051147634627,      0.22455795692367114,
        0.15767577793824084,     0.07346357047781228,     9.386967218113192e-18,
        -0.04245177335530129,    -0.0487483392361389,     -0.028650470437300308,
        -8.379312071061862e-18,  0.020647278836962893,    0.02510728381137605,
        0.015340851867117465,    6.908750361896574e-18,   -0.011544255769852275,
        -0.01418076636456307,    -0.008705408876344524,   -5.228856038035346e-18,
        0.0065307714202767556,   0.007969847402365364,    0.004846936744287676,
        3.5958712522910524e-18,  -0.003541680745470664,   -0.0042504596388292576,
        -0.0025362107449828698,  -2.210082953194534e-18,  0.0017711988391128537,
        0.002070080161846552,    0.0011994631388749504,   1.1803903744786633e-18,
        -0.000782060317801968,   -0.0008779146646607554,  -0.0004861931180949965,
        -5.195448361255068e-19,  0.0002840891246441517,   0.0002980248128025553,
        0.00015238102783352264,  1.6636269015907442e-19,  -7.171909618134312e-05,
        -6.435528108851458e-05,  -2.6462262345687998e-05, -2.279337438576827e-20
    };

    //: Measured against the Python reference, not derived: see this header's
    //: docstring. Both FIR stages' group delay combined, at the base rate.
    inline constexpr int kOversamplerLatencySamples = 20;

    namespace detail
    {
        /// A direct-form FIR with its own circular delay line, so it can run
        /// one sample at a time with no knowledge of how long the render is.
        class Fir
        {
        public:
            void reset() noexcept
            {
                line.fill (0.0);
                pos = 0;
            }

            double process (double x, double scale) noexcept
            {
                line[pos] = x;

                double acc = 0.0;
                std::size_t idx = pos;
                for (double tap : kOversampleFilter)
                {
                    acc += tap * line[idx];
                    idx = (idx == 0) ? (kOversampleFilterTaps - 1) : (idx - 1);
                }

                pos = (pos + 1 == kOversampleFilterTaps) ? 0 : (pos + 1);
                return acc * scale;
            }

        private:
            std::array<double, kOversampleFilterTaps> line {};
            std::size_t pos { 0 };
        };
    }

    class Oversampler
    {
    public:
        void reset() noexcept
        {
            up.reset();
            down.reset();
        }

        /// Runs `curve` at kOversampleFactor times the caller's rate and
        /// returns the band-limited causal result, one base-rate sample in,
        /// one base-rate sample out.
        template <typename Curve>
        double process (double x, Curve&& curve) noexcept
        {
            double result = 0.0;
            for (int phase = 0; phase < kOversampleFactor; ++phase)
            {
                const auto stuffed = (phase == 0) ? x : 0.0;
                const auto upsampled = up.process (stuffed, double (kOversampleFactor));
                const auto curved = curve (upsampled);
                const auto downsampled = down.process (curved, 1.0);
                if (phase == 0)
                    result = downsampled;
            }
            return result;
        }

    private:
        detail::Fir up, down;
    };
}
