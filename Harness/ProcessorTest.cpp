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
#include "../Source/PluginProcessor.h"
#include "../Source/Dsp/Meltdown.h"

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
