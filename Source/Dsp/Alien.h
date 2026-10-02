#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "Rng.h"

/** ALIEN's engine, ported from `prototype/reactor.py`'s `_alien_engine`.

    A source oscillator, not a filter on the input: each scheduled event gets
    its own deterministic hashed pitch, an exponential sweep, self-FM with a
    decaying index, an AM burst, an attack/release envelope and a short chirp
    accent. With no events and a silent input it produces nothing.

    The prototype renders each event as a whole array and sums the overlaps.
    A real-time port cannot, so this is a voice pool: `trigger()` starts one,
    `process()` advances every live voice and sums them. Voices run until
    their envelope has decayed past `kVoiceDuration`, which is the prototype's
    own per-event `dur`, so the two cover the same span of samples.

    `theta_m` and `theta_c` in the prototype are the same cumulative sum, so
    the modulator IS the carrier: `sin(theta + beta*sin(theta))`. That is
    phase modulation by its own phase, not a separate operator, and the port
    keeps one accumulator rather than inventing a second.
*/
namespace squelch::dsp
{
    inline constexpr int kAlienPitchStream = 401;
    inline constexpr double kAlienDMaxSemitones = 36.0;
    inline constexpr double kAlienBeta0Max = 6.0;
    inline constexpr double kAlienTauBetaS = 0.03;
    inline constexpr double kAlienMuMax = 1.5;
    inline constexpr double kAlienAmHzLo = 30.0;
    inline constexpr double kAlienAmHzHi = 200.0;
    inline constexpr double kAlienChirpSemitones = 12.0;
    inline constexpr double kAlienTauZS = 0.01;
    inline constexpr double kAlienAttackS = 0.003;
    inline constexpr double kAlienInputGain = 0.6;

    struct AlienProfile
    {
        double baseHz { 400.0 };
        double sweepOctaves { 5.2 };
        double decayLoS { 0.04 };
        double decayHiS { 0.5 };
    };

    struct AlienParams
    {
        double spread { 0.0 };
        double decay { 0.3 };
        double toxicity { 0.0 };
        double exposure { 0.0 };
        std::uint64_t seed { 0 };
    };

    class AlienEngine
    {
    public:
        static constexpr int kMaxVoices = 16;

        void prepare (double sampleRateIn) noexcept
        {
            sr = sampleRateIn;
            for (auto& v : voices)
                v.active = false;
        }

        void configure (const AlienProfile& profile, const AlienParams& p) noexcept
        {
            baseHz = profile.baseHz;
            seed = p.seed;
            sweepSemitones = 12.0 * profile.sweepOctaves * p.spread;

            const auto decayTime = profile.decayLoS
                                 + (profile.decayHiS - profile.decayLoS) * p.decay;
            tauF = std::max (decayTime * 0.5, 0.01);
            tauR = std::max (decayTime, 0.02);
            beta0 = kAlienBeta0Max * p.toxicity;
            mu = kAlienMuMax * p.exposure;
            fA = kAlienAmHzLo + (kAlienAmHzHi - kAlienAmHzLo) * p.exposure;

            // The prototype's per-event `dur`: how long one event is rendered
            // for before it is dropped, not a fade applied to it.
            voiceSamples = static_cast<int> (sr * std::max (8.0 * tauR, 0.05));
        }

        /// `inputLevel` is the prototype's e_x[start]: the input's peak at the
        /// moment the event fires, sampled once and held for the whole event.
        void trigger (std::uint64_t eventIndex, bool accent, double pan,
                      double inputLevel) noexcept
        {
            auto* v = findFreeVoice();
            if (v == nullptr)
                return;

            const auto r = rng::ubipolar ({ seed, kAlienPitchStream, eventIndex });
            v->f0 = baseHz * std::pow (2.0, (kAlienDMaxSemitones * r) / 12.0);
            v->accent = accent;
            v->pan = 0.5 * (1.0 + pan);
            v->amplitude = (accent ? 1.3 : 0.8) + kAlienInputGain * inputLevel;
            v->chirpAmplitude = accent ? 0.6 : 0.3;
            v->theta = 0.0;
            v->thetaZ = 0.0;
            v->n = 0;
            v->active = true;
        }

        void process (double& outL, double& outR) noexcept
        {
            outL = 0.0;
            outR = 0.0;

            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                const auto tt = v.n / sr;

                const auto fC = std::clamp (
                    v.f0 * std::pow (2.0, sweepSemitones * (1.0 - std::exp (-tt / tauF)) / 12.0),
                    20.0, sr * 0.45);

                v.theta += 2.0 * M_PI * fC / sr;
                const auto beta = beta0 * std::exp (-tt / kAlienTauBetaS);
                const auto zFm = std::sin (v.theta + beta * std::sin (v.theta));

                const auto mA = 0.5 * (1.0 + std::sin (2.0 * M_PI * fA * tt));
                const auto zAm = (1.0 + mu * mA) * zFm;

                const auto env = (1.0 - std::exp (-tt / kAlienAttackS)) * std::exp (-tt / tauR);

                const auto fZ = std::clamp (fC * std::pow (2.0, kAlienChirpSemitones / 12.0),
                                            20.0, sr * 0.45);
                v.thetaZ += 2.0 * M_PI * fZ / sr;
                const auto zZ = v.chirpAmplitude * std::exp (-tt / kAlienTauZS) * std::sin (v.thetaZ);

                const auto burst = v.amplitude * env * zAm + zZ;
                outL += burst * (1.0 - v.pan);
                outR += burst * v.pan;

                if (++v.n >= voiceSamples)
                    v.active = false;
            }
        }

    private:
        struct Voice
        {
            bool active { false };
            bool accent { false };
            double f0 { 400.0 }, pan { 0.5 }, amplitude { 0.8 }, chirpAmplitude { 0.3 };
            double theta { 0.0 }, thetaZ { 0.0 };
            int n { 0 };
        };

        Voice* findFreeVoice() noexcept
        {
            for (auto& v : voices)
                if (! v.active)
                    return &v;

            // Steal the oldest rather than drop the event: a missing event is
            // a missing note, a stolen one is a shortened tail.
            Voice* oldest = &voices[0];
            for (auto& v : voices)
                if (v.n > oldest->n)
                    oldest = &v;
            return oldest;
        }

        double sr { 44100.0 };
        std::uint64_t seed { 0 };
        double baseHz { 400.0 }, sweepSemitones { 0.0 };
        double tauF { 0.02 }, tauR { 0.04 }, beta0 { 0.0 }, mu { 0.0 }, fA { 30.0 };
        int voiceSamples { 2205 };

        std::array<Voice, kMaxVoices> voices {};
    };
}
