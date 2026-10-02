#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "Rng.h"
#include "Scheduler.h"

/** Cutoff, feedback and loop drive, ported from `reactor.py`'s `envelopes`.

    The cutoff is the 303's: it jumps to its peak the instant an event fires
    and falls back exponentially, fc(t) = base * 2^(sweep * exp(-t/tau)), with
    the sweep in octaves so it stays musical wherever base sits. An accented
    step opens the sweep further and pushes feedback nearer to oscillation, so
    accents change the voice rather than the volume.

    Nothing here touches amplitude. Multiplying the source by an event envelope
    replaces its dynamics with a uniform stream.

    One dependency is not causal in the prototype: a slid event's glide is
    capped at half the interval to the NEXT event, which a streaming port
    cannot know from the audio. It does not have to -- the scheduler places
    step k from k alone, so the caller can ask it where the next event falls
    and pass the distance in. That keeps the behaviour rather than
    approximating it.
*/
namespace squelch::dsp
{
    inline constexpr double kAccentSweepOct = 0.8;
    inline constexpr double kAccentFeedback = 0.25;
    inline constexpr double kCarryLo = 0.30;
    inline constexpr double kCarryHi = 0.95;
    inline constexpr double kIonizeSpectral = 0.55;
    inline constexpr double kSlideS = 0.060;
    inline constexpr double kGlideS = 0.002;
    inline constexpr int kEnvelopeStream = 22;

    struct EnvelopeProfile
    {
        double baseHz { 180.0 };
        double sweepOctaves { 4.8 };
        double tauLoS { 0.035 };
        double tauHiS { 0.26 };
        double feedbackLo { 1.5 };
        double feedbackHi { 3.25 };
        double driveLo { 1.1 };
        double driveHi { 2.5 };
        std::array<int, 8> accents { 0, 0, 1, 0, 0, 1, 0, 1 };
    };

    struct EnvelopeParams
    {
        double spread { 0.0 };
        double decay { 0.3 };
        double exposure { 0.0 };
        double toxicity { 0.0 };
        double containment { 0.0 };
        double halfLife { 0.4 };
        double ionizeAmount { 0.0 };
        std::uint64_t seed { 0 };
    };

    struct EnvelopeValues { double cutoffHz, feedback, drive; };

    class Envelopes
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            // The length is truncated to whole samples before the coefficient
            // is taken from it, so exp(-1/88) at 44.1k and not exp(-1/88.2).
            glide = std::exp (-1.0 / std::max (static_cast<int> (kGlideS * sr), 1));
            slideSamples = std::max (static_cast<int> (kSlideS * sr), 1);
            active = false;
        }

        void configure (const EnvelopeProfile& p, const EnvelopeParams& v) noexcept
        {
            profile = p;
            params = v;

            sweep = p.sweepOctaves * v.spread * (1.0 - 0.7 * v.containment);
            tau = p.tauLoS + (p.tauHiS - p.tauLoS) * v.decay;
            driveAmount = p.driveLo + (p.driveHi - p.driveLo) * v.toxicity;

            // HALF-LIFE sets how far feedback falls between events, which is
            // what decides how long the filter keeps ringing. Letting it drop
            // all the way back kills the tail.
            peak = p.feedbackLo + (p.feedbackHi - p.feedbackLo) * v.exposure;
            floor = peak * (kCarryLo + (kCarryHi - kCarryLo) * v.halfLife);

            baseOct = std::log2 (p.baseHz);
            held = baseOct;

            // The prototype seeds its glide from the first value of the
            // already-computed control, not from the resting one, so the
            // first sample passes through unfiltered.
            primed = false;
            lastCurve = baseOct;
        }

        /// `samplesToNext` is the distance to the following event, which only
        /// a slid event uses. Pass the render length if nothing follows.
        void trigger (const ScheduledEvent& e, std::int64_t samplesToNext) noexcept
        {
            // The reaction's accent pattern rides on the GRID STEP, so each
            // keeps its rhythmic signature whatever the seed does. Indexing by
            // the event's own index would step through five at a time, because
            // the scheduler numbers sub-events inside each step.
            const auto pattern = profile.accents[(e.index / kMaxSubEvents) % profile.accents.size()];
            accent = std::min (1.0, pattern + (e.accent ? 0.6 : 0.0));
            weight = e.intensity;
            tauEvent = std::max (tau * e.decayScale, 0.005);

            const auto ionize = params.ionizeAmount;
            registerOct = baseOct + ionize * kIonizeSpectral
                        * rng::ubipolar ({ params.seed, kEnvelopeStream, e.index });
            depth = (sweep + kAccentSweepOct * accent * params.spread) * weight;

            slideFrom = lastCurve;
            slideLength = e.slide
                        ? std::min<std::int64_t> (slideSamples,
                                                  std::max<std::int64_t> (samplesToNext / 2, 1))
                        : 0;
            elapsed = 0;
            active = true;
        }

        EnvelopeValues process() noexcept
        {
            auto curve = baseOct;
            auto feedback = floor;

            if (active)
            {
                const auto shape = std::exp (-double (elapsed) / (tauEvent * sr));
                curve = registerOct + depth * shape;

                if (elapsed < slideLength)
                {
                    // A slid note glides in from wherever the last one
                    // finished rather than jumping. This is the 303's
                    // portamento, and it stays separate from the glide below,
                    // which is only an anti-click.
                    //
                    // np.linspace(0, 1, N) steps by 1/(N-1), reaching 1 on its
                    // last point, so the divisor is not N.
                    const auto span = std::max<std::int64_t> (slideLength - 1, 1);
                    const auto t = std::min (1.0, double (elapsed) / double (span));
                    curve = slideFrom * (1.0 - t) + curve * t;
                }

                feedback = floor + (peak - floor) * shape * weight
                         + kAccentFeedback * accent * params.exposure * weight;
                ++elapsed;
            }

            lastCurve = curve;

            const auto rawCutoff = std::pow (2.0, curve);

            // The ladder rings hard, so a step in its cutoff is a click.
            if (! primed)
            {
                glidedCutoff = rawCutoff;
                glidedFeedback = feedback;
                primed = true;
            }
            else
            {
                glidedCutoff = (1.0 - glide) * rawCutoff + glide * glidedCutoff;
                glidedFeedback = (1.0 - glide) * feedback + glide * glidedFeedback;
            }

            return { std::max (glidedCutoff, 20.0),
                     std::clamp (glidedFeedback, 0.0, 3.95),
                     driveAmount };
        }

    private:
        double sr { 44100.0 };
        EnvelopeProfile profile;
        EnvelopeParams params;

        double sweep { 0.0 }, tau { 0.1 }, driveAmount { 1.0 };
        double peak { 1.5 }, floor { 0.5 }, baseOct { 7.5 };

        bool active { false };
        double accent { 0.0 }, weight { 1.0 }, tauEvent { 0.1 };
        double registerOct { 7.5 }, depth { 0.0 };
        double slideFrom { 7.5 }, held { 7.5 }, lastCurve { 7.5 };
        std::int64_t elapsed { 0 }, slideLength { 0 };
        int slideSamples { 2646 };

        double glide { 0.0 }, glidedCutoff { 180.0 }, glidedFeedback { 0.5 };
        bool primed { false };
    };
}
