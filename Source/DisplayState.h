#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "Dsp/Placement.h"

namespace squelch
{
/** Everything the display is allowed to draw, published once per block.

    Fixed size, so publishing it allocates nothing: the events are a ring of the
    last `maxEvents` with a running total, and the voice pool is already bounded
    at sixteen. Every field is a value the audio thread produced.
*/
struct DisplayState
{
    static constexpr int maxEvents = 32;
    static constexpr int maxSteps = 48;
    static constexpr int maxVoices = dsp::Placement::kMaxVoices;

    /** One fired event, carrying the cutoff curve the envelope built for it, so
        the display plots the same closed form the audio thread is playing rather
        than sampling a per-sample value at frame rate. */
    struct Event
    {
        std::int64_t start = 0;
        float restOct = 0.0f, peakOct = 0.0f, tauSeconds = 0.1f;
        float slideFromOct = 0.0f, slideSeconds = 0.0f;

        /// Cutoff octave at which this event's feedback reaches self-oscillation, or < 0 if it never does.
        float ringOct = -1.0f;

        float intensity = 1.0f, pan = 0.0f, drive = 1.0f;
        bool accent = false, slide = false;
    };

    /// A grid step and whether it passed the probability gate.
    struct Step
    {
        std::int64_t start = 0;
        bool fired = false;
    };

    /// One placement voice, as the control-rate pool holds it.
    struct Voice
    {
        float env = 0.0f, panL = 0.0f, panR = 0.0f, send = 0.0f;
    };

    std::array<Event, maxEvents> events {};
    unsigned eventCount = 0;

    std::array<Step, maxSteps> steps {};
    unsigned stepCount = 0;

    std::array<Voice, maxVoices> voices {};
    int liveVoices = 0;

    float cutoffHz = 180.0f, heldHz = 180.0f, feedback = 0.0f, drive = 1.0f;

    /// The highest feedback this EXPOSURE can reach, against dsp::kSelfOscillation.
    float peakFeedback = 0.0f;

    float stepSeconds = 0.25f;

    /// The audio thread's own monotonic sample count at the end of the block. Not
    /// the host timeline, which jumps on a locate and would drag the display with it.
    std::int64_t nowSample = 0;
    double sampleRate = 44100.0;

    int reaction = 0, mode = 0, gridIndex = 8;
    bool meltdown = false, ionize = false;
};

/** A seqlock. The audio thread writes a whole snapshot without ever blocking or
    allocating; the message thread retries if it catches one mid-write. */
template <typename T>
class Published
{
public:
    void write (const T& next) noexcept
    {
        const auto start = sequence.load (std::memory_order_relaxed);
        sequence.store (start + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        value = next;
        std::atomic_thread_fence (std::memory_order_release);
        sequence.store (start + 2, std::memory_order_relaxed);
    }

    T read() const noexcept
    {
        T out;

        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const auto before = sequence.load (std::memory_order_relaxed);

            if ((before & 1u) != 0u)
                continue;

            std::atomic_thread_fence (std::memory_order_acquire);
            out = value;
            std::atomic_thread_fence (std::memory_order_acquire);

            if (sequence.load (std::memory_order_relaxed) == before)
                break;
        }

        return out;
    }

private:
    T value {};
    std::atomic<unsigned> sequence { 0 };
};
}
