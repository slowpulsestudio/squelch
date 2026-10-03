#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

/** AFTERGLOW's diffusion reverb, ported from `prototype/reverb.py`.

    Built for IONIZE, which gives every event its own send, so some arrive
    close and dry while others wash back. The tail crosses between channels
    rather than sitting still, so events scatter in depth and in space at once.

    Everything is a feedback structure and is normalised by ENERGY rather than
    DC gain: with DC normalisation a short decay comes out audibly louder than
    a long one and the control reads as a level rather than a time.

    The prototype solves its cross-fed delay pair as a recursion with a single
    denominator of length 2*delay, which is the same thing a pair of delay
    lines does sample by sample -- each side sees itself one round trip later.
*/
namespace squelch::dsp
{
    inline constexpr std::array<double, 4> kCombMs { 29.7, 37.1, 41.1, 43.7 };
    inline constexpr std::array<double, 3> kAllpassMs { 5.0, 1.7, 12.3 };
    inline constexpr double kAllpassG = 0.62;
    inline constexpr double kPingPongMs = 95.0;

    namespace detail
    {
        class Delay
        {
        public:
            void prepare (int samples)
            {
                length = std::max (samples, 1);
                line.assign (static_cast<size_t> (length), 0.0);
                pos = 0;
            }

            double read() const noexcept { return line[static_cast<size_t> (pos)]; }

            void write (double v) noexcept
            {
                line[static_cast<size_t> (pos)] = v;
                pos = (pos + 1 == length) ? 0 : (pos + 1);
            }

            void clear() noexcept { std::fill (line.begin(), line.end(), 0.0); }

        private:
            std::vector<double> line;
            int length { 1 }, pos { 0 };
        };

        /// Lowpassed feedback comb. The prototype's denominator carries the
        /// damping one sample past the delay, so the loop reads a two-tap
        /// average of its own output rather than a one-pole, and the input
        /// reaches the output immediately rather than a delay later.
        class DampedComb
        {
        public:
            void prepare (int samples)
            {
                delay = std::max (samples, 1);
                line.assign (static_cast<size_t> (delay + 2), 0.0);
                pos = 0;
            }

            double process (double x, double feedback, double damping) noexcept
            {
                const auto size = static_cast<int> (line.size());
                const auto i0 = ((pos - delay) % size + size) % size;
                const auto i1 = ((pos - delay - 1) % size + size) % size;

                const auto scale = std::sqrt (std::max (1.0 - feedback * feedback, 1e-6));
                const auto y = scale * x
                             + feedback * ((1.0 - damping) * line[static_cast<size_t> (i0)]
                                           + damping * line[static_cast<size_t> (i1)]);

                line[static_cast<size_t> (pos)] = y;
                pos = (pos + 1 == size) ? 0 : (pos + 1);
                return y;
            }

        private:
            std::vector<double> line;
            int delay { 1 }, pos { 0 };
        };

        class Allpass
        {
        public:
            void prepare (int samples) { delay.prepare (samples); }

            double process (double x, double g) noexcept
            {
                const auto delayed = delay.read();
                const auto v = x + g * delayed;
                delay.write (v);
                return delayed - g * v;
            }

        private:
            Delay delay;
        };
    }

    class Reverb
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;

            for (size_t i = 0; i < kAllpassMs.size(); ++i)
            {
                const auto n = std::max (static_cast<int> (kAllpassMs[i] * sr / 1000.0), 1);
                allpassL[i].prepare (n);
                allpassR[i].prepare (n);
            }

            const auto pp = std::max (static_cast<int> (kPingPongMs * sr / 1000.0), 1);
            pingDelay = pp;
            crossL.prepare (pp);
            crossR.prepare (pp);
            roundTripL.assign (static_cast<size_t> (2 * pp), 0.0);
            roundTripR.assign (static_cast<size_t> (2 * pp), 0.0);
            roundTripPos = 0;

            for (size_t i = 0; i < kCombMs.size(); ++i)
            {
                const auto n = std::max (static_cast<int> (kCombMs[i] * sr / 1000.0), 1);
                combL[i].prepare (n);
                combR[i].prepare (n);
            }
        }

        void set (double decay, double dampingAmount) noexcept
        {
            pingFeedback = 0.45 + 0.35 * decay;
            combFeedback = 0.70 + 0.21 * decay;
            damping = dampingAmount;
        }

        void process (double xL, double xR, double& outL, double& outR) noexcept
        {
            auto l = xL, r = xR;

            for (size_t i = 0; i < allpassL.size(); ++i)
            {
                l = allpassL[i].process (l, kAllpassG);
                r = allpassR[i].process (r, kAllpassG);
            }

            // What leaves one side arrives on the other. Each side therefore
            // sees itself one ROUND TRIP later, which is a single recursion
            // at twice the delay rather than a pair of coupled ones.
            const auto crossedL = l + pingFeedback * crossR.read();
            const auto crossedR = r + pingFeedback * crossL.read();
            crossL.write (l);
            crossR.write (r);

            const auto scale = std::sqrt (std::max (1.0 - pingFeedback * pingFeedback, 1e-6));
            const auto squared = pingFeedback * pingFeedback;
            const auto size = static_cast<int> (roundTripL.size());
            const auto back = ((roundTripPos - 2 * pingDelay) % size + size) % size;

            l = scale * crossedL + squared * roundTripL[static_cast<size_t> (back)];
            r = scale * crossedR + squared * roundTripR[static_cast<size_t> (back)];
            roundTripL[static_cast<size_t> (roundTripPos)] = l;
            roundTripR[static_cast<size_t> (roundTripPos)] = r;
            roundTripPos = (roundTripPos + 1 == size) ? 0 : (roundTripPos + 1);

            auto tailL = 0.0, tailR = 0.0;
            for (size_t i = 0; i < combL.size(); ++i)
            {
                tailL += combL[i].process (l, combFeedback, damping);
                tailR += combR[i].process (r, combFeedback, damping);
            }

            outL = tailL / double (kCombMs.size());
            outR = tailR / double (kCombMs.size());
        }

    private:
        double sr { 44100.0 };
        double pingFeedback { 0.45 }, combFeedback { 0.70 }, damping { 0.35 };

        std::array<detail::Allpass, 3> allpassL, allpassR;
        detail::Delay crossL, crossR;
        std::vector<double> roundTripL, roundTripR;
        int pingDelay { 1 }, roundTripPos { 0 };
        std::array<detail::DampedComb, 4> combL, combR;
    };
}
