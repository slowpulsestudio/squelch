#pragma once

#include <cmath>

#include "Ladder.h"
#include "Rng.h"

/** CHEMICAL's engine, ported from `prototype/reactor.py`'s `_chemical_engine`.

    A resonant feedback ladder with a stochastic register around it. The
    register is event-domain state, not sample-domain: dsp-maths.md specifies
    q[n] = q_i held for the whole event interval, entering the cutoff as a log
    frequency OFFSET and never as a multiplier on the sweep depth. That is the
    distinction from RADIATION, whose state evolves sample to sample.

    `setEvent()` is therefore the whole mechanism: the host calls it when an
    event fires and the register holds until the next one.
*/
namespace squelch::dsp
{
    inline constexpr int kChemicalRegisterStream = 501;
    inline constexpr double kChemicalRegisterOct = 1.3;

    class ChemicalEngine
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            ladder.prepare (sampleRate);
            ladder.setTap ({ 0.0, 0.0, 0.0, 1.0 });
            registerOffset = 1.0;
        }

        void setSeed (std::uint64_t s) noexcept { seed = s; }

        void setTap (const LadderTap& tap) noexcept { ladder.setTap (tap); }

        /// Zero-order hold: called once when an event fires, held until the
        /// next call. Nothing recomputes it per sample.
        void setEvent (std::uint64_t eventIndex) noexcept
        {
            const auto q = rng::ubipolar ({ seed, kChemicalRegisterStream, eventIndex });
            registerOffset = std::pow (2.0, kChemicalRegisterOct * q);
        }

        double process (double x, double cutoffHz, double feedback, double drive) noexcept
        {
            return ladder.process (x, cutoffHz * registerOffset, feedback, drive);
        }

        /// The stochastic register itself. Test 6 has to see that this is
        /// piecewise constant between events rather than evolving per sample,
        /// which is what separates CHEMICAL from RADIATION.
        double registerValue() const noexcept { return registerOffset; }

    private:
        Ladder ladder;
        std::uint64_t seed { 0 };
        double registerOffset { 1.0 };
    };
}
