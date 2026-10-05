/** Checks that run the whole processor rather than one engine.

    Validate.cpp drives the engines directly, so it cannot see a parameter that
    never reaches them: ENRICHMENT sat unwired in the plugin while every one of
    those checks passed. These go through SquelchAudioProcessor::processBlock,
    which is the only place a control's wiring can be observed.

    Lines start [PASS] / [FAIL] so scripts/fault-injection.py reads them the way
    it reads Validate's.
*/

#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "../Source/Parameters.h"
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"
#include "../Source/Dsp/Meltdown.h"
#include "../Source/Dsp/NoiseBed.h"

namespace
{
constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;
constexpr int numBlocks = 172; // about two seconds

int failures = 0;

void report (bool pass, const char* name, double measured, const char* detail)
{
    std::printf ("[%s] %s: %.2f %s\n", pass ? "PASS" : "FAIL", name, measured, detail);
    failures += pass ? 0 : 1;
}

void setParameter (SquelchAudioProcessor& processor, const char* id, float normalised)
{
    processor.apvts.getParameter (id)->setValueNotifyingHost (normalised);
}

/** Two seconds of deterministic noise through a fresh processor, left channel. */
std::vector<float> render (int reactionIndex, float enrichment, float mix, bool meltdownHeld = false)
{
    SquelchAudioProcessor processor;
    processor.mix = mix;

    const auto* reaction = processor.apvts.getParameter (squelch::ids::reaction);
    setParameter (processor, squelch::ids::reaction, reaction->convertTo0to1 (static_cast<float> (reactionIndex)));
    setParameter (processor, squelch::ids::enrichment, enrichment);
    setParameter (processor, squelch::ids::meltdown, meltdownHeld ? 1.0f : 0.0f);

    processor.prepareToPlay (sampleRate, blockSize);

    std::vector<float> out;
    juce::MidiBuffer midi;
    std::uint32_t state = 12345u;

    for (int b = 0; b < numBlocks; ++b)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);

        for (int i = 0; i < blockSize; ++i)
        {
            state = state * 1664525u + 1013904223u;
            const auto v = 0.25f * (static_cast<float> (state >> 8) / 8388608.0f - 1.0f);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        processor.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    return out;
}

double rms (const std::vector<float>& x)
{
    double sum = 0.0;
    for (auto v : x)
        sum += static_cast<double> (v) * v;
    return std::sqrt (sum / static_cast<double> (std::max<size_t> (x.size(), 1)));
}

double db (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1.0e-12)); }

std::vector<float> difference (const std::vector<float>& a, const std::vector<float>& b)
{
    std::vector<float> d (a.size());
    for (size_t i = 0; i < a.size(); ++i)
        d[i] = a[i] - b[i];
    return d;
}

/** A longer render with knobs set and MELTDOWN held for a stretch from the start. */
struct Session
{
    int reaction { 0 };
    double seconds { 2.0 };
    int block { blockSize };

    /// MELTDOWN is held for this long from the first sample; negative never presses it.
    double meltdownSeconds { -1.0 };

    std::vector<std::pair<const char*, float>> knobs;
};

std::vector<float> renderSession (const Session& s)
{
    SquelchAudioProcessor processor;

    const auto* reaction = processor.apvts.getParameter (squelch::ids::reaction);
    setParameter (processor, squelch::ids::reaction, reaction->convertTo0to1 (static_cast<float> (s.reaction)));

    for (const auto& knob : s.knobs)
        setParameter (processor, knob.first, knob.second);

    processor.prepareToPlay (sampleRate, s.block);

    const auto total = static_cast<int> (s.seconds * sampleRate);
    std::vector<float> out;
    juce::MidiBuffer midi;
    std::uint32_t state = 12345u;

    for (int done = 0; done < total; done += s.block)
    {
        setParameter (processor, squelch::ids::meltdown,
                      done / sampleRate < s.meltdownSeconds ? 1.0f : 0.0f);

        juce::AudioBuffer<float> buffer (2, s.block);

        for (int i = 0; i < s.block; ++i)
        {
            state = state * 1664525u + 1013904223u;
            const auto v = 0.25f * (static_cast<float> (state >> 8) / 8388608.0f - 1.0f);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        processor.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + s.block);
    }

    out.resize (static_cast<size_t> (total));
    return out;
}

double rmsBetween (const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    const auto a = static_cast<size_t> (fromSeconds * sampleRate);
    const auto b = std::min (static_cast<size_t> (toSeconds * sampleRate), x.size());
    return rms (std::vector<float> (x.begin() + static_cast<std::ptrdiff_t> (a),
                                    x.begin() + static_cast<std::ptrdiff_t> (b)));
}

/** One noise bed on a steady 200 Hz tone, as prototype/checks.py measures it. */
struct BedRun { double diffRms, outRms; double exactZero; };

BedRun runBed (int reaction, double amount, double wetScale)
{
    using namespace squelch::dsp;

    constexpr int n = 4 * 44100;
    constexpr int blocks = (n + kControlBlock - 1) / kControlBlock;

    NoiseBed bed;
    bed.prepare (sampleRate);
    bed.configure (reaction, 0, 0.75, 0.3, 0.0);
    bed.setAmount (amount);

    double diffSum = 0.0, outSum = 0.0, worst = 0.0, env = 0.2;

    for (int i = 0; i < n; ++i)
    {
        const auto wet = wetScale * 0.15 * std::sin (2.0 * M_PI * 200.0 * i / sampleRate);

        if (i % kControlBlock == 0)
            env = 0.2 + 0.8 * (i / kControlBlock) / (blocks - 1);

        if (i % 5512 == 0)
        {
            ScheduledEvent e;
            e.start = i;
            e.index = static_cast<std::uint64_t> (i / 5512);
            bed.trigger (e);
        }

        double l = 0.0, r = 0.0;
        bed.process (i, wet, wet, wet, wet, env, l, r);

        diffSum += (l - wet) * (l - wet) + (r - wet) * (r - wet);
        outSum += l * l + r * r;
        worst = std::max (worst, std::abs (l - wet));
    }

    return { std::sqrt (diffSum / (2.0 * n)), std::sqrt (outSum / (2.0 * n)), worst };
}

/// The first block boundary at or after a time, which is when a press made then is seen.
int blockAligned (double seconds, int block)
{
    return static_cast<int> (std::ceil (seconds * sampleRate / block)) * block;
}

juce::MouseEvent makeMouseEvent (juce::Component& component)
{
    const auto now = juce::Time::getCurrentTime();
    const juce::Point<float> where { 4.0f, 4.0f };

    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), where,
                             juce::ModifierKeys(), juce::MouseInputSource::defaultPressure,
                             juce::MouseInputSource::defaultOrientation,
                             juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX,
                             juce::MouseInputSource::defaultTiltY, &component, &component, now,
                             where, now, 1, false);
}

/** A render in which MELTDOWN is pressed and released by the editor's own button.

    Not mouse automation: the button's press and release handlers are called
    directly, which is the whole of what it does to the plugin, and they set the
    parameter that processBlock reads. A host sees nothing else of it.
*/
struct Timeline
{
    int reaction { 0 };
    double seconds { 2.0 };
    int block { blockSize };

    /// Sample at which each is made, which must be a block boundary. Negative never.
    int pressAt { -1 };
    int releaseAt { -1 };

    std::vector<std::pair<const char*, float>> knobs;
};

std::vector<float> renderTimeline (const Timeline& t)
{
    SquelchAudioProcessor processor;

    const auto* reaction = processor.apvts.getParameter (squelch::ids::reaction);
    setParameter (processor, squelch::ids::reaction, reaction->convertTo0to1 (static_cast<float> (t.reaction)));

    for (const auto& knob : t.knobs)
        setParameter (processor, knob.first, knob.second);

    processor.prepareToPlay (sampleRate, t.block);

    MomentaryGesture button (*processor.apvts.getParameter (squelch::ids::meltdown), "Meltdown", "");
    const auto event = makeMouseEvent (button);

    const auto total = static_cast<int> (t.seconds * sampleRate);
    std::vector<float> out;
    juce::MidiBuffer midi;
    std::uint32_t state = 12345u;
    auto pressed = false, released = false;

    for (int done = 0; done < total; done += t.block)
    {
        if (! pressed && t.pressAt >= 0 && done >= t.pressAt)
        {
            button.mouseDown (event);
            pressed = true;
        }

        if (pressed && ! released && t.releaseAt >= 0 && done >= t.releaseAt)
        {
            button.mouseUp (event);
            released = true;
        }

        juce::AudioBuffer<float> buffer (2, t.block);

        for (int i = 0; i < t.block; ++i)
        {
            state = state * 1664525u + 1013904223u;
            const auto v = 0.25f * (static_cast<float> (state >> 8) / 8388608.0f - 1.0f);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        processor.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + t.block);
    }

    out.resize (static_cast<size_t> (total));
    return out;
}

/// Index of the first sample two renders disagree at, or -1 if they never do.
long long firstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    const auto n = std::min (a.size(), b.size());

    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return static_cast<long long> (i);

    return -1;
}
} // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    // What the listener hears of ENRICHMENT: the processed output with the
    // knob at either end of its travel, as a level relative to the output
    // itself. By the scale in the notes, -20 dB is subtle and -26 is inaudible,
    // so a control that does something must clear -20.
    for (const auto* name : { "RADIATION", "FISSION", "SLUDGE", "CHEMICAL" })
    {
        const auto index = squelch::reactionNames.indexOf (name);

        const auto low = render (index, 0.0f, 1.0f);
        const auto high = render (index, 1.0f, 1.0f);
        const auto relative = db (rms (difference (high, low)) / rms (high));

        char label[96];
        std::snprintf (label, sizeof label, "Processor ENRICHMENT changes the %s output", name);
        report (relative > -20.0, label, relative, "dB (needs > -20)");
    }

    // ENRICHMENT drives the engines only. At mix 0 nothing but the dry path
    // reaches the output, so the knob must not move a single sample.
    {
        const auto low = render (0, 0.0f, 0.0f);
        const auto high = render (0, 1.0f, 0.0f);
        const auto worst = rms (difference (high, low));
        report (worst == 0.0, "Processor ENRICHMENT leaves the dry path alone", db (worst),
                "dB (needs exactly 0 difference)");
    }

    // MELTDOWN held from the first sample must move every reaction, measured the
    // way ENRICHMENT is: the held render against the idle one, relative to itself.
    for (const auto* name : { "RADIATION", "FISSION", "SLUDGE", "CHEMICAL", "ALIEN" })
    {
        const auto index = squelch::reactionNames.indexOf (name);
        const auto idle = render (index, 0.5f, 1.0f);
        const auto held = render (index, 0.5f, 1.0f, true);
        const auto relative = db (rms (difference (held, idle)) / rms (held));

        char label[96];
        std::snprintf (label, sizeof label, "Processor MELTDOWN changes the %s output", name);
        report (relative > -20.0, label, relative, "dB (needs > -20)");
    }

    // The stages arrive in order, and a short tap does not reach the late ones.
    {
        using namespace squelch::dsp;
        constexpr double sr = 44100.0;

        Meltdown tap;
        tap.prepare (sr);
        tap.setGate (true);
        tap.advance (static_cast<int> (0.3 * sr));
        tap.setGate (false);

        const auto contamination = tap.progressOf (Staged::contamination);
        const auto exposure = tap.progressOf (Staged::exposure);
        const auto containment = tap.progressOf (Staged::containment);

        report (containment > 0.99 && exposure < 0.1 && contamination == 0.0,
                "Processor MELTDOWN stages in, and a tap stops short", exposure,
                "exposure progress after 0.3 s (containment 1, contamination 0)");
    }

    // The state after a stretch of samples must not depend on how the host cut it up.
    {
        using namespace squelch::dsp;
        constexpr double sr = 44100.0;

        const auto runWith = [] (int chunk)
        {
            Meltdown m;
            m.prepare (sr);
            m.setGate (true);
            for (int done = 0; done < 66150; done += chunk)
                m.advance (chunk);
            m.setGate (false);
            for (int done = 0; done < 44100; done += chunk)
                m.advance (chunk);
            return m;
        };

        const auto whole = runWith (1);
        double worst = 0.0;
        for (const auto chunk : { 7, 49, 441, 2205 })
        {
            const auto other = runWith (chunk);
            for (std::size_t s = 0; s < static_cast<std::size_t> (Staged::count); ++s)
                worst = std::max (worst, std::abs (whole.progressOf (static_cast<Staged> (s))
                                                   - other.progressOf (static_cast<Staged> (s))));
        }

        report (worst < 1.0e-9, "Processor MELTDOWN does not depend on the block size", worst,
                "worst progress difference (needs < 1e-9)");
    }

    // CONTAMINATION. The beds are tested alone for their level, and through
    // processBlock for everything that depends on how they are wired.
    {
        // Exactly nothing at zero: the bed does not touch the signal at all.
        double worst = 0.0;
        for (int reaction = 0; reaction < 5; ++reaction)
            worst = std::max (worst, runBed (reaction, 0.0, 1.0).exactZero);

        report (worst == 0.0, "Processor CONTAMINATION adds nothing at zero", worst, "(needs exactly 0)");
    }

    {
        // Against the output at full travel, as noise-beds.md specifies, and 12 dB
        // below that at half. The targets are written out here and not read back
        // from the profile, or the goal would move with the constant being tested.
        const double targets[] { -28.0, -18.0, -14.0, -23.0, -16.0 };
        const char* names[] { "RADIATION", "FISSION", "SLUDGE", "CHEMICAL", "ALIEN" };

        for (int reaction = 0; reaction < 5; ++reaction)
        {
            const auto half = runBed (reaction, 0.5, 1.0);
            const auto full = runBed (reaction, 1.0, 1.0);
            const auto fullDb = db (full.diffRms / full.outRms);
            const auto halfDb = db (half.diffRms / half.outRms);

            char label[96], detail[96];
            std::snprintf (label, sizeof label, "Processor CONTAMINATION level of the %s bed", names[reaction]);
            std::snprintf (detail, sizeof detail, "dB at full (target %.0f, half %.1f dB below)",
                           targets[reaction], fullDb - halfDb);
            report (std::abs (fullDb - targets[reaction]) < 1.5 && std::abs ((fullDb - halfDb) - 12.0) < 1.5,
                    label, fullDb, detail);
        }
    }

    {
        // Referenced to the output's own level, so it follows the output.
        const auto loud = runBed (1, 1.0, 1.0);
        const auto quiet = runBed (1, 1.0, 0.3);
        const auto offset = db (loud.diffRms / loud.outRms) - db (quiet.diffRms / quiet.outRms);
        report (std::abs (offset) < 0.5, "Processor CONTAMINATION follows the output's level", offset,
                "dB between a loud and a quiet source (needs < 0.5)");
    }

    const auto makeSession = [] (int reaction, double seconds, double meltdownSeconds,
                                 std::vector<std::pair<const char*, float>> knobs, int block = blockSize)
    {
        Session s;
        s.reaction = reaction;
        s.seconds = seconds;
        s.meltdownSeconds = meltdownSeconds;
        s.knobs = std::move (knobs);
        s.block = block;
        return s;
    };

    // More CONTAMINATION is more bed, in every reaction, measured through the
    // processor against the same render with the control at zero.
    for (const auto* name : { "RADIATION", "FISSION", "SLUDGE", "CHEMICAL", "ALIEN" })
    {
        const auto index = squelch::reactionNames.indexOf (name);
        const auto none = renderSession (makeSession (index, 2.0, -1.0, { { squelch::ids::contamination, 0.0f } }));

        auto rising = true;
        auto previous = 0.0;
        for (const auto amount : { 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const auto with = renderSession (makeSession (index, 2.0, -1.0, { { squelch::ids::contamination, amount } }));
            const auto bed = rms (difference (with, none));
            rising = rising && bed > previous;
            previous = bed;
        }

        char label[96];
        std::snprintf (label, sizeof label, "Processor CONTAMINATION grows with the control in %s", name);
        report (rising, label, db (previous / rms (none)), "dB of bed at full, against the render without it");
    }

    {
        // MELTDOWN's last stage must reach the bed. Every other staged parameter is
        // parked on its target first, so the gesture can change nothing else and what
        // differs from the idle render is the bed alone.
        const std::vector<std::pair<const char*, float>> atTargets
        {
            { squelch::ids::containment, 0.0f }, { squelch::ids::probability, 1.0f },
            { squelch::ids::spread, 1.0f },      { squelch::ids::drive, 1.0f },
            { squelch::ids::reactivity, 0.85f }, { squelch::ids::toxicity, 1.0f },
            { squelch::ids::exposure, 1.0f },    { squelch::ids::contamination, 0.0f },
        };

        constexpr int fission = 1;
        const auto idle = renderSession (makeSession (fission, 7.0, -1.0, atTargets));
        const auto held = renderSession (makeSession (fission, 7.0, 5.0, atTargets));
        const auto tap = renderSession (makeSession (fission, 7.0, 0.3, atTargets));
        const auto heldBed = difference (held, idle);
        const auto tapBed = difference (tap, idle);
        const auto reference = rms (idle);

        // A float knob cannot sit exactly on a double target, so this is a figure
        // just above rounding and not a zero.
        const auto early = db (rmsBetween (heldBed, 0.0, 0.6) / reference);
        report (early < -100.0, "Processor MELTDOWN leaves the bed alone before its stage", early,
                "dB until 0.66 s (needs < -100)");

        const auto tapped = db (rms (tapBed) / reference);
        report (tapped < -100.0, "Processor MELTDOWN tap never reaches the bed", tapped,
                "dB over the whole render (needs < -100)");

        // The stage's progress is 0.45 at 1.0 s, 0.85 at 1.3 s and 1 from 1.41 s on, and
        // the bed rises through that. The check is on the shape and not the exponent: it
        // is not progress squared as it is alone, because DRIVE here is parked at 1 and
        // saturates what the bed adds. Nor is it steady afterwards, because the level
        // match downstream tracks over seconds and keeps settling.
        const auto w1 = rmsBetween (heldBed, 0.9, 1.1);
        const auto w2 = rmsBetween (heldBed, 1.2, 1.4);
        const auto w3 = rmsBetween (heldBed, 2.0, 2.2);
        report (w3 > 0.0 && w1 < w2 && w2 < w3 && w1 / w3 > 0.20 && w1 / w3 < 0.70,
                "Processor MELTDOWN bed follows the staged envelope", w1 / w3,
                "of the full bed at 1.0 s (needs 0.2-0.7, and rising through 1.3 s)");

        // Released at 5 s, the stage falls with a 1.13 s time constant.
        const auto before = rmsBetween (heldBed, 4.5, 4.9);
        const auto after = rmsBetween (heldBed, 6.8, 7.0);
        report (before > 0.0 && after / before < 0.15, "Processor MELTDOWN bed falls back on release",
                after / before, "of the full bed 1.8 s after release (needs < 0.15)");

        // Everything above sets the parameter directly. The editor's button is the only
        // other thing that does, so make the same press and release with it and require
        // the same render, sample for sample. The release is at a block boundary, which
        // is the only place a host can see one.
        const auto release = blockAligned (5.0, blockSize);
        const auto pressedHeld = renderTimeline ({ fission, 7.0, blockSize, 0, release, atTargets });
        const auto pressedTap = renderTimeline ({ fission, 7.0, blockSize, 0, blockAligned (0.3, blockSize), atTargets });

        const auto heldDiffers = firstDifference (pressedHeld, held);
        report (heldDiffers < 0 && pressedHeld.size() == held.size(),
                "Processor MELTDOWN button press and release render as the parameter does",
                static_cast<double> (heldDiffers), "first differing sample, held (-1 is none)");

        const auto tapDiffers = firstDifference (pressedTap, tap);
        report (tapDiffers < 0 && pressedTap.size() == tap.size(),
                "Processor MELTDOWN button tap renders as the parameter does",
                static_cast<double> (tapDiffers), "first differing sample, tap (-1 is none)");

        // The staged state does not depend on the host's block size, so neither does the
        // bed it drives, up to the one thing that does: the values are read once per
        // block, so a ramp is followed in steps of the block's length.
        const auto small = renderTimeline ({ fission, 7.0, 128, 0, release, atTargets });
        const auto smallBed = difference (small, idle);
        const auto w3Small = rmsBetween (smallBed, 2.0, 2.2);
        const auto w1Small = rmsBetween (smallBed, 0.9, 1.1);
        const auto stepError = db (rms (difference (smallBed, heldBed)) / rms (heldBed));
        char detail[128];
        std::snprintf (detail, sizeof detail,
                       "dB between 128 and 512 sample blocks (needs < -20; full-bed ratio at 1.0 s %.2f against %.2f)",
                       w1Small / w3Small, w1 / w3);
        report (stepError < -20.0 && std::abs (w1Small / w3Small - w1 / w3) < 0.05,
                "Processor MELTDOWN pressed bed does not depend on the block size", stepError, detail);
    }

    {
        // The bed is a function of the sample and its control block, not of how the host
        // cut the audio up, so a small block and a large one give the same bed.
        const auto bedAt = [&] (int block)
        {
            const auto with = renderSession (makeSession (1, 2.0, -1.0, { { squelch::ids::contamination, 0.8f } }, block));
            const auto without = renderSession (makeSession (1, 2.0, -1.0, { { squelch::ids::contamination, 0.0f } }, block));
            return difference (with, without);
        };

        const auto large = bedAt (512);
        const auto small = bedAt (128);
        const auto error = db (rms (difference (large, small)) / rms (large));
        report (error < -30.0, "Processor CONTAMINATION does not depend on the block size", error,
                "dB between 512 and 128 sample blocks (needs < -30)");
    }

    // The button itself: pressing it turns MELTDOWN on and releasing it turns it off.
    {
        SquelchAudioProcessor processor;
        MomentaryGesture button (*processor.apvts.getParameter (squelch::ids::meltdown), "Meltdown", "");
        const auto event = makeMouseEvent (button);
        const auto raw = [&] { return processor.apvts.getRawParameterValue (squelch::ids::meltdown)->load(); };

        const auto idle = raw();
        button.mouseDown (event);
        const auto held = raw();
        button.mouseUp (event);
        const auto let = raw();

        report (idle == 0.0f && held == 1.0f && let == 0.0f,
                "Processor MELTDOWN button engages on press and lets go on release", held,
                "while held (off, on, off needed)");
    }

    // A press leaves the render exactly as it was up to and including the block it was
    // made in, so the staged values start from the knobs and nothing jumps, and then
    // moves it within a fraction of a second.
    {
        constexpr int press = 100 * blockSize;

        Timeline idleTimeline;
        idleTimeline.seconds = 2.0;
        auto pressedTimeline = idleTimeline;
        pressedTimeline.pressAt = press;

        const auto idle = renderTimeline (idleTimeline);
        const auto pressed = renderTimeline (pressedTimeline);
        const auto first = firstDifference (idle, pressed);
        const auto after = first < 0 ? -1.0 : (first - press) / sampleRate;

        report (first >= press + blockSize && after < 0.2,
                "Processor MELTDOWN press starts from the knobs", after,
                "s from the press to the first changed sample (needs a block to 0.2)");
    }

    // The staged envelopes themselves, through the state the processor runs.
    {
        using namespace squelch::dsp;
        constexpr double sr = 44100.0;
        constexpr double knob = 0.37;
        constexpr auto count = static_cast<std::size_t> (Staged::count);

        const auto progress = [] (const Meltdown& m, std::size_t i) { return m.progressOf (static_cast<Staged> (i)); };

        // The instant of a press: every stage is exactly at its knob.
        {
            Meltdown m;
            m.prepare (sr);
            m.setGate (true);

            auto atKnob = true;
            for (std::size_t i = 0; i < count; ++i)
                atKnob = atKnob && progress (m, i) == 0.0
                         && m.value (static_cast<Staged> (i), knob) == knob;

            report (atKnob && m.active(), "Processor MELTDOWN stages start at their knobs when pressed",
                    atKnob ? 1.0 : 0.0, "(all eight exactly at the knob, and active)");
        }

        // Held, the stages arrive in order, each by its own schedule. Written out by hand
        // from the prototype's table: at 0.3 s the rods are out, the fallout has not
        // begun, and the rest are part way.
        {
            const double expected[count] { 1.0, 1.0, 0.54, 0.24 / 0.26, 0.2 / 0.3, 0.1 / 0.36, 0.0, 0.0 };

            Meltdown m;
            m.prepare (sr);
            m.setGate (true);
            m.advance (static_cast<int> (0.3 * sr));

            auto worst = 0.0;
            for (std::size_t i = 0; i < count; ++i)
                worst = std::max (worst, std::abs (progress (m, i) - expected[i]));

            Meltdown full;
            full.prepare (sr);
            full.setGate (true);
            full.advance (static_cast<int> (2.0 * sr));

            auto reached = true;
            for (std::size_t i = 0; i < count; ++i)
                reached = reached && progress (full, i) == 1.0;

            report (worst < 1.0e-9 && reached, "Processor MELTDOWN held press reaches the stages in order",
                    worst, "worst error against the hand figures at 0.3 s, and all eight at 1 by 2 s");
        }

        // Released, every stage falls back towards its knob by its own time constant,
        // which is a third of its release time, and never rises on the way.
        {
            const double releaseSeconds[count] { 1.4, 0.9, 1.8, 1.2, 1.1, 1.6, 2.1, 3.4 };

            Meltdown m;
            m.prepare (sr);
            m.setGate (true);
            m.advance (static_cast<int> (2.0 * sr));
            m.setGate (false);

            std::array<double, count> previous {};
            for (std::size_t i = 0; i < count; ++i)
                previous[i] = progress (m, i);

            auto never = true;
            for (int step = 0; step < 10; ++step)
            {
                m.advance (static_cast<int> (0.1 * sr));
                for (std::size_t i = 0; i < count; ++i)
                {
                    never = never && progress (m, i) <= previous[i];
                    previous[i] = progress (m, i);
                }
            }

            m.advance (static_cast<int> (sr));
            auto worst = 0.0;
            for (std::size_t i = 0; i < count; ++i)
            {
                // Two seconds in all: the stage was at 1 at the release.
                const auto tau = releaseSeconds[i] / 3.0;
                worst = std::max (worst, std::abs (progress (m, i) - std::exp (-2.0 / tau)));
            }

            report (never && worst < 1.0e-12, "Processor MELTDOWN release returns every stage toward its knob",
                    worst, "worst error against exp(-t / tau) two seconds after the release");
        }

        // Nothing is left behind once it has settled, and a second press is the first again.
        {
            const auto trace = [] (Meltdown& m)
            {
                std::array<double, 4 * count> seen {};
                m.setGate (true);
                for (int k = 0; k < 4; ++k)
                {
                    m.advance (static_cast<int> (0.25 * sr));
                    for (std::size_t i = 0; i < count; ++i)
                        seen[static_cast<std::size_t> (k) * count + i] = m.progressOf (static_cast<Staged> (i));
                }
                return seen;
            };

            Meltdown fresh;
            fresh.prepare (sr);
            const auto first = trace (fresh);

            Meltdown used;
            used.prepare (sr);
            used.setGate (true);
            used.advance (static_cast<int> (3.0 * sr));
            used.setGate (false);
            used.advance (static_cast<int> (20.0 * sr));

            auto atKnob = ! used.active();
            for (std::size_t i = 0; i < count; ++i)
                atKnob = atKnob && progress (used, i) == 0.0
                         && used.value (static_cast<Staged> (i), knob) == knob;

            const auto again = trace (used);
            report (atKnob && again == first, "Processor MELTDOWN leaves no state behind after release",
                    atKnob ? 1.0 : 0.0, "(settled at the knobs, inactive, and the next press identical to the first)");
        }

        // Pressed again while still falling, it carries on from where it was.
        {
            Meltdown m;
            m.prepare (sr);
            m.setGate (true);
            m.advance (static_cast<int> (2.0 * sr));
            m.setGate (false);
            m.advance (static_cast<int> (0.5 * sr));

            std::array<double, count> before {};
            for (std::size_t i = 0; i < count; ++i)
                before[i] = progress (m, i);

            m.setGate (true);

            auto continuous = true;
            for (std::size_t i = 0; i < count; ++i)
                continuous = continuous && progress (m, i) == before[i];

            auto rising = true;
            for (int step = 0; step < 20; ++step)
            {
                m.advance (static_cast<int> (0.1 * sr));
                for (std::size_t i = 0; i < count; ++i)
                {
                    rising = rising && progress (m, i) >= before[i];
                    before[i] = progress (m, i);
                }
            }

            auto full = true;
            for (std::size_t i = 0; i < count; ++i)
                full = full && progress (m, i) == 1.0;

            report (continuous && rising && full,
                    "Processor MELTDOWN pressed mid-release carries on from where it was",
                    continuous ? 1.0 : 0.0, "(no jump at the press, never falling while held, all stages back at 1)");
        }
    }

    // Gestures are provoked, not recalled: a saved session with them on must reopen with them off.
    {
        SquelchAudioProcessor saved;
        for (const auto* id : { squelch::ids::ionize, squelch::ids::meltdown, squelch::ids::clip })
            setParameter (saved, id, 1.0f);

        juce::MemoryBlock block;
        saved.getStateInformation (block);

        SquelchAudioProcessor restored;
        restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

        double stillOn = 0.0;
        for (const auto* id : { squelch::ids::ionize, squelch::ids::meltdown, squelch::ids::clip })
            stillOn += restored.apvts.getRawParameterValue (id)->load() > 0.5f ? 1.0 : 0.0;

        report (stillOn == 0.0, "Processor gestures come back off from a saved state", stillOn,
                "of 3 still on (needs 0)");
    }

    std::printf ("%s\n", failures == 0 ? "PROCESSOR TESTS: PASS" : "PROCESSOR TESTS: FAIL");
    return failures == 0 ? 0 : 1;
}
