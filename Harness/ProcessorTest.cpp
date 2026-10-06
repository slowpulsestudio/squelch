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
#include <functional>
#include <memory>
#include <set>
#include <string>
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

    // A fault that crashes the run must not take the verdicts already reached with it.
    std::fflush (stdout);
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

/** What a host's transport says at the start of a block. */
struct TransportState
{
    bool playing { false };
    double bpm { 120.0 };
    std::int64_t timeSamples { 0 };
    bool hasPpq { true };
    double ppqOverride { -1.0 };
    bool valid { true };
    bool looping { false };
    double loopStartPpq { 0.0 }, loopEndPpq { 0.0 };
};

/** A host transport driven by a script from the host's sample counter, so a host that
    reports a frozen position, one that loops and one that seeks are all a function. */
class ScriptedTransport : public juce::AudioPlayHead
{
public:
    explicit ScriptedTransport (std::function<TransportState (std::int64_t)> f) : script (std::move (f)) {}

    void setBlockStart (std::int64_t sample) { blockStart = sample; }

    juce::Optional<PositionInfo> getPosition() const override
    {
        const auto s = script (blockStart);

        if (! s.valid)
            return {};

        PositionInfo info;
        info.setBpm (s.bpm);
        info.setTimeInSamples (s.timeSamples);
        info.setIsPlaying (s.playing);

        if (s.hasPpq)
            info.setPpqPosition (s.ppqOverride >= 0.0 ? s.ppqOverride
                                                      : static_cast<double> (s.timeSamples) * s.bpm / (60.0 * sampleRate));

        if (s.looping)
        {
            info.setIsLooping (true);
            info.setLoopPoints (juce::AudioPlayHead::LoopPoints { s.loopStartPpq, s.loopEndPpq });
        }

        return info;
    }

private:
    std::function<TransportState (std::int64_t)> script;
    std::int64_t blockStart { 0 };
};

/** A longer render with knobs set and MELTDOWN held for a stretch from the start. */
struct Session
{
    int reaction { 0 };
    double seconds { 2.0 };
    int block { blockSize };

    /// MELTDOWN is held for this long from the first sample; negative never presses it.
    double meltdownSeconds { -1.0 };

    /// The host's transport, or none.
    const std::function<TransportState (std::int64_t)>* transport { nullptr };

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

    std::unique_ptr<ScriptedTransport> transport;

    if (s.transport != nullptr)
    {
        transport = std::make_unique<ScriptedTransport> (*s.transport);
        processor.setPlayHead (transport.get());
    }

    const auto total = static_cast<int> (s.seconds * sampleRate);
    std::vector<float> out;
    juce::MidiBuffer midi;
    std::uint32_t state = 12345u;

    for (int done = 0; done < total; done += s.block)
    {
        if (transport != nullptr)
            transport->setBlockStart (done);
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

/** A sine through the dry path alone: mix is 0, so what comes out is the delayed input
    after nothing but the output stage's ceiling, and CLIP is what chooses it. */
std::vector<float> renderCeiling (double amplitude, bool clip, int block = blockSize, double seconds = 1.0)
{
    SquelchAudioProcessor processor;
    processor.mix = 0.0f;
    setParameter (processor, squelch::ids::clip, clip ? 1.0f : 0.0f);
    processor.prepareToPlay (sampleRate, block);

    const auto total = static_cast<int> (seconds * sampleRate);
    std::vector<float> out;
    juce::MidiBuffer midi;

    for (int done = 0; done < total; done += block)
    {
        juce::AudioBuffer<float> buffer (2, block);

        for (int i = 0; i < block; ++i)
        {
            const auto v = static_cast<float> (amplitude * std::sin (2.0 * M_PI * 220.0 * (done + i) / sampleRate));
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        processor.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + block);
    }

    out.resize (static_cast<size_t> (total));
    return out;
}

/// Fraction of samples sitting on the ceiling, and the largest magnitude, after a settling period.
struct CeilingStats { double onCeiling, peak; };

CeilingStats ceilingStats (const std::vector<float>& x)
{
    const auto from = static_cast<size_t> (0.3 * sampleRate);
    size_t on = 0;
    double peak = 0.0;

    for (auto i = from; i < x.size(); ++i)
    {
        const auto m = std::abs (static_cast<double> (x[i]));
        peak = std::max (peak, m);
        on += m >= 0.9699 ? 1u : 0u;
    }

    return { static_cast<double> (on) / static_cast<double> (x.size() - from), peak };
}

/** ALIEN from silence at full probability makes sound only on sequencer events, so the
    output is a direct record of when the sequencer fired. */
std::vector<float> renderEvents (const std::function<TransportState (std::int64_t)>* script, int block,
                                 double seconds = 6.0, int reaction = 4,
                                 std::vector<std::pair<const char*, float>> knobs = {})
{
    SquelchAudioProcessor processor;

    const auto* reactionParameter = processor.apvts.getParameter (squelch::ids::reaction);
    setParameter (processor, squelch::ids::reaction, reactionParameter->convertTo0to1 (static_cast<float> (reaction)));
    setParameter (processor, squelch::ids::probability, 1.0f);

    for (const auto& knob : knobs)
        setParameter (processor, knob.first, knob.second);

    processor.prepareToPlay (sampleRate, block);

    std::unique_ptr<ScriptedTransport> transport;

    if (script != nullptr)
    {
        transport = std::make_unique<ScriptedTransport> (*script);
        processor.setPlayHead (transport.get());
    }

    const auto total = static_cast<int> (seconds * sampleRate);
    std::vector<float> out;
    juce::MidiBuffer midi;

    for (int done = 0; done < total; done += block)
    {
        if (transport != nullptr)
            transport->setBlockStart (done);

        juce::AudioBuffer<float> buffer (2, block);
        buffer.clear();
        processor.processBlock (buffer, midi);
        out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + block);
    }

    out.resize (static_cast<size_t> (total));
    return out;
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

    // CLIP replaces the limiter's ceiling with a hard one, at the end of the chain. The
    // dry path alone is used (mix 0), so the only thing between input and output is the
    // ceiling, and the signal is well over it.
    {
        const auto limited = renderCeiling (1.5, false);
        const auto clipped = renderCeiling (1.5, true);
        const auto soft = ceilingStats (limited);
        const auto hard = ceilingStats (clipped);

        report (soft.peak <= 0.97 + 1e-6 && soft.onCeiling < 0.05,
                "Processor CLIP off leaves the signal unclipped", soft.onCeiling,
                "of samples on the ceiling over a 1.5 peak sine (needs under 0.05, peak at most 0.97)");

        report (hard.peak <= 0.97 + 1e-6 && hard.onCeiling > 0.30,
                "Processor CLIP on clips it hard at the ceiling", hard.onCeiling,
                "of samples on the ceiling over the same sine (needs over 0.30, peak at most 0.97)");

        // More over-threshold signal is more clipping.
        const auto mild = ceilingStats (renderCeiling (1.1, true)).onCeiling;
        const auto strong = ceilingStats (renderCeiling (3.0, true)).onCeiling;
        report (mild < hard.onCeiling && hard.onCeiling < strong && mild > 0.0,
                "Processor CLIP clips more as the signal rises over the ceiling", strong,
                "of samples on the ceiling at 3.0, against the 1.5 and 1.1 figures, rising");
    }

    {
        // Under the ceiling the two are the same signal, aligned to the sample: a latency
        // that differed by even one sample would show at 220 Hz.
        const auto limited = renderCeiling (0.5, false);
        const auto clipped = renderCeiling (0.5, true);
        const auto error = db (rms (difference (clipped, limited)) / rms (limited));
        report (error < -50.0, "Processor CLIP and the limiter are aligned and transparent below the ceiling",
                error, "dB between them on a 0.5 sine (needs < -50)");
    }

    // TRANSPORT. The sequencer is on the host's musical time while the host is playing
    // and on its own clock otherwise. Every check is against the host's block size:
    // events placed in musical time are the same whatever it is, and a sequencer that
    // reads a frozen position as a new one every block, or rounds a loop or a seek to
    // the block edge, is not.
    {
        using Script = std::function<TransportState (std::int64_t)>;

        const int sizes[] { 128, 512, 2048 };
        const auto total = 8.0;

        const auto relativeDb = [] (const std::vector<float>& a, const std::vector<float>& b, double fromSeconds)
        {
            const auto from = static_cast<std::ptrdiff_t> (fromSeconds * sampleRate);
            const std::vector<float> ta (a.begin() + from, a.end()), tb (b.begin() + from, b.end());
            return db (rms (difference (ta, tb)) / std::max (rms (tb), 1.0e-9));
        };

        // Worst dB between a render at each block size and the reference (the 512 render
        // when there is none).
        const auto acrossBlocks = [&] (const std::function<Script (int)>& scriptFor,
                                       const std::vector<float>* reference)
        {
            std::vector<std::vector<float>> renders;

            for (const auto bs : sizes)
            {
                const auto script = scriptFor (bs);
                renders.push_back (renderEvents (script ? &script : nullptr, bs, total));
            }

            auto worst = -300.0;
            for (size_t i = 0; i < renders.size(); ++i)
                if (reference != nullptr || i != 1)
                    worst = std::max (worst, relativeDb (renders[i], reference != nullptr ? *reference : renders[1], 0.0));

            return std::pair { worst, renders[1] };
        };

        const auto same = [&] (const char* name, const char* detail, const std::function<Script (int)>& scriptFor,
                               const std::vector<float>& reference)
        {
            const auto result = acrossBlocks (scriptFor, &reference);
            report (result.first < -100.0, name, result.first, detail);
        };

        const auto constant = [] (Script s) { return [s] (int) { return s; }; };

        // The fallback: no transport at all. This is also the free-running sequencer
        // that every other case is measured against.
        const auto fallback = acrossBlocks ([] (int) { return Script {}; }, nullptr);
        report (fallback.first < -100.0, "Processor sequencer without a transport does not depend on the block size",
                fallback.first, "dB between 128, 512 and 2048 sample blocks (needs < -100)");
        const auto& freeRun = fallback.second;
        report (rms (freeRun) > 1.0e-3, "Processor sequencer probe makes sound from events", rms (freeRun),
                "rms (needs > 0.001; a silent probe would pass every comparison below)");

        same ("Processor sequencer treats a host with no position as the free run",
              "dB from the free-running render, at 128, 512 and 2048 (needs < -100)",
              constant ([] (std::int64_t) { TransportState s; s.valid = false; return s; }), freeRun);

        // A stopped host's position does not advance. Read as a new position every block
        // it fires the same events at the block rate; it must leave the free run alone.
        same ("Processor sequencer ignores a stopped host's position",
              "dB from the free-running render, at 128, 512 and 2048 (needs < -100)",
              constant ([] (std::int64_t) { TransportState s; s.timeSamples = 0; return s; }), freeRun);
        same ("Processor sequencer ignores a stopped host wherever it is parked",
              "dB from the free-running render (needs < -100)",
              constant ([] (std::int64_t) { TransportState s; s.timeSamples = 123456; return s; }), freeRun);

        // Playing, the host's position is the timeline. From zero at 120 that is the free run
        // whichever of its two clocks says so.
        same ("Processor sequencer follows a playing host in musical time",
              "dB from the free-running render, at 128, 512 and 2048 (needs < -100)",
              constant ([] (std::int64_t d) { TransportState s; s.playing = true; s.timeSamples = d; return s; }), freeRun);
        same ("Processor sequencer follows the host's PPQ before its sample counter",
              "dB from the free-running render with a counter stuck at zero (needs < -100)",
              constant ([] (std::int64_t d) {
                  TransportState s; s.playing = true; s.timeSamples = 0;
                  s.ppqOverride = static_cast<double> (d) * 120.0 / (60.0 * sampleRate); return s; }), freeRun);
        same ("Processor sequencer falls back to the sample counter without a PPQ",
              "dB from the free-running render (needs < -100)",
              constant ([] (std::int64_t d) { TransportState s; s.playing = true; s.timeSamples = d; s.hasPpq = false; return s; }),
              freeRun);

        // Tempo: the grid is in beats, so the same beat position at another tempo is a
        // different sample. A host at 90 bpm is the free run at 90 bpm.
        {
            const auto at90 = acrossBlocks (constant ([] (std::int64_t) { TransportState s; s.bpm = 90.0; return s; }), nullptr);
            report (at90.first < -100.0, "Processor sequencer free-run at another tempo does not depend on the block size",
                    at90.first, "dB between block sizes (needs < -100)");
            report (relativeDb (at90.second, freeRun, 0.0) > -20.0, "Processor sequencer tempo moves the events",
                    relativeDb (at90.second, freeRun, 0.0), "dB, 90 bpm against 120 (needs > -20)");
            same ("Processor sequencer converts the host's PPQ at the host's tempo",
                  "dB from the free run at 90 bpm (needs < -100)",
                  constant ([] (std::int64_t d) {
                      TransportState s; s.playing = true; s.bpm = 90.0; s.timeSamples = d;
                      s.ppqOverride = static_cast<double> (d) * 90.0 / (60.0 * sampleRate); return s; }),
                  at90.second);
        }

        // Seek. A jump of the host's position repositions the sequencer to it, at the sample.
        // The reference is a host that was already at that place: past the tails of what
        // played before the jump the two are the same.
        const std::int64_t seekAt = 2048 * 64, seekTo = 70001;
        const Script seeks = [=] (std::int64_t d) {
            TransportState s; s.playing = true; s.timeSamples = d < seekAt ? d : seekTo + (d - seekAt); return s; };
        const Script alreadyThere = [=] (std::int64_t d) {
            TransportState s; s.playing = true; s.timeSamples = seekTo - seekAt + d; return s; };
        const Script straight = [] (std::int64_t d) { TransportState s; s.playing = true; s.timeSamples = d; return s; };

        {
            const auto jumped = acrossBlocks (constant (seeks), nullptr);
            const auto there = renderEvents (&alreadyThere, 512, total);
            const auto straightOn = renderEvents (&straight, 512, total);
            const auto settled = static_cast<double> (seekAt) / sampleRate + 2.0;

            report (jumped.first < -100.0, "Processor sequencer seek does not depend on the block size", jumped.first,
                    "dB between 128, 512 and 2048 (needs < -100)");
            report (relativeDb (jumped.second, there, settled) < -80.0, "Processor sequencer seek lands where the host put it",
                    relativeDb (jumped.second, there, settled), "dB from a host already there, after the tails (needs < -80)");
            report (relativeDb (jumped.second, straightOn, settled) > -30.0, "Processor sequencer seek changes what plays",
                    relativeDb (jumped.second, straightOn, settled), "dB from a host that did not seek (needs > -30)");
        }

        // Loop. A host that loops reports the loop's start when a block crosses its end, so
        // the sequencer has to wrap inside the block. Block sizes that do not divide the
        // loop put the wrap mid-block.
        {
            const auto loopEndPpq = 3.9;
            const auto loopLength = static_cast<std::int64_t> (std::llround (loopEndPpq * 60.0 / 120.0 * sampleRate));

            const auto looping = [=] (int bs) -> Script
            {
                std::vector<std::int64_t> starts;
                std::int64_t position = 0;

                for (int done = 0; done < static_cast<int> (total * sampleRate) + bs; done += bs)
                {
                    starts.push_back (position);
                    position += bs;
                    if (position >= loopLength)
                        position -= loopLength;
                }

                return [=] (std::int64_t d)
                {
                    TransportState s;
                    s.playing = true;
                    s.timeSamples = starts[static_cast<size_t> (d / bs)];
                    s.looping = true;
                    s.loopStartPpq = 0.0;
                    s.loopEndPpq = loopEndPpq;
                    return s;
                };
            };

            // A block the length of the loop never crosses its end, so the host does all the
            // wrapping: that render is what every other block size has to equal.
            const auto idealScript = looping (static_cast<int> (loopLength));
            const auto ideal = renderEvents (&idealScript, static_cast<int> (loopLength), total);
            same ("Processor sequencer loop wraps at the sample, whatever the block size",
                  "dB from a host whose blocks end at the loop, at 128, 512 and 2048 (needs < -100)", looping, ideal);

            const auto looped = acrossBlocks (looping, nullptr);
            const auto straightOn = renderEvents (&straight, 512, total);
            report (looped.first < -100.0, "Processor sequencer loop does not depend on the block size", looped.first,
                    "dB between 128, 512 and 2048, with the wrap mid-block (needs < -100)");
            report (relativeDb (looped.second, straightOn, 0.0) > -20.0, "Processor sequencer loop returns to the loop start",
                    relativeDb (looped.second, straightOn, 0.0), "dB from a host that ran on past the end (needs > -20)");
        }

        // Stop and start. Stopped, the sequencer carries on from where it was; started, it
        // takes the host's position. The same session always gives the same render.
        {
            const std::int64_t stopAt = 2048 * 40, startAt = 2048 * 100, startFrom = 30011;
            const Script stops = [=] (std::int64_t d) {
                TransportState s;
                s.playing = d < stopAt || d >= startAt;
                s.timeSamples = d < stopAt ? d : d < startAt ? stopAt : startFrom + (d - startAt);
                return s; };
            // Stopped, the sequencer's own clock carries on from where the host stopped; it is
            // the host that has not moved. So the same host with the transport left playing
            // but the position made to jump at the restart is the reference, to the sample.
            const Script jumpsAtRestart = [=] (std::int64_t d) {
                TransportState s; s.playing = true; s.timeSamples = d < startAt ? d : startFrom + (d - startAt); return s; };

            const auto stopped = acrossBlocks (constant (stops), nullptr);
            const auto again = renderEvents (&stops, 512, total);
            const auto there = renderEvents (&jumpsAtRestart, 512, total);
            const auto straightOn = renderEvents (&straight, 512, total);
            const auto afterStart = static_cast<double> (startAt) / sampleRate;

            report (stopped.first < -100.0, "Processor sequencer stop and start do not depend on the block size", stopped.first,
                    "dB between 128, 512 and 2048 (needs < -100)");
            report (firstDifference (stopped.second, again) < 0, "Processor sequencer stop and start are deterministic",
                    static_cast<double> (firstDifference (stopped.second, again)), "first differing sample (needs -1)");
            report (firstDifference (stopped.second, there) < 0, "Processor sequencer carries on while stopped and takes the host's position on start",
                    static_cast<double> (firstDifference (stopped.second, there)),
                    "first sample differing from a host that only jumped (needs -1)");
            report (relativeDb (stopped.second, straightOn, afterStart) > -20.0, "Processor sequencer start moves the sequencer to the host's position",
                    relativeDb (stopped.second, straightOn, afterStart), "dB from a host that never moved, after the start (needs > -20)");
        }

        // The contamination ticks of RADIATION are driven by the same events, so they are the
        // thing a wrongly-read transport silences: under a stopped host reporting a frozen
        // position they must be there, whatever the seed.
        {
            const Script parked = [] (std::int64_t) { TransportState s; s.timeSamples = 0; return s; };
            auto worst = -300.0, weakest = 0.0;

            for (const auto seed : { 0.05f, 0.2f, 0.4f, 0.6f, 0.8f, 0.95f })
            {
                Session with, without;
                with.reaction = without.reaction = squelch::reactionNames.indexOf ("RADIATION");
                with.seconds = without.seconds = 3.0;
                with.transport = without.transport = &parked;
                with.knobs = { { squelch::ids::contamination, 1.0f }, { squelch::ids::seed, seed } };
                without.knobs = { { squelch::ids::contamination, 0.0f }, { squelch::ids::seed, seed } };

                const auto level = relativeDb (renderSession (with), renderSession (without), 0.0);
                weakest = std::min (weakest, level);
                worst = std::max (worst, level);
                std::printf ("    RADIATION seed %.2f ticks %.2f dB\n", seed, level);
            }

            report (weakest > -40.0, "Processor RADIATION ticks sound under a stopped host, for every seed", weakest,
                    "dB of contamination against none, weakest of six seeds (needs > -40)");
        }
    }

    // PRESETS and RANDOMISE, through the editor's own wiring: the callbacks are the ones the
    // toolbar's buttons invoke.
    {
        namespace presets = squelch::presets;

        const auto normalised = [] (SquelchAudioProcessor& p, const char* id) { return p.apvts.getParameter (id)->getValue(); };

        const auto countMoved = [&] (SquelchAudioProcessor& p, const std::vector<std::pair<juce::String, float>>& before)
        {
            auto moved = 0;
            for (const auto& b : before)
                if (std::abs (p.apvts.getParameter (b.first)->getValue() - b.second) > 1.0e-4f)
                    ++moved;
            return moved;
        };

        const auto snapshot = [] (SquelchAudioProcessor& p)
        {
            std::vector<std::pair<juce::String, float>> values;
            presets::forEachParameter (p.apvts, [&values] (juce::RangedAudioParameter& r) { values.emplace_back (r.paramID, r.getValue()); });
            return values;
        };

        {
            SquelchAudioProcessor processor;
            SquelchAudioProcessorEditor editor (processor);

            report (processor.presetIndex.load() == 0 && editor.getToolbar().getSelectedPreset() == 0
                        && presets::matches (processor.apvts, 0),
                    "Processor a fresh editor opens on the Default preset", processor.presetIndex.load(),
                    "preset index, with every parameter at its default (needs 0)");

            setParameter (processor, squelch::ids::spread, 0.1f);
            setParameter (processor, squelch::ids::reaction, 1.0f);
            setParameter (processor, squelch::ids::clip, 1.0f);
            setParameter (processor, squelch::ids::ionize, 1.0f);
            const auto changed = ! presets::matches (processor.apvts, 0) && editor.getToolbar().isDirty();
            report (changed, "Processor editing a parameter makes the preset dirty", changed ? 1.0 : 0.0, "dirty (needs 1)");

            editor.getToolbar().cyclePreset (1);
            const auto nextLoaded = presets::matches (processor.apvts, 1) && processor.presetIndex.load() == 1
                                 && ! editor.getToolbar().isDirty();
            editor.getToolbar().cyclePreset (-1);
            report (nextLoaded && presets::matches (processor.apvts, 0) && ! editor.getToolbar().isDirty()
                        && normalised (processor, squelch::ids::clip) == 1.0f
                        && normalised (processor, squelch::ids::ionize) == 1.0f,
                    "Processor the toolbar's next and previous buttons load presets and leave the gestures and CLIP alone",
                    std::abs (normalised (processor, squelch::ids::spread) - 0.75f), "spread distance from default (needs 0)");

            const auto before = snapshot (processor);
            auto worstMoved = 1000;
            auto clipTouched = false, meltdownTouched = false, ionizeTouched = false;
            std::set<float> reactionSeen, seedSeen;

            for (auto round = 0; round < 20; ++round)
            {
                setParameter (processor, squelch::ids::clip, 1.0f);
                setParameter (processor, squelch::ids::ionize, 1.0f);
                setParameter (processor, squelch::ids::ionizeAmount, 0.2f);
                editor.getToolbar().onRandomise();
                worstMoved = std::min (worstMoved, countMoved (processor, before));
                clipTouched = clipTouched || normalised (processor, squelch::ids::clip) != 1.0f;
                meltdownTouched = meltdownTouched || normalised (processor, squelch::ids::meltdown) != 0.0f;
                ionizeTouched = ionizeTouched || normalised (processor, squelch::ids::ionize) != 1.0f
                             || std::abs (normalised (processor, squelch::ids::ionizeAmount) - 0.2f) > 1.0e-4f;
                reactionSeen.insert (normalised (processor, squelch::ids::reaction));
                seedSeen.insert (normalised (processor, squelch::ids::seed));
            }

            const auto total = static_cast<int> (before.size());
            report (worstMoved >= total * 2 / 3, "Processor Randomise moves the parameters", worstMoved,
                    ("of " + std::to_string (total) + ", fewest in 20 presses (needs at least two thirds)").c_str());
            report (reactionSeen.size() >= 3 && seedSeen.size() >= 15, "Processor Randomise varies the reaction and the seed",
                    static_cast<double> (seedSeen.size()), "distinct seeds in 20 presses (needs 15), and 3 reactions");
            report (! clipTouched && ! meltdownTouched && ! ionizeTouched,
                    "Processor Randomise leaves the gestures and CLIP alone", clipTouched || ionizeTouched ? 1.0 : 0.0,
                    "touched (needs 0)");
            report (processor.presetIndex.load() == presets::randomised && ! editor.getToolbar().isDirty(),
                    "Processor Randomise deselects the preset", processor.presetIndex.load(), "preset index (needs -1)");

            auto strips = true;
            for (auto round = 0; round < 5; ++round)
            {
                processor.inputTrimDb = -3.0f;
                processor.outputTrimDb = 2.0f;
                processor.mix = 0.4f;
                editor.getToolbar().onRandomise();
                strips = strips && processor.inputTrimDb.load() == -3.0f && processor.outputTrimDb.load() == 2.0f
                                && processor.mix.load() == 0.4f;
            }
            report (strips, "Processor Randomise leaves the input and output strips alone", strips ? 1.0 : 0.0, "held (needs 1)");
        }

        // A session carries its preset, and a restored one is never taken for a fresh one.
        {
            SquelchAudioProcessor saved;
            setParameter (saved, squelch::ids::spread, 0.9f);
            juce::MemoryBlock block;
            saved.getStateInformation (block);

            SquelchAudioProcessor restored;
            restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
            SquelchAudioProcessorEditor editor (restored);

            report (std::abs (normalised (restored, squelch::ids::spread) - 0.9f) < 1.0e-4f && restored.presetIndex.load() == 0,
                    "Processor opening the editor on a restored session keeps its values",
                    normalised (restored, squelch::ids::spread), "spread (needs 0.9)");

            editor.getToolbar().onRandomise();
            juce::MemoryBlock after;
            restored.getStateInformation (after);
            SquelchAudioProcessor again;
            again.setStateInformation (after.getData(), static_cast<int> (after.getSize()));
            report (again.presetIndex.load() == presets::randomised, "Processor a session remembers that it was randomised",
                    again.presetIndex.load(), "preset index (needs -1)");
        }
    }

    // SHIPPING DEFAULTS. These, not prototype/params.py, are the product contract, and the
    // editor, the Default preset and a restored session all have to agree with them.
    {
        struct Expected { const char* id; float value; };
        const Expected expected[] {
            { squelch::ids::reaction, 0.0f },     { squelch::ids::mode, 0.0f },
            { squelch::ids::grid, 8.0f },         { squelch::ids::seed, 0.0f },
            { squelch::ids::enrichment, 0.5f },   { squelch::ids::flux, 0.0f },
            { squelch::ids::probability, 0.65f }, { squelch::ids::reactivity, 0.3f },
            { squelch::ids::volatility, 0.3f },   { squelch::ids::halfLife, 0.0f },
            { squelch::ids::decay, 0.3f },        { squelch::ids::spread, 0.75f },
            { squelch::ids::toxicity, 0.45f },    { squelch::ids::containment, 0.0f },
            { squelch::ids::drive, 0.3f },        { squelch::ids::contamination, 0.25f },
            { squelch::ids::exposure, 0.6f },     { squelch::ids::collimator, 0.0f },
            { squelch::ids::fallout, 0.3f },      { squelch::ids::afterglow, 0.0f },
            { squelch::ids::ionize, 0.0f },       { squelch::ids::ionizeAmount, 0.7f },
            { squelch::ids::meltdown, 0.0f },     { squelch::ids::clip, 0.0f },
        };

        const auto worstFrom = [&] (SquelchAudioProcessor& p)
        {
            auto worst = 0.0f;
            for (const auto& e : expected)
                worst = std::max (worst, std::abs (p.apvts.getRawParameterValue (e.id)->load() - e.value));
            return worst;
        };

        SquelchAudioProcessor fresh;
        const auto layout = worstFrom (fresh);

        SquelchAudioProcessor opened;
        SquelchAudioProcessorEditor editor (opened);
        const auto viaEditor = worstFrom (opened);

        juce::MemoryBlock block;
        fresh.getStateInformation (block);
        SquelchAudioProcessor restored;
        restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
        const auto viaState = worstFrom (restored);

        report (layout < 1.0e-6f, "Processor the parameter layout holds the shipping defaults", layout,
                "largest distance from the contract's values, 24 parameters (needs 0)");
        report (viaEditor < 1.0e-6f, "Processor the editor opens on the shipping defaults", viaEditor,
                "largest distance, after the Default preset loads (needs 0)");
        report (viaState < 1.0e-6f, "Processor a saved default session reloads as the shipping defaults", viaState,
                "largest distance after a save and reload (needs 0)");
    }

    // The preset table: thirteen presets, each of which loads, sounds and differs.
    {
        namespace presets = squelch::presets;

        SquelchAudioProcessor processor;
        auto covered = true, nothingElse = true;
        auto visited = 0;
        presets::forEachParameter (processor.apvts, [&] (juce::RangedAudioParameter& p)
        {
            ++visited;
            auto named = false;
            for (const auto* column : presets::columns)
                named = named || p.paramID == column;
            covered = covered && named;
        });
        for (const auto* column : presets::columns)
            nothingElse = nothingElse && processor.apvts.getParameter (column) != nullptr
                       && ! squelch::isExcludedFromPresets (column);

        report (covered && nothingElse && visited == static_cast<int> (std::size (presets::columns)),
                "Processor the preset table names exactly the parameters presets store", visited,
                "parameters stored (every one named, none excluded)");
        report (presets::names.size() == 13 && presets::names[0] == "Default"
                    && presets::names.size() == static_cast<int> (std::size (presets::rows)) + 1,
                "Processor there are twelve presets after Default", presets::names.size(), "entries (needs 13)");

        auto allLoad = true;
        auto outOfRange = false;
        for (int i = 0; i < presets::names.size(); ++i)
        {
            SquelchAudioProcessor fresh;
            setParameter (fresh, squelch::ids::ionizeAmount, 0.2f);
            setParameter (fresh, squelch::ids::clip, 1.0f);
            presets::apply (fresh.apvts, i);

            // Against the table itself, not the lookup that loaded it.
            for (size_t c = 0; i > 0 && c < std::size (presets::columns); ++c)
                allLoad = allLoad && std::abs (fresh.apvts.getParameter (presets::columns[c])->getValue()
                                               - presets::rows[static_cast<size_t> (i - 1)][c]) < 1.0e-4f;

            allLoad = allLoad && presets::matches (fresh.apvts, i)
                   && std::abs (fresh.apvts.getParameter (squelch::ids::ionizeAmount)->getValue() - 0.2f) < 1.0e-4f
                   && fresh.apvts.getParameter (squelch::ids::clip)->getValue() == 1.0f;
        }
        for (const auto& row : presets::rows)
            for (const auto v : row)
                outOfRange = outOfRange || v < 0.0f || v > 1.0f;
        report (allLoad && ! outOfRange, "Processor every preset loads and leaves the performance controls alone",
                allLoad ? 1.0 : 0.0, "loaded, with IONIZEAMOUNT and CLIP untouched (needs 1)");

        std::vector<std::vector<float>> renders;
        auto audible = true, finite = true;
        double quietest = 1.0e9;
        for (int i = 1; i < presets::names.size(); ++i)
        {
            Session s;
            s.seconds = 3.0;
            for (size_t c = 0; c < std::size (presets::columns); ++c)
            {
                if (std::string (presets::columns[c]) == squelch::ids::reaction)
                    s.reaction = juce::roundToInt (presets::rows[static_cast<size_t> (i - 1)][c] * 4.0f);

                s.knobs.emplace_back (presets::columns[c], presets::rows[static_cast<size_t> (i - 1)][c]);
            }

            renders.push_back (renderSession (s));
            const auto level = rms (renders.back());
            quietest = std::min (quietest, level);
            audible = audible && level > 1.0e-3;
            for (const auto v : renders.back())
                finite = finite && std::isfinite (v) && std::abs (v) <= 1.5f;
        }

        auto distinct = true;
        for (size_t a = 0; a < renders.size(); ++a)
            for (size_t b = a + 1; b < renders.size(); ++b)
                distinct = distinct && firstDifference (renders[a], renders[b]) >= 0;

        report (audible && finite, "Processor every preset makes sound and stays in range", quietest,
                "rms of the quietest, on noise (needs > 0.001, and finite)");
        report (distinct, "Processor the presets all render differently", distinct ? 1.0 : 0.0, "distinct (needs 1)");
    }

    // MONO. A mono source feeds both legs of the stereo path, so what comes out is what a
    // stereo track carrying the same signal in both channels would give.
    {
        const auto layoutOf = [] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
        {
            SquelchAudioProcessor::BusesLayout layout;
            layout.inputBuses.add (in);
            layout.outputBuses.add (out);
            return layout;
        };

        const auto mono = juce::AudioChannelSet::mono();
        const auto stereo = juce::AudioChannelSet::stereo();

        SquelchAudioProcessor probe;
        report (probe.isBusesLayoutSupported (layoutOf (mono, mono)) && probe.isBusesLayoutSupported (layoutOf (mono, stereo))
                    && probe.isBusesLayoutSupported (layoutOf (stereo, stereo)) && ! probe.isBusesLayoutSupported (layoutOf (stereo, mono)),
                "Processor accepts mono in, mono or stereo out, and stereo in and out", 1.0,
                "layouts (stereo in with mono out is not offered)");

        const auto renderLayout = [&] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
        {
            SquelchAudioProcessor processor;
            if (! processor.setBusesLayout (layoutOf (in, out)))
                return std::vector<std::vector<float>> (static_cast<size_t> (out.size()));

            processor.prepareToPlay (sampleRate, blockSize);

            const auto channels = juce::jmax (in.size(), out.size());
            const auto total = static_cast<int> (2.0 * sampleRate);
            std::vector<std::vector<float>> output (static_cast<size_t> (out.size()));
            juce::MidiBuffer midi;
            std::uint32_t state = 12345u;

            for (int done = 0; done < total; done += blockSize)
            {
                juce::AudioBuffer<float> buffer (channels, blockSize);
                buffer.clear();

                for (int i = 0; i < blockSize; ++i)
                {
                    state = state * 1664525u + 1013904223u;
                    const auto v = 0.25f * (static_cast<float> (state >> 8) / 8388608.0f - 1.0f);
                    for (int c = 0; c < in.size(); ++c)
                        buffer.setSample (c, i, v);
                }

                processor.processBlock (buffer, midi);

                for (int c = 0; c < out.size(); ++c)
                    output[static_cast<size_t> (c)].insert (output[static_cast<size_t> (c)].end(), buffer.getReadPointer (c),
                                                            buffer.getReadPointer (c) + blockSize);
            }

            return output;
        };

        const auto identical = [] (const std::vector<float>& a, const std::vector<float>& b)
        {
            return ! a.empty() && a.size() == b.size() && firstDifference (a, b) < 0;
        };

        const auto reference = renderLayout (stereo, stereo);
        const auto monoToStereo = renderLayout (mono, stereo);
        report (identical (monoToStereo[0], reference[0]) && identical (monoToStereo[1], reference[1]),
                "Processor mono in, stereo out is the stereo path fed the same signal twice",
                static_cast<double> (std::max (firstDifference (monoToStereo[0], reference[0]),
                                               firstDifference (monoToStereo[1], reference[1]))),
                "first sample differing from a dual-mono stereo track (needs -1, and a render)");

        const auto monoToMono = renderLayout (mono, mono);
        report (identical (monoToMono[0], reference[0]), "Processor mono in, mono out is the stereo path's left leg",
                static_cast<double> (firstDifference (monoToMono[0], reference[0])),
                "first sample differing from a dual-mono stereo track (needs -1, and a render)");
        report (rms (reference[0]) > 1.0e-3 && rms (difference (reference[0], reference[1])) > 1.0e-4,
                "Processor mono probe is audible and has a stereo image", rms (difference (reference[0], reference[1])),
                "rms of left minus right (needs > 0.0001), so the left leg is not the whole story");
    }

    // MELTDOWN is held, so a saved session with it down must reopen with it up. IONIZE and
    // CLIP are performances and settings, and must come back as they were saved.
    {
        SquelchAudioProcessor saved;
        for (const auto* id : { squelch::ids::ionize, squelch::ids::meltdown, squelch::ids::clip })
            setParameter (saved, id, 1.0f);

        juce::MemoryBlock block;
        saved.getStateInformation (block);

        SquelchAudioProcessor restored;
        restored.setStateInformation (block.getData(), static_cast<int> (block.getSize()));

        const auto meltdownBack = restored.apvts.getRawParameterValue (squelch::ids::meltdown)->load();
        report (meltdownBack < 0.5f, "Processor MELTDOWN comes back released from a saved state", meltdownBack,
                "restored value (needs 0)");

        const auto ionizeBack = restored.apvts.getRawParameterValue (squelch::ids::ionize)->load();
        report (ionizeBack > 0.5f, "Processor IONIZE survives a reload engaged", ionizeBack, "restored value (needs 1)");
        const auto clipBack = restored.apvts.getRawParameterValue (squelch::ids::clip)->load();
        report (clipBack > 0.5f, "Processor CLIP survives a reload", clipBack, "restored value (needs 1)");

        SquelchAudioProcessor off;
        juce::MemoryBlock offBlock;
        off.getStateInformation (offBlock);
        SquelchAudioProcessor offRestored;
        offRestored.setStateInformation (offBlock.getData(), static_cast<int> (offBlock.getSize()));
        const auto stillOff = offRestored.apvts.getRawParameterValue (squelch::ids::clip)->load();
        report (stillOff < 0.5f, "Processor CLIP saved off reloads off", stillOff, "restored value (needs 0)");
    }

    std::printf ("%s\n", failures == 0 ? "PROCESSOR TESTS: PASS" : "PROCESSOR TESTS: FAIL");
    return failures == 0 ? 0 : 1;
}
