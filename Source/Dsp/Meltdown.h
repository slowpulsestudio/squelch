#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

/** MELTDOWN, ported from `meltdown.py`.

    Each staged parameter has its own envelope rather than everything snapping
    at once, in the order the metaphor gives: the rods come out first, and the
    fallout arrives last and outlives everything else. How far the reaction gets
    depends on how long the gate is held, because a short tap never reaches the
    later stages.

    The prototype computes every curve across a whole render, which a stream
    cannot do. The same curves are kept as state instead: a rise is a linear
    ramp from when the gate opened, and a release is an exponential from
    wherever the rise had got to. Both advance in closed form, so the state
    after a stretch of samples is the same however that stretch is divided into
    host blocks.

    A gate opened while a release is still falling continues from the current
    level and does not jump back to zero.
*/
namespace squelch::dsp
{
    enum class Staged : std::size_t
    {
        containment, probability, spread, drive,
        reactivity, toxicity, exposure, contamination,
        count
    };

    struct MeltdownStage
    {
        /// Seconds after the gate opens before this parameter starts moving.
        double delay;
        /// Seconds to travel from its knob position to its critical value.
        double attack;
        /// Seconds to fall back after the gate closes.
        double release;
        /// Where the parameter goes at full meltdown.
        double target;
    };

    /// In the order of `Staged`, which is the prototype's order of arrival.
    inline constexpr std::array<MeltdownStage, static_cast<std::size_t> (Staged::count)> kMeltdownStages
    {{
        { 0.00, 0.10, 1.40, 0.00 },  // containment
        { 0.02, 0.18, 0.90, 1.00 },  // probability
        { 0.03, 0.50, 1.80, 1.00 },  // spread
        { 0.06, 0.26, 1.20, 1.00 },  // drive
        { 0.10, 0.30, 1.10, 0.85 },  // reactivity
        { 0.20, 0.36, 1.60, 1.00 },  // toxicity
        { 0.42, 0.44, 2.10, 1.00 },  // exposure
        { 0.66, 0.75, 3.40, 1.00 },  // contamination
    }};

    class Meltdown
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            reset();
        }

        void reset() noexcept
        {
            gate = false;
            heldSamples = 0;
            progress.fill (0.0);
            settled = true;
        }

        /// Opening the gate restarts the clock the stages are timed from.
        void setGate (bool open) noexcept
        {
            if (open && ! gate)
                heldSamples = 0;

            gate = open;
        }

        /// False once the gate is shut and every stage has fallen back, which is
        /// when the knob positions are the whole story again.
        bool active() const noexcept { return gate || ! settled; }

        /// How far through its stage a parameter is, 0 at its knob and 1 at its target.
        double progressOf (Staged s) const noexcept { return progress[static_cast<std::size_t> (s)]; }

        /// The effective value of a parameter whose knob is at `base`. Exactly
        /// `base` while nothing is happening, so an unfired MELTDOWN is inert.
        double value (Staged s, double base) const noexcept
        {
            const auto i = static_cast<std::size_t> (s);
            return base + (kMeltdownStages[i].target - base) * progress[i];
        }

        void advance (int samples) noexcept
        {
            if (samples <= 0)
                return;

            if (gate)
            {
                heldSamples += samples;
                const auto held = static_cast<double> (heldSamples) / sr;

                for (std::size_t i = 0; i < progress.size(); ++i)
                {
                    const auto& stage = kMeltdownStages[i];
                    const auto rising = std::clamp ((held - stage.delay) / std::max (stage.attack, 1.0e-6),
                                                    0.0, 1.0);
                    progress[i] = std::max (progress[i], rising);
                }
            }
            else if (! settled)
            {
                for (std::size_t i = 0; i < progress.size(); ++i)
                {
                    // The prototype's time constant is a third of the release time.
                    const auto tau = kMeltdownStages[i].release / 3.0;
                    progress[i] *= std::exp (-static_cast<double> (samples) / (tau * sr));

                    if (progress[i] < kSettledBelow)
                        progress[i] = 0.0;
                }
            }

            settled = std::all_of (progress.begin(), progress.end(),
                                   [] (double p) { return p <= 0.0; });
        }

    private:
        /// Below this a stage is indistinguishable from its knob, and letting
        /// it go to zero is what lets `active()` end.
        static constexpr double kSettledBelow = 1.0e-6;

        double sr { 44100.0 };
        bool gate { false };
        bool settled { true };
        long long heldSamples { 0 };
        std::array<double, static_cast<std::size_t> (Staged::count)> progress {};
    };
}
