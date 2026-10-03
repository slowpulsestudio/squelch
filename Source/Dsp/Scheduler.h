#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "Rng.h"

/** The event scheduler, ported from `prototype/scheduler.py`.

    The prototype produces every event for a whole render at once. A plugin
    cannot: it is handed one block at a time and the transport can jump. So
    this computes a step's events from its index alone, and `forRange()`
    scans only the steps that can land in the block it is given.

    That is possible because GRID and FREE place step k at a position derived
    from k, the seed and the tempo, with nothing carried forward. RANDOM
    accumulates its own step lengths and INPUT reads onsets out of the audio,
    so neither is indexable this way; they are not ported here.

    Grid lengths are held as double. Parameters.h keeps a float table for the
    editor, and 2/3, 1/3, 1/6 and 1/12 in float would place events a sample
    or two out from the prototype.
*/
namespace squelch::dsp
{
    inline constexpr int kMaxSubEvents = 5;
    inline constexpr double kFreeRateRatio = 0.7213;
    inline constexpr double kAccentChance = 0.30;
    inline constexpr double kSlideChance = 0.30;
    inline constexpr double kDensitySuppression = 0.55;
    inline constexpr double kTimingJitter = 0.18;

    inline constexpr std::array<double, 15> kGridBeats {
        4.0, 2.0, 1.0, 1.5, 2.0 / 3.0,
        0.5, 0.75, 1.0 / 3.0, 0.25, 0.375,
        1.0 / 6.0, 0.125, 0.1875, 1.0 / 12.0,
        0.0625
    };

    struct ScheduledEvent
    {
        std::int64_t start { 0 };
        std::uint64_t index { 0 };
        double intensity { 1.0 };
        double decayScale { 1.0 };
        double tone { 0.0 };
        double pan { 0.0 };
        double shape { 0.0 };
        double depth { 0.0 };
        bool accent { false };
        bool slide { false };
    };

    enum class Mode { grid, random, free, input };

    struct ScheduleSettings
    {
        int gridIndex { 8 };
        double bpm { 140.0 };
        double flux { 0.0 };
        double probability { 1.0 };
        double reactivity { 0.0 };
        double volatility { 0.0 };
        double containment { 0.0 };
        double subEventBias { 1.0 };
        Mode mode { Mode::grid };
        std::uint64_t seed { 0 };
    };

    class Scheduler
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            randomCursor = 0.0;
            randomStep = 0;
            randomReady = false;
            onsetEnvelope = 0.0;
            onsetPrevious = 0.0;
            onsetPeak = 1e-9;
            onsetHold = 0.0;
            onsetAt = 0.0;
            framesSinceOnset = 1 << 20;
            onsetIndex = 0;
            hopCounter = 0;
            queued = 0;
        }

        void configure (const ScheduleSettings& s) noexcept
        {
            // RANDOM accumulates its own step lengths, so a change of tempo
            // or FLUX restarts the walk rather than retro-fitting it.
            if (s.bpm != settings.bpm || s.flux != settings.flux
                || s.gridIndex != settings.gridIndex || s.seed != settings.seed)
                randomReady = false;

            settings = s;
        }

        double stepSeconds() const noexcept
        {
            return kGridBeats[static_cast<size_t> (settings.gridIndex)] * 60.0 / settings.bpm;
        }

        /** Emits every event starting in [from, from + length).

            `emit` is called once per event and must not allocate. Steps are
            scanned with a margin either side because FLUX swing and jitter
            can move a step up to 0.8 of a step late or 0.3 early.

            INPUT mode is not scanned this way: its events come from onsets in
            the audio, so they arrive through `detectOnsets` instead.
        */
        template <typename Emit>
        void forRange (std::int64_t from, int length, Emit&& emit) const
        {
            if (settings.mode == Mode::input)
                return;

            const auto step = stepSeconds();
            if (step <= 0.0 || length <= 0)
                return;

            if (settings.mode == Mode::random)
            {
                advanceRandom (from, length, emit);
                return;
            }

            const auto stepSamples = step * sr;
            const auto first = static_cast<std::int64_t> (std::floor (from / stepSamples)) - 2;
            const auto last = static_cast<std::int64_t> (std::floor ((from + length) / stepSamples)) + 2;

            for (auto k = std::max<std::int64_t> (first, 0); k <= last; ++k)
                stepEvents (static_cast<std::uint64_t> (k), from, from + length, emit);
        }

        /** INPUT mode: onsets from a half-wave-rectified flux envelope.

            Call once per sample with the input's magnitude. The prototype
            normalises flux by the whole render's maximum, which a stream does
            not have, so this tracks a decaying running peak instead -- the
            one place where INPUT cannot be bit-identical to the offline
            version, and it is a normalisation choice rather than a mechanism.
        */
        template <typename Emit>
        void detectOnsets (double magnitude, std::int64_t position, Emit&& emit)
        {
            if (settings.mode != Mode::input)
                return;

            // An onset's sub-events are spread across the step that follows
            // it, so they are queued when it is detected and released at
            // their own sample rather than all firing on the transient.
            for (int i = 0; i < queued; )
            {
                if (queue[static_cast<size_t> (i)].start <= position)
                {
                    emit (queue[static_cast<size_t> (i)]);
                    queue[static_cast<size_t> (i)] = queue[static_cast<size_t> (--queued)];
                }
                else
                {
                    ++i;
                }
            }

            frameMax = std::max (frameMax, magnitude);
            if (++hopCounter < kOnsetHop)
                return;

            hopCounter = 0;
            const auto envelope = frameMax;
            frameMax = 0.0;

            auto flux = envelope - onsetPrevious;
            onsetPrevious = envelope;
            if (flux < 0.0)
                flux = 0.0;

            onsetPeak = std::max (onsetPeak * kOnsetPeakDecay, flux);
            const auto normalised = flux / std::max (onsetPeak, 1e-9);

            const auto minimumGap = static_cast<int> (0.045 * sr / kOnsetHop);
            ++framesSinceOnset;

            const auto rising = normalised >= kOnsetThreshold && framesSinceOnset >= minimumGap;

            if (! rising)
                return;

            framesSinceOnset = 0;
            onsetAt = double (position) / sr;

            const auto window = static_cast<std::int64_t> (stepSeconds() * sr) + 1;
            stepEvents (onsetIndex, position, position + window,
                        [this] (const ScheduledEvent& e)
                        {
                            if (queued < int (kQueueSize))
                                queue[static_cast<size_t> (queued++)] = e;
                        });
            ++onsetIndex;
        }

    private:
        static constexpr int kOnsetHop = 256;
        static constexpr double kOnsetThreshold = 0.12;
        static constexpr double kOnsetPeakDecay = 0.9995;

        /** RANDOM's step lengths accumulate, so step k cannot be computed
            from k alone the way GRID's can. The walk is carried forward
            instead, which means it is correct for linear playback and
            restarts on a transport jump rather than silently desyncing.
        */
        template <typename Emit>
        void advanceRandom (std::int64_t from, int length, Emit&& emit) const
        {
            const auto step = stepSeconds();

            if (! randomReady || randomCursor * sr > double (from) + length)
            {
                randomCursor = 0.0;
                randomStep = 0;
                randomReady = true;
            }

            while (randomCursor * sr < double (from) + length)
            {
                const auto at = static_cast<std::int64_t> (randomCursor * sr);
                if (at >= from)
                    stepEvents (randomStep, from, from + length, emit);

                const auto spread = 0.25 + 1.75 * rng::urand ({ settings.seed, 102, randomStep });
                randomCursor += step * (1.0 - settings.flux + settings.flux * spread * 2.0);
                ++randomStep;

                if (randomStep > (1u << 24))
                    break;
            }
        }

        /// Where step k sits, in seconds. Derived from k alone.
        double timeOfStep (std::uint64_t k) const noexcept
        {
            const auto step = stepSeconds();

            if (settings.mode == Mode::free)
            {
                const auto freeStep = step * kFreeRateRatio;
                const auto jitter = 0.3 * settings.flux * freeStep
                                  * rng::ubipolar ({ settings.seed, 103, k });
                return double (k) * freeStep + jitter;
            }

            if (settings.mode == Mode::random)
                return randomCursor;

            // INPUT's events are anchored to the onset that produced them,
            // not to a position on the grid.
            if (settings.mode == Mode::input)
                return onsetAt;

            const auto swing = (k % 2) ? 0.5 * settings.flux * step : 0.0;
            const auto jitter = 0.3 * settings.flux * step
                              * rng::ubipolar ({ settings.seed, 101, k });
            return double (k) * step + swing + jitter;
        }

        template <typename Emit>
        void stepEvents (std::uint64_t k, std::int64_t from, std::int64_t to, Emit&& emit,
                         double spreadScale = 0.9) const
        {
            const auto density = 1.0 - kDensitySuppression * settings.containment;
            const auto probability = settings.probability * density;

            if (rng::urand ({ settings.seed, 1, k }) >= probability)
                return;

            // Python's round() is half-to-even, and std::round is not; a step
            // landing exactly on .5 would otherwise fan out differently.
            const auto wanted = settings.reactivity * settings.subEventBias
                              * density * (kMaxSubEvents - 1);
            const auto count = 1 + static_cast<int> (std::nearbyint (wanted));

            const auto step = stepSeconds();
            const auto t = timeOfStep (k);

            for (int s = 0; s < count; ++s)
            {
                auto offset = (double (s) / count) * step * spreadScale;
                offset += step * kTimingJitter * settings.volatility
                        * rng::ubipolar ({ settings.seed, 7, k, std::uint64_t (s) });

                const auto start = static_cast<std::int64_t> ((t + offset) * sr);
                if (start < from || start >= to || start < 0)
                    continue;

                ScheduledEvent e;
                e.start = start;
                e.index = k * kMaxSubEvents + std::uint64_t (s);
                e.intensity = 1.0 - 0.35 * (double (s) / std::max (count - 1, 1));
                e.decayScale = 1.0 + settings.volatility
                             * rng::ubipolar ({ settings.seed, 2, k, std::uint64_t (s) });
                e.tone = rng::urand ({ settings.seed, 3, k, std::uint64_t (s) });
                e.pan = rng::ubipolar ({ settings.seed, 4, k, std::uint64_t (s) });
                e.shape = rng::urand ({ settings.seed, 8, k, std::uint64_t (s) });
                e.depth = rng::urand ({ settings.seed, 9, k, std::uint64_t (s) });
                e.accent = rng::urand ({ settings.seed, 5, k, std::uint64_t (s) }) < kAccentChance;
                e.slide = rng::urand ({ settings.seed, 6, k, std::uint64_t (s) }) < kSlideChance;
                emit (e);
            }
        }

        double sr { 44100.0 };
        ScheduleSettings settings;

        // RANDOM carries its walk forward; INPUT carries its onset detector.
        mutable double randomCursor { 0.0 };
        mutable std::uint64_t randomStep { 0 };
        mutable bool randomReady { false };

        double frameMax { 0.0 }, onsetEnvelope { 0.0 }, onsetPrevious { 0.0 };
        double onsetPeak { 1e-9 }, onsetHold { 0.0 }, onsetAt { 0.0 };
        int framesSinceOnset { 1 << 20 }, hopCounter { 0 };
        std::uint64_t onsetIndex { 0 };

        static constexpr std::size_t kQueueSize = 16;
        std::array<ScheduledEvent, kQueueSize> queue {};
        int queued { 0 };
    };
}
