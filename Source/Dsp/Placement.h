#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "Rng.h"
#include "Scheduler.h"

/** Event placement and send, ported from `reactor.py`'s `build_controls`.

    Only the parts the output chain consumes: the event envelope, the stereo
    placement derived from it, and AFTERGLOW's send. Cutoff and resonance come
    from Envelopes.h for the reactions that use a ladder.

    Runs at control rate (one value per BLOCK samples), which is what the
    prototype does and what dsp-testing.md Test 4 permits as explicitly
    documented control-domain state.

    The placement rule is not an average. Overlapping events take the LOUDEST
    event's position, because averaging pulls every one of them back to the
    middle and the point is that successive reactions bounce across the field.
    That makes it a small voice pool rather than a single running value.
*/
namespace squelch::dsp
{
    inline constexpr int kControlBlock = 8;
    inline constexpr double kAttackS = 0.005;
    inline constexpr double kShapeRange = 0.8;
    inline constexpr double kPanSmoothS = 0.004;
    inline constexpr double kAntiStepS = 0.015;
    inline constexpr double kIonizeDepthLo = 0.15;
    inline constexpr double kIonizeDepthSpan = 1.5;

    struct PlacementValues { double env, panL, panR, send; };

    class Placement
    {
    public:
        static constexpr int kMaxVoices = 16;

        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate;
            controlRate = sr / kControlBlock;
            panSmooth = std::exp (-1.0 / std::max (kPanSmoothS * controlRate, 1e-6));
            antiStep = std::exp (-1.0 / std::max (kAntiStepS * controlRate, 1e-6));
            for (auto& v : voices)
                v.active = false;
            panLeft = panRight = std::sqrt (2.0) * std::cos (0.25 * M_PI);
            primed = false;
            counter = 0;
        }

        void configure (double volatility, double ionizeAmount,
                        double decayLoS, double decayHiS, double decay,
                        double persistence, double halfLife) noexcept
        {
            volatilityAmount = volatility;
            ionize = ionizeAmount;
            decaySeconds = decayLoS + (decayHiS - decayLoS) * decay;
            hold = halfLife * 0.85 * persistence;
        }

        void trigger (const ScheduledEvent& e) noexcept
        {
            auto* v = findFree();
            if (v == nullptr)
                return;

            // VOLATILITY varies the envelope's shape and attack, not only its
            // length: a soft swell and a sharp pluck are different events.
            v->curve = 1.0 + kShapeRange * volatilityAmount * (e.shape * 2.0 - 1.0);
            v->attackBlocks = std::max (static_cast<int> (kAttackS
                                       * (1.0 + 2.5 * volatilityAmount * e.shape) * controlRate), 1);
            v->tauBlocks = std::max (decaySeconds * e.decayScale, 0.005) * controlRate;
            v->lengthBlocks = static_cast<int> (v->tauBlocks * 6.0);

            auto placementValue = e.pan;
            auto spread = volatilityAmount;
            if (ionize > 0.0)
            {
                // Consecutive events throw to opposite sides, which is what
                // makes it read as bouncing rather than merely wide.
                const auto side = (e.index % 2) ? 1.0 : -1.0;
                const auto scattered = side * (0.55 + 0.45 * std::abs (e.pan));
                spread = std::max (spread, ionize);
                placementValue = e.pan * (1.0 - ionize) + scattered * ionize;
            }

            const auto angle = (placementValue * spread + 1.0) * 0.25 * M_PI;
            v->panL = std::cos (angle) * std::sqrt (2.0);
            v->panR = std::sin (angle) * std::sqrt (2.0);
            v->distance = 1.0 - ionize + ionize * (kIonizeDepthLo + kIonizeDepthSpan * e.depth);
            v->elapsed = 0;
            v->active = true;
        }

        /// Advances one SAMPLE; the control value only moves every kControlBlock.
        PlacementValues process() noexcept
        {
            if (counter == 0)
                step();
            counter = (counter + 1 == kControlBlock) ? 0 : (counter + 1);
            return { envelope, panLeft, panRight, sendLevel };
        }

    private:
        struct Voice
        {
            bool active { false };
            double curve { 1.0 }, tauBlocks { 1.0 }, panL { 1.0 }, panR { 1.0 }, distance { 1.0 };
            int attackBlocks { 1 }, lengthBlocks { 1 }, elapsed { 0 };
        };

        Voice* findFree() noexcept
        {
            for (auto& v : voices)
                if (! v.active)
                    return &v;

            Voice* oldest = &voices[0];
            for (auto& v : voices)
                if (v.elapsed > oldest->elapsed)
                    oldest = &v;
            return oldest;
        }

        void step() noexcept
        {
            auto rawEnv = 0.0;
            auto rawSend = 0.0;

            auto loudest = 0.0;
            // Centre when nothing is playing: the prototype's pan_gain array
            // is initialised to unity and only written where an event covers,
            // so gaps return to centre rather than holding the last position.
            auto wantL = 1.0, wantR = 1.0;

            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                const auto t = double (v.elapsed);
                auto e = std::pow (std::exp (-t / std::max (v.tauBlocks, 1e-6)), v.curve);
                if (v.elapsed < v.attackBlocks)
                    e *= std::min (t / v.attackBlocks, 1.0);

                rawEnv = std::max (rawEnv, e);
                rawSend = std::max (rawSend, e * v.distance);
                // The loudest event present owns the placement.
                if (e > loudest)
                {
                    loudest = e;
                    wantL = v.panL;
                    wantR = v.panR;
                }

                if (++v.elapsed >= v.lengthBlocks)
                    v.active = false;
            }

            if (! primed)
            {
                panLeft = wantL;
                panRight = wantR;
                envelope = rawEnv;
                sendLevel = rawSend;
                primed = true;
            }
            else
            {
                panLeft = (1.0 - panSmooth) * wantL + panSmooth * panLeft;
                panRight = (1.0 - panSmooth) * wantR + panSmooth * panRight;

                // Anti-step smoothing runs always, not only under CONTAINMENT.
                envelope = (1.0 - antiStep) * rawEnv + antiStep * envelope;
                sendLevel = (1.0 - antiStep) * rawSend + antiStep * sendLevel;
            }
        }

        double sr { 44100.0 }, controlRate { 5512.5 };
        double volatilityAmount { 0.0 }, ionize { 0.0 };
        double decaySeconds { 0.1 }, hold { 0.0 };
        double panSmooth { 0.0 }, antiStep { 0.0 }, panLeft { 1.0 }, panRight { 1.0 };
        double envelope { 0.0 }, sendLevel { 0.0 };
        bool primed { false };
        int counter { 0 };

        std::array<Voice, kMaxVoices> voices {};
    };
}
