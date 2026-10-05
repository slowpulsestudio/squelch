#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Filters.h"
#include "Placement.h"
#include "Rng.h"
#include "Scheduler.h"

/** CONTAMINATION: each reaction's noise bed, ported from `reactions.py`.

    The bed is added to the engine's output, ahead of DRIVE, at a level taken
    from the output itself, so CONTAMINATION maps to a level in dB rather than
    to whatever the gain chain left. Squaring the control puts full travel 12 dB
    above half. noise-beds.md has the five characters; the prototype has the
    numbers, and where the two differ the prototype wins.

    Four of the five are the prototype's generators and agree with it to
    machine precision. CHEMICAL is not, and cannot be: its fizz places grains at
    random across the whole render and normalises them by the render's own peak
    and level, which a stream does not have. It lays grains as a hashed Poisson
    process of the same density, referenced to a running level instead.

    Everything is a function of the absolute sample position and the control
    block it falls in, never of how the host cut the audio into blocks.

    The bed lags the audio by one control block. The prototype interpolates the
    smoothed event envelope between control values, which needs the next one, and
    a stream can only have that by waiting for it. Eight samples is 0.18 ms of
    noise, and delaying all five by the same amount keeps their timing alike.
*/
namespace squelch::dsp
{
    inline constexpr double kSidechainDepth = 0.9;
    inline constexpr double kSidechainAttackS = 0.005;
    inline constexpr double kSidechainReleaseS = 0.080;
    inline constexpr double kBedMatchS = 1.5;

    /// Smooths a change of CONTAMINATION, so a stepped control does not click.
    inline constexpr double kAmountSmoothS = 0.010;

    inline constexpr int kBedStreamRadiation = 1;
    inline constexpr int kBedStreamFission = 2;
    inline constexpr int kBedStreamSludge = 3;
    inline constexpr int kBedStreamChemical = 4;
    inline constexpr int kBedStreamAlien = 5;

    /// In the order of the reactions.
    struct BedLevels { double fullLevel, unitRms; };

    inline constexpr std::array<BedLevels, 5> kBedLevels
    {{
        { 0.0229, 0.005499 },   // RADIATION
        { 0.0902, 0.069393 },   // FISSION
        { 0.1373, 0.099999 },   // SLUDGE
        { 0.0530, 0.206800 },   // CHEMICAL, calibrated for this bed: see the header
        { 0.1147, 0.045540 },   // ALIEN
    }};

    namespace detail
    {
        /// Standard normal per (stream, channel, sample), by Box-Muller on two
        /// hashed uniforms, as `rng.gaussian_array`.
        inline double gaussian (std::uint64_t stream, std::uint64_t channel,
                                std::uint64_t index) noexcept
        {
            const auto u1 = std::clamp (rng::urand ({ stream, channel, 0, index }), 1e-12, 1.0);
            const auto u2 = rng::urand ({ stream, channel, 1, index });
            return std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * M_PI * u2);
        }

        /** A swept RBJ bandpass in transposed direct form II, as `lfilter`.

            The form matters once the coefficients move: direct form I and II
            carry different states, so the same sweep rings differently.
        */
        class SweptBandpass
        {
        public:
            void reset() noexcept { z1 = z2 = 0.0; }

            void set (double fc, double q, double sr) noexcept
            {
                fc = std::clamp (fc, 20.0, sr * 0.45);
                q = std::max (q, 0.3);
                const auto w0 = 2.0 * M_PI * fc / sr;
                const auto alpha = std::sin (w0) / (2.0 * q);
                const auto a0 = 1.0 + alpha;
                b0 = alpha / a0;
                b2 = -alpha / a0;
                a1 = -2.0 * std::cos (w0) / a0;
                a2 = (1.0 - alpha) / a0;
            }

            double process (double x) noexcept
            {
                const auto y = b0 * x + z1;
                z1 = -a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }

        private:
            double b0 { 0.0 }, b2 { 0.0 }, a1 { 0.0 }, a2 { 0.0 };
            double z1 { 0.0 }, z2 { 0.0 };
        };

        /// A plain average that warms up the way `filters.running_mean` does.
        class RunningMean
        {
        public:
            void prepare (double sampleRate, double timeSeconds) noexcept
            {
                coeff = std::exp (-1.0 / std::max (timeSeconds * sampleRate, 1.0));
                warmup = static_cast<std::uint64_t> (
                    std::llround (1.0 / std::max (1.0 - coeff, 1e-12)));
                reset();
            }

            void reset() noexcept { sum = value = 0.0; count = 0; }

            double process (double x) noexcept
            {
                ++count;

                if (count <= warmup)
                {
                    sum += x;
                    value = sum / static_cast<double> (count);
                }
                else
                {
                    value = (1.0 - coeff) * x + coeff * value;
                }

                return value;
            }

        private:
            double coeff { 0.0 }, sum { 0.0 }, value { 0.0 };
            std::uint64_t warmup { 0 }, count { 0 };
        };
    }

    class NoiseBed
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            controlRate = sr / kControlBlock;

            releaseCoeff = std::exp (-1.0 / std::max (kSidechainReleaseS * sr, 1.0));
            amountCoeff = std::exp (-1.0 / std::max (kAmountSmoothS * sr, 1.0));
            peakWindow = std::max (static_cast<int> (kSidechainAttackS * sr), 1);
            peakRing.assign (static_cast<size_t> (peakWindow), 0.0);

            followerMean.prepare (sr, kBedMatchS);
            wetRms.prepare (sr, kBedMatchS);
            flattenRms.prepare (sr, kBedMatchS);
            flattenCoeff = std::exp (-1.0 / std::max (0.015 * sr, 1.0));

            hold = std::max (static_cast<int> (0.003 * sr / kControlBlock), 1);
            jumpSmooth.prepare (controlRate, 0.001);

            // A tick keeps its fast onset: the shape is what makes it a tick.
            burstLength = std::max (static_cast<int> (0.008 * sr), 1);
            const auto burstAttack = std::max (static_cast<int> (0.0002 * sr), 1);
            burstShape.resize (static_cast<size_t> (burstLength));
            const auto burstStep = 6.0 / std::max (burstLength - 1, 1);
            for (int k = 0; k < burstLength; ++k)
                burstShape[static_cast<size_t> (k)]
                    = std::exp (-(static_cast<double> (k) * burstStep))
                    * std::min (static_cast<double> (k) / burstAttack, 1.0);

            grainLength = std::max (static_cast<int> (0.006 * sr), 1);
            const auto grainAttack = std::max (static_cast<int> (0.0005 * sr), 1);
            grainShape.resize (static_cast<size_t> (grainLength));
            const auto grainStep = 5.0 / std::max (grainLength - 1, 1);
            grainArea = 0.0;
            for (int k = 0; k < grainLength; ++k)
            {
                const auto v = std::exp (-(static_cast<double> (k) * grainStep))
                             * std::min (static_cast<double> (k) / grainAttack, 1.0);
                grainShape[static_cast<size_t> (k)] = v;
                grainArea += v;
            }

            radiationHpL.setCoefficients (highpass (2000.0, 0.8, sr));
            radiationHpR.setCoefficients (highpass (2000.0, 0.8, sr));
            radiationLpL.setCoefficients (lowpass (6000.0, 0.8, sr));
            radiationLpR.setCoefficients (lowpass (6000.0, 0.8, sr));
            sludgeLpL.setCoefficients (lowpass (420.0, 0.7, sr));
            sludgeLpR.setCoefficients (lowpass (420.0, 0.7, sr));
            sludgeHpL.setCoefficients (highpass (55.0, 0.7, sr));
            sludgeHpR.setCoefficients (highpass (55.0, 0.7, sr));

            engaged = false;
            amountNow = 0.0;
            target = 0.0;
            coldStart();
        }

        /// The knobs the beds read as the prototype does: SPREAD, REACTIVITY
        /// and CONTAINMENT as they sit, not as MELTDOWN is moving them.
        void configure (int reactionIndex, std::uint64_t newSeed, double newSpread,
                        double newReactivity, double newDamping) noexcept
        {
            const auto index = std::clamp (reactionIndex, 0, 4);

            if (index != reaction)
            {
                reaction = index;
                if (engaged)
                    coldStart();
            }

            seed = newSeed;
            spread = newSpread;
            reactivity = newReactivity;
            damping = newDamping;

            grainRate = 420.0 + 900.0 * reactivity;
            grainMean = std::max (grainRate * grainArea / sr, 1e-12);
        }

        /// CONTAMINATION as it should be now, MELTDOWN's stage included.
        void setAmount (double amount) noexcept { target = amount; }

        /// RADIATION's ticks are fired from events, on a sparse hashed subset.
        void trigger (const ScheduledEvent& e) noexcept
        {
            if (reaction != 0 || ! engaged)
                return;

            if (rng::urand ({ seed, 30, e.index }) >= 0.30)
                return;

            if (burstCount < static_cast<int> (bursts.size()))
                bursts[static_cast<size_t> (burstCount++)] = 0;
        }

        /// `position` is the absolute sample, `env` the placement's event
        /// envelope, and `dry` is what the engines were fed. Writes `wet` plus
        /// the bed, or `wet` untouched while CONTAMINATION is at zero.
        void process (std::int64_t position, double wetL, double wetR, double dryL, double dryR,
                      double env, double& outL, double& outR) noexcept
        {
            if (target <= 0.0 && amountNow <= 0.0)
            {
                engaged = false;
                outL = wetL;
                outR = wetR;
                return;
            }

            if (! engaged)
            {
                coldStart();
                engaged = true;
                amountNow = target;
            }
            else
            {
                amountNow = target + (amountNow - target) * amountCoeff;

                if (target <= 0.0 && amountNow < 1.0e-6)
                    amountNow = 0.0;
            }

            const auto slot = static_cast<size_t> (position % kControlBlock);

            if (slot == 0)
                tick (position / kControlBlock, env);

            double bedL = 0.0, bedR = 0.0;
            generate (position, bedL, bedR);

            // The sidechain is the input's envelope, normalised to average one:
            // a fast attack from a rolling peak and a slow release, both looking
            // back only, so the bed arrives with the source and falls in the gaps.
            const auto magnitude = std::max (std::abs (dryL), std::abs (dryR));
            peakRing[peakPos] = magnitude;
            peakPos = (peakPos + 1 == peakRing.size()) ? 0 : (peakPos + 1);
            const auto peak = *std::max_element (peakRing.begin(), peakRing.end());

            followerEnv = (1.0 - releaseCoeff) * peak + releaseCoeff * followerEnv;
            const auto follower = followerEnv / std::max (followerMean.process (followerEnv), 1e-12);

            // Referenced to the programme's level and to a constant, never to
            // the bed's own: a sparse bed decays towards silence between its
            // events, and dividing by its own level lets the gain run away in
            // the gaps.
            const auto wetLevel = wetRms.process (std::sqrt (0.5 * (wetL * wetL + wetR * wetR)));
            const auto& levels = kBedLevels[static_cast<size_t> (reaction)];
            const auto level = levels.fullLevel * amountNow * amountNow * (1.0 - 0.5 * damping);
            const auto scale = (1.0 - kSidechainDepth + kSidechainDepth * follower)
                             * (wetLevel / levels.unitRms) * level;

            const auto heldL = ringL[slot];
            const auto heldR = ringR[slot];
            ringL[slot] = bedL * scale;
            ringR[slot] = bedR * scale;

            const auto amp = ampPrevious + (ampNow - ampPrevious) * static_cast<double> (slot)
                           / kControlBlock;

            outL = wetL + heldL * amp;
            outR = wetR + heldR * amp;
        }

    private:
        static constexpr int kMaxVoices = 64;

        void coldStart() noexcept
        {
            std::fill (peakRing.begin(), peakRing.end(), 0.0);
            peakPos = 0;
            followerEnv = 0.0;
            followerMean.reset();
            wetRms.prepare (sr, kBedMatchS);
            flattenRms.prepare (sr, kBedMatchS);
            flattenEnv = 0.0;

            ringL.fill (0.0);
            ringR.fill (0.0);
            ampPrevious = ampNow = 1.0;
            ticked = false;

            envSmooth.prepare (controlRate, smoothTime());
            jumpSmooth.prepare (controlRate, 0.001);

            for (auto* f : { &radiationHpL, &radiationHpR, &radiationLpL, &radiationLpR,
                             &sludgeLpL, &sludgeLpR, &sludgeHpL, &sludgeHpR })
                f->reset();

            for (auto& f : swept)
                f.reset();

            burstCount = 0;
            grainCount = 0;
        }

        /// How slowly each bed breathes with the event envelope.
        double smoothTime() const noexcept
        {
            switch (reaction)
            {
                case 1:  return 0.05;
                case 2:  return 0.25;
                case 4:  return 0.12;
                default: return 0.0;
            }
        }

        /// Control rate: the filters sweep and the envelope is sampled here.
        void tick (std::int64_t block, double env) noexcept
        {
            // As the prototype: a control block's time is its index over the
            // control rate, which is not quite the same sum as samples over sr.
            const auto t = static_cast<double> (block) / (sr / kControlBlock);
            const auto before = ampNow;

            switch (reaction)
            {
                case 1:
                {
                    const auto drift = 0.5 - 0.5 * std::cos (2.0 * M_PI * 0.07 * t);
                    static constexpr double octaves[] { -0.65, 0.0, 0.8 };

                    for (int i = 0; i < 3; ++i)
                    {
                        const auto divergence = octaves[i] * (0.5 + 1.5 * spread);
                        const auto fc = 1400.0 * std::pow (2.0, drift * divergence);
                        swept[static_cast<size_t> (2 * i)].set (fc, 9.0, sr);
                        swept[static_cast<size_t> (2 * i + 1)].set (fc, 9.0, sr);
                    }

                    ampNow = 0.35 + 0.65 * envSmooth.process (env);
                    break;
                }

                case 2:
                    ampNow = 0.30 + 0.70 * envSmooth.process (env);
                    break;

                case 3:
                {
                    const auto step = static_cast<std::uint64_t> (block / hold);
                    const auto jump = jumpSmooth.process (rng::urand ({ seed, 50, step }));
                    const auto fc = 600.0 * std::pow (2.0, 2.7 * jump);
                    swept[0].set (fc, 4.0, sr);
                    swept[1].set (fc, 4.0, sr);
                    ampNow = 1.0;
                    break;
                }

                case 4:
                {
                    const auto drift = 0.5 - 0.5 * std::cos (2.0 * M_PI * 0.08 * t);
                    const auto base = 700.0 * std::pow (2.0, 1.2 * drift);

                    for (int channel = 0; channel < 2; ++channel)
                    {
                        const auto phase = channel == 0 ? 0.0 : M_PI * 0.5;
                        const auto rotation = std::sin (2.0 * M_PI * 5.0 * t + phase)
                                            + 0.7 * std::sin (2.0 * M_PI * 5.9 * t + phase * 1.3);
                        swept[static_cast<size_t> (channel)]
                            .set (base * std::pow (2.0, 0.55 * rotation), 11.0, sr);
                    }

                    ampNow = 0.45 + 0.55 * envSmooth.process (env);
                    break;
                }

                default:
                    ampNow = 1.0;
                    break;
            }

            ampPrevious = ticked ? before : ampNow;
            ticked = true;
        }

        /// One sample of this reaction's bed, before any level is applied.
        void generate (std::int64_t position, double& bedL, double& bedR) noexcept
        {
            const auto index = static_cast<std::uint64_t> (position);

            switch (reaction)
            {
                case 0:
                {
                    const auto stream = seed + kBedStreamRadiation;
                    auto l = radiationLpL.process (radiationHpL.process (detail::gaussian (stream, 0, index)));
                    auto r = radiationLpR.process (radiationHpR.process (detail::gaussian (stream, 1, index)));

                    // Overlapping ticks take the louder, not the sum.
                    auto burst = 0.0;
                    for (int i = 0; i < burstCount;)
                    {
                        auto& age = bursts[static_cast<size_t> (i)];
                        burst = std::max (burst, burstShape[static_cast<size_t> (age)]);

                        if (++age >= burstLength)
                            bursts[static_cast<size_t> (i)] = bursts[static_cast<size_t> (--burstCount)];
                        else
                            ++i;
                    }

                    bedL = l * burst;
                    bedR = r * burst;
                    break;
                }

                case 1:
                {
                    const auto stream = seed + kBedStreamFission;
                    const auto nL = detail::gaussian (stream, 0, index);
                    const auto nR = detail::gaussian (stream, 1, index);

                    bedL = bedR = 0.0;
                    for (int i = 0; i < 3; ++i)
                    {
                        const auto pan = 0.5 + 0.5 * std::cos (i * 2.1);
                        bedL += swept[static_cast<size_t> (2 * i)].process (nL) * pan;
                        bedR += swept[static_cast<size_t> (2 * i + 1)].process (nR) * (1.0 - pan);
                    }
                    break;
                }

                case 2:
                {
                    const auto stream = seed + kBedStreamSludge;
                    bedL = std::tanh (sludgeHpL.process (sludgeLpL.process (detail::gaussian (stream, 0, index))) * 1.6);
                    bedR = std::tanh (sludgeHpR.process (sludgeLpR.process (detail::gaussian (stream, 1, index))) * 1.6);
                    break;
                }

                case 3:
                {
                    const auto stream = seed + kBedStreamChemical;
                    auto l = swept[0].process (detail::gaussian (stream, 0, index));
                    auto r = swept[1].process (detail::gaussian (stream, 1, index));

                    // A swept resonance swings in level as much as in timbre,
                    // which reads as separate events. Take the swing out, and
                    // keep the movement.
                    const auto magnitude = 0.5 * (std::abs (l) + std::abs (r));
                    flattenEnv = (1.0 - flattenCoeff) * magnitude + flattenCoeff * flattenEnv;
                    const auto reference = flattenRms.process (magnitude);
                    const auto gain = std::pow ((flattenEnv + 1e-9) / reference, 0.8);
                    l /= gain;
                    r /= gain;

                    // Grains overlap several deep, so they fuse into a fizz.
                    if (rng::urand ({ seed, 51, index }) < grainRate / sr
                        && grainCount < kMaxVoices)
                        grainAges[static_cast<size_t> (grainCount++)] = 0;

                    auto grains = 0.0;
                    for (int i = 0; i < grainCount;)
                    {
                        auto& age = grainAges[static_cast<size_t> (i)];
                        grains += grainShape[static_cast<size_t> (age)];

                        if (++age >= grainLength)
                            grainAges[static_cast<size_t> (i)] = grainAges[static_cast<size_t> (--grainCount)];
                        else
                            ++i;
                    }

                    bedL = l * grains / grainMean;
                    bedR = r * grains / grainMean;
                    break;
                }

                default:
                {
                    const auto stream = seed + kBedStreamAlien;
                    bedL = swept[0].process (detail::gaussian (stream, 0, index));
                    bedR = swept[1].process (detail::gaussian (stream, 1, index));
                    break;
                }
            }
        }

        double sr { 44100.0 }, controlRate { 5512.5 };
        int reaction { 0 };
        std::uint64_t seed { 0 };
        double spread { 0.0 }, reactivity { 0.0 }, damping { 0.0 };

        double target { 0.0 }, amountNow { 0.0 }, amountCoeff { 0.0 };
        bool engaged { false };

        // The sidechain follower and the level references.
        double releaseCoeff { 0.0 }, followerEnv { 0.0 };
        int peakWindow { 220 };
        std::vector<double> peakRing;
        size_t peakPos { 0 };
        detail::RunningMean followerMean;
        RunningRms wetRms;

        // Eight samples of finished bed, waiting for the envelope's next value.
        std::array<double, kControlBlock> ringL {}, ringR {};
        double ampPrevious { 1.0 }, ampNow { 1.0 };
        bool ticked { false };
        OnePole envSmooth;

        // RADIATION
        Biquad radiationHpL, radiationHpR, radiationLpL, radiationLpR;
        std::array<int, 16> bursts {};
        int burstCount { 0 }, burstLength { 352 };
        std::vector<double> burstShape;

        // SLUDGE
        Biquad sludgeLpL, sludgeLpR, sludgeHpL, sludgeHpR;

        // FISSION (three bands, left and right), CHEMICAL and ALIEN (left and right)
        std::array<detail::SweptBandpass, 6> swept;

        // CHEMICAL
        OnePole jumpSmooth;
        int hold { 16 };
        double flattenCoeff { 0.0 }, flattenEnv { 0.0 };
        RunningRms flattenRms;
        std::array<int, kMaxVoices> grainAges {};
        int grainCount { 0 }, grainLength { 264 };
        std::vector<double> grainShape;
        double grainArea { 1.0 }, grainRate { 420.0 }, grainMean { 1.0 };
    };
}
